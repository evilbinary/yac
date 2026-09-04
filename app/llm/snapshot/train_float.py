#!/usr/bin/env python3
"""Snapshot LM: bidirectional Transformer + MaskGIT shutter.

No causal mask. Loss only on MASK holes. See DESIGN.md.

  python app/llm/snapshot/train_float.py [corpus] [epochs] [new]
  python app/llm/snapshot/train_float.py infer [ckpt] [prompt] [K] [chat]
"""
from __future__ import annotations

import os
import random
import sys
import time
from collections import Counter

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

T = 64
D = 192
N_LAYER = 4
N_HEAD = 6
FF = 768
DROP = 0.05
BATCH = 96
STRIDE = 64
VOCAB_MAX = 4096
MASK_P = 0.15
LR = 6e-4
WD = 0.01
FREEZE_N = 8
GEN_TEMP = 1.0
GEN_REP = 1.5
MASK_CH = "[MASK]"
UNK_CH = "[UNK]"
PAD_CH = "[PAD]"
CKPT = "app/llm/snapshot/snapshot_bert.pt"


def chars_of(s: str):
    for c in s:
        if c == "\r":
            continue
        yield c


def build_vocab(text: str, cap: int) -> list[str]:
    freq = Counter(chars_of(text))
    # reserve PAD UNK MASK
    n = max(8, cap - 3)
    top = [c for c, k in freq.most_common(n) if k >= 2]
    return [PAD_CH, UNK_CH, MASK_CH] + top


def encode(text: str, stoi: dict[str, int]) -> np.ndarray:
    unk = stoi[UNK_CH]
    ids = []
    for c in chars_of(text):
        ids.append(stoi.get(c, unk))
    return np.array(ids, dtype=np.int32)


def windows(ids: np.ndarray, t: int, stride: int) -> np.ndarray:
    n = len(ids)
    if n < t:
        pad = np.full(t - n, 0, dtype=np.int32)
        return np.stack([np.concatenate([ids, pad])])
    pos = list(range(0, n - t + 1, stride))
    if pos[-1] != n - t:
        pos.append(n - t)
    return np.stack([ids[p : p + t] for p in pos])


class Block(nn.Module):
    def __init__(self, d: int, n_head: int, ff: int, drop: float):
        super().__init__()
        self.n_head = n_head
        self.dh = d // n_head
        self.drop_p = drop
        self.ln1 = nn.LayerNorm(d)
        self.qkv = nn.Linear(d, 3 * d)
        self.wo = nn.Linear(d, d)
        self.ln2 = nn.LayerNorm(d)
        self.mlp = nn.Sequential(
            nn.Linear(d, ff),
            nn.GELU(),
            nn.Dropout(drop),
            nn.Linear(ff, d),
            nn.Dropout(drop),
        )
        # bidirectional ALiBi: nearer keys preferred, so two MASK slots
        # see different neighbors even when token embeddings are identical
        slopes = 2 ** (-8.0 * torch.arange(1, n_head + 1) / n_head)
        self.register_buffer("slopes", slopes, persistent=False)
        i = torch.arange(T)
        dist = (i[:, None] - i[None, :]).abs().float()
        self.register_buffer("alibi", -(slopes[:, None, None] * dist), persistent=False)

    def _bias(self, t: int, key_pad: torch.Tensor | None, dtype: torch.dtype):
        bias = self.alibi[:, :t, :t].to(dtype)
        if key_pad is None:
            return bias
        return bias.unsqueeze(0).masked_fill(key_pad[:, None, None, :], torch.finfo(dtype).min)

    def forward(self, x: torch.Tensor, key_pad: torch.Tensor | None):
        b, t, d = x.shape
        h = self.ln1(x)
        qkv = self.qkv(h).view(b, t, 3, self.n_head, self.dh).permute(2, 0, 3, 1, 4)
        q, k, v = qkv.unbind(0)
        a = F.scaled_dot_product_attention(
            q, k, v, attn_mask=self._bias(t, key_pad, q.dtype),
            dropout_p=self.drop_p if self.training else 0.0,
        )
        a = self.wo(a.transpose(1, 2).contiguous().view(b, t, d))
        x = x + a
        x = x + self.mlp(self.ln2(x))
        return x


class SnapshotLM(nn.Module):
    def __init__(self, vocab: list[str]):
        super().__init__()
        self.vocab = vocab
        self.V = len(vocab)
        self.pad = vocab.index(PAD_CH)
        self.unk = vocab.index(UNK_CH)
        self.mask = vocab.index(MASK_CH)
        self.tok = nn.Embedding(self.V, D, padding_idx=self.pad)
        self.pos = nn.Embedding(T, D)
        self.drop = nn.Dropout(DROP)
        self.blocks = nn.ModuleList([Block(D, N_HEAD, FF, DROP) for _ in range(N_LAYER)])
        self.ln = nn.LayerNorm(D)
        self.register_buffer("pos_ids", torch.arange(T), persistent=False)
        self._init_w()

    def _init_w(self):
        nn.init.normal_(self.tok.weight, std=0.02)
        self.tok.weight.data[self.pad].zero_()
        nn.init.normal_(self.pos.weight, std=0.02)
        for m in self.modules():
            if isinstance(m, nn.Linear):
                nn.init.normal_(m.weight, std=0.02)
                if m.bias is not None:
                    nn.init.zeros_(m.bias)

    def encode(self, ids: torch.Tensor) -> torch.Tensor:
        b, t = ids.shape
        x = self.drop(self.tok(ids) + self.pos(self.pos_ids[:t].unsqueeze(0).expand(b, t)))
        key_pad = ids == self.pad
        if not bool(key_pad.any()):
            key_pad = None
        for blk in self.blocks:
            x = blk(x, key_pad)
        return self.ln(x)

    def logits_pick(self, ids: torch.Tensor, pick: torch.Tensor) -> torch.Tensor:
        return F.linear(self.encode(ids)[pick], self.tok.weight)

    def forward(self, ids: torch.Tensor) -> torch.Tensor:
        return F.linear(self.encode(ids), self.tok.weight)


def _span_pick(valid: torch.Tensor, ratio: torch.Tensor) -> torch.Tensor:
    """Start at ~ratio/2.5 of positions, cover 2–4 tokens (phrase-sized holes)."""
    b, t = valid.shape
    p_start = (ratio / 2.5).clamp(0.04, 0.35)
    starts = (torch.rand(b, t, device=valid.device) < p_start) & valid
    pick = starts.clone()
    for k in range(1, 4):
        pick[:, k:] |= starts[:, : t - k]
    return pick & valid


def mask_tokens(gold: torch.Tensor, pad: int, mask_id: int, p: float | None):
    """MASK holes only. Train mixes cloze / high-r / prefix-frozen continuation."""
    b, t = gold.shape
    valid = gold != pad
    if p is not None:
        pick = (torch.rand(b, t, device=gold.device) < p) & valid
        canvas = gold.clone()
        canvas[pick] = mask_id
        return canvas, pick

    device = gold.device
    u = torch.rand(b, 1, device=device)
    is_cont = u < 0.35
    is_high = (u >= 0.35) & (u < 0.70)
    r_light = torch.rand(b, 1, device=device) * 0.23 + 0.12
    r_high = torch.rand(b, 1, device=device) * 0.35 + 0.50
    ratio = torch.where(is_high, r_high, r_light)
    pick_ind = (torch.rand(b, t, device=device) < ratio) & valid
    pick_span = _span_pick(valid, ratio)
    use_span = (~is_cont) & (torch.rand(b, 1, device=device) < 0.5)
    pick = torch.where(use_span.expand(b, t), pick_span, pick_ind)
    freeze = torch.randint(4, FREEZE_N + 1, (b, 1), device=device)
    left = torch.arange(t, device=device).view(1, t) < freeze
    pick_cont = valid & ~left
    pick = torch.where(is_cont.expand(b, t), pick_cont, pick)

    canvas = gold.clone()
    canvas[pick] = mask_id
    return canvas, pick


def run_epoch(model, opts, wins: torch.Tensor, train: bool, sched=None):
    model.train(train)
    nwin = wins.size(0)
    order = torch.randperm(nwin) if train else torch.arange(nwin)
    n_batch = max(1, (nwin + BATCH - 1) // BATCH)
    hit = n = 0
    nll_sum = 0.0
    n_steps = 0
    vc = torch.zeros(model.V, dtype=torch.long, device=wins.device)
    ctx = torch.enable_grad() if train else torch.inference_mode()
    with ctx:
        for i0 in range(0, nwin, BATCH):
            gold = wins[order[i0 : i0 + BATCH]]
            p = None if train else MASK_P
            canvas, pick = mask_tokens(gold, model.pad, model.mask, p)
            if not pick.any():
                continue
            logits = model.logits_pick(canvas, pick)
            y = gold[pick]
            loss = F.cross_entropy(logits, y)
            if train:
                opts.zero_grad(set_to_none=True)
                loss.backward()
                opts.step()
                if sched is not None:
                    sched.step()
            pred = logits.argmax(-1)
            hit += int((pred == y).sum())
            n += int(y.numel())
            vc.index_add_(0, y, torch.ones_like(y))
            nll_sum += float(loss.detach())
            n_steps += 1
            if train and n_steps % 50 == 0:
                print(
                    f"  step {n_steps}/{n_batch}  "
                    f"mask-acc ~{100 * hit // max(n, 1)}%  nll {nll_sum / n_steps:.3f}",
                    flush=True,
                )
    acc = 100 * hit // n if n else 0
    nll = nll_sum / max(n_steps, 1)
    maj = 100 * int(vc.max()) // n if n else 0
    return acc, nll, hit, n, maj


@torch.inference_mode()
def eval_rate(model, wins: torch.Tensor, p: float, max_win: int = 128) -> int:
    """MASK acc at a fixed hole rate (generate-like check)."""
    gold = wins[: min(max_win, wins.size(0))]
    hit = n = 0
    for i0 in range(0, gold.size(0), BATCH):
        g = gold[i0 : i0 + BATCH]
        canvas, pick = mask_tokens(g, model.pad, model.mask, p)
        if not pick.any():
            continue
        pred = model.logits_pick(canvas, pick).argmax(-1)
        hit += int((pred == g[pick]).sum())
        n += int(g[pick].numel())
    return 100 * hit // n if n else 0


@torch.inference_mode()
def eval_cont(model, wins: torch.Tensor, freeze: int = 8, max_win: int = 128) -> int:
    """Infer-like: keep left freeze chars, MASK the rest."""
    gold = wins[: min(max_win, wins.size(0))]
    hit = n = 0
    for i0 in range(0, gold.size(0), BATCH):
        g = gold[i0 : i0 + BATCH]
        pick = (g != model.pad)
        pick[:, :freeze] = False
        if not pick.any():
            continue
        canvas = g.clone()
        canvas[pick] = model.mask
        pred = model.logits_pick(canvas, pick).argmax(-1)
        hit += int((pred == g[pick]).sum())
        n += int(g[pick].numel())
    return 100 * hit // n if n else 0


@torch.no_grad()
def probe(model, wins, device) -> tuple[int, int]:
    model.eval()
    gold = wins[: min(32, len(wins))]
    canvas, pick = mask_tokens(gold, model.pad, model.mask, 0.5)
    a = model(canvas).argmax(-1)
    vis = (canvas != model.mask) & (canvas != model.pad)
    scramble = canvas.clone()
    nvis = int(vis.sum().item())
    if nvis:
        scramble[vis] = torch.randint(3, model.V, (nvis,), device=device)
    b = model(scramble).argmax(-1)
    holes = canvas == model.mask
    changed = int(((a != b) & holes).sum().item())
    nh = int(holes.sum().item())
    return changed, nh


def prompt_glyphs(prompt: str) -> list[str]:
    return [c for c in prompt if c != "\r"][-FREEZE_N:]


def encode_prompt(prompt: str, stoi: dict[str, int]) -> list[int]:
    unk = stoi[UNK_CH]
    ids = [stoi.get(c, unk) for c in prompt if c != "\r"]
    return ids[-FREEZE_N:]


def n_freeze(prompt: str) -> int:
    n = len(prompt_glyphs(prompt))
    return min(n, T - 1)


def show_tok(model, tok: int) -> str:
    c = model.vocab[tok]
    if c == MASK_CH:
        return "░"
    if c == PAD_CH:
        return " "
    if c == "\n":
        return "↵"
    return c


def show_canvas(model, ids: list[int], glyphs: list[str] | None = None, start: int = 0) -> str:
    out = []
    for i, tok in enumerate(ids[start:]):
        pos = start + i
        if glyphs is not None and pos < len(glyphs):
            out.append(glyphs[pos])
        else:
            out.append(show_tok(model, tok))
    return "".join(out)


def _gen_logits(model, canvas: torch.Tensor) -> torch.Tensor:
    """One shutter: logits from the current film only. Temp/rep on this tensor."""
    logits = model(canvas)[0] / GEN_TEMP
    logits[:, model.pad] = -1e9
    logits[:, model.mask] = -1e9
    vis = canvas[0]
    vis = vis[(vis != model.mask) & (vis != model.pad)]
    if vis.numel():
        counts = torch.bincount(vis, minlength=model.V).to(logits.dtype)
        logits = logits - GEN_REP * counts
    return logits


def blank_canvas(prompt: str, stoi: dict[str, int], model, device) -> torch.Tensor:
    ids = encode_prompt(prompt, stoi)
    canvas = torch.full((1, T), model.mask, dtype=torch.long, device=device)
    n = min(len(ids), T - 1)
    if n:
        canvas[0, :n] = torch.tensor(ids[:n], device=device)
    return canvas


@torch.no_grad()
def denoise_ids(model, prompt: str, stoi: dict[str, int], k: int, device, frames: bool = False):
    """MaskGIT: each step one forward, then commit m holes at once."""
    model.eval()
    canvas = blank_canvas(prompt, stoi, model, device)
    k = max(1, k)
    snaps = [canvas[0].clone()] if frames else None
    for step in range(k):
        left = k - step
        holes = (canvas == model.mask)[0]
        nm = int(holes.sum().item())
        if nm <= 0:
            break
        logits = _gen_logits(model, canvas)
        pred = logits.argmax(-1)
        conf = F.softmax(logits, dim=-1)[torch.arange(T, device=device), pred]
        conf = torch.where(holes, conf, torch.zeros_like(conf))
        ntake = nm if left == 1 else max(1, (nm + left - 1) // left)
        top = torch.topk(conf, k=min(ntake, nm)).indices
        canvas[0, top] = pred[top]
        if snaps is not None:
            snaps.append(canvas[0].clone())
    out = canvas[0]
    return (out, snaps) if frames else out


def denoise(model, prompt: str, stoi: dict[str, int], k: int, device) -> str:
    ids = denoise_ids(model, prompt, stoi, k, device).tolist()
    return show_canvas(model, ids, prompt_glyphs(prompt))


def print_frames(model, prompt: str, stoi: dict[str, int], k: int, device):
    glyphs = prompt_glyphs(prompt)
    ids, snaps = denoise_ids(model, prompt, stoi, k, device, frames=True)
    n = len(snaps)
    mid = min(1 + (n - 1) // 2, n - 1) if n > 2 else 0
    print(f"step 0  {show_canvas(model, snaps[0].tolist(), glyphs)}", flush=True)
    if n > 2 and mid not in (0, n - 1):
        print(f"step {mid}  {show_canvas(model, snaps[mid].tolist(), glyphs)}", flush=True)
    print(f"final   {show_canvas(model, ids.tolist(), glyphs)}", flush=True)


def print_k_curve(model, stoi: dict[str, int], device):
    print("--- generate (prefix frozen, fill MASK) ---", flush=True)
    print(f"K=8 悟空  {denoise(model, '悟空', stoi, 8, device)}", flush=True)
    print(f"K=8 猴    {denoise(model, '猴', stoi, 8, device)}", flush=True)


def save_ckpt(path: str, model, vocab: list[str], opts=None, sched=None, ep=0):
    blob = {"model": model.state_dict(), "vocab": vocab, "cfg": {"T": T, "D": D}, "ep": ep}
    if opts is not None:
        blob["opt"] = opts.state_dict()
    if sched is not None:
        blob["sched"] = sched.state_dict()
    torch.save(blob, path)


def _load(path, device):
    try:
        return torch.load(path, map_location=device, weights_only=False)
    except TypeError:
        return torch.load(path, map_location=device)


def load_ckpt(path: str, device):
    blob = _load(path, device)
    vocab = blob["vocab"]
    model = SnapshotLM(vocab).to(device)
    model.load_state_dict(blob["model"])
    model.eval()
    return model, {c: i for i, c in enumerate(vocab)}


def setup_device():
    global BATCH
    if torch.cuda.is_available():
        torch.backends.cudnn.benchmark = True
        BATCH = 256
        print(f"device  cuda  {torch.cuda.get_device_name(0)}  B={BATCH}", flush=True)
        return torch.device("cuda")
    nt = os.cpu_count() or 4
    torch.set_num_threads(nt)
    torch.set_num_interop_threads(1)
    if hasattr(torch.backends, "mkldnn"):
        torch.backends.mkldnn.enabled = True
    print(f"device  cpu  B={BATCH}", flush=True)
    return torch.device("cpu")


def chat_once(model, prompt: str, stoi: dict[str, int], k: int, device):
    ids = denoise_ids(model, prompt, stoi, k, device).tolist()
    u = n_freeze(prompt)
    print(f"reply  {show_canvas(model, ids, start=u)}", flush=True)


def infer_repl(model, stoi: dict[str, int], k: int, device, chat: bool):
    print("you> ", end="", flush=True)
    while True:
        line = sys.stdin.readline()
        if not line:
            break
        line = line.rstrip("\n\r")
        if line == "" or line in ("quit", "exit"):
            break
        if chat:
            chat_once(model, line, stoi, k, device)
        else:
            print(f"final  {denoise(model, line, stoi, k, device)}", flush=True)
        print("you> ", end="", flush=True)


def cmd_infer(argv: list[str]) -> int:
    ckpt = argv[1] if len(argv) > 1 else CKPT
    prompt = argv[2] if len(argv) > 2 else "悟空"
    k = int(argv[3]) if len(argv) > 3 else 8
    chat = len(argv) > 4 and argv[4] == "chat"
    if not os.path.isfile(ckpt):
        print(f"load failed (train first): {ckpt}", flush=True)
        return 1
    device = setup_device()
    torch.manual_seed(1)
    model, stoi = load_ckpt(ckpt, device)
    print(f"snapshot  V={model.V}  d={D}  T={T}  K={k}  {ckpt}", flush=True)
    if chat:
        if prompt == "-":
            print("chat  (quit to exit)", flush=True)
            infer_repl(model, stoi, k, device, True)
        else:
            print("--- chat (frozen ask, fill reply) ---", flush=True)
            chat_once(model, prompt, stoi, k, device)
    elif prompt == "-":
        print("generate  (quit to exit)", flush=True)
        infer_repl(model, stoi, k, device, False)
    else:
        print("--- generate (prefix frozen, fill MASK) ---", flush=True)
        print_frames(model, prompt, stoi, k, device)
    return 0


def main():
    argv = sys.argv[1:]
    if argv and argv[0] in ("infer", "chat"):
        if argv[0] == "chat" and (len(argv) < 5 or argv[-1] != "chat"):
            argv = ["infer"] + argv[1:] + ["chat"]
        return cmd_infer(argv)
    path = argv[0] if argv else "app/llm/xyj.txt"
    epochs = int(argv[1]) if len(argv) > 1 else 16
    fresh = any(a == "new" for a in argv[2:])
    raw = open(path, "rb").read()
    if raw[:1] == b"{":
        print("jsonl not supported; pass a plain text book", flush=True)
        return 1
    text = raw.decode("utf-8", errors="replace")
    vocab = build_vocab(text, VOCAB_MAX)
    stoi = {c: i for i, c in enumerate(vocab)}
    ids = encode(text, stoi)
    wins = torch.from_numpy(windows(ids, T, STRIDE)).long()
    ntr = int(len(wins) * 0.9)
    train_w, test_w = wins[:ntr], wins[ntr:]
    device = setup_device()
    train_w = train_w.to(device)
    test_w = test_w.to(device)
    torch.manual_seed(1)
    random.seed(1)
    np.random.seed(1)
    model = SnapshotLM(vocab).to(device)
    opts = torch.optim.AdamW(model.parameters(), lr=LR, weight_decay=WD)
    n_batch = max(1, (len(train_w) + BATCH - 1) // BATCH)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opts, T_max=max(1, epochs * n_batch), eta_min=LR * 0.1)
    resume = (not fresh) and os.path.isfile(CKPT)
    if resume:
        blob = _load(CKPT, device)
        model.load_state_dict(blob["model"])
        if blob.get("opt"):
            try:
                opts.load_state_dict(blob["opt"])
                for g in opts.param_groups:
                    g["lr"] = LR
            except Exception:
                print("resume  opt skipped (device/shape)", flush=True)
        print(f"resume  {CKPT}", flush=True)
    nparam = sum(p.numel() for p in model.parameters() if p.requires_grad)
    print(
        f"snapshot-bert  V={model.V}  d={D}  L={N_LAYER}  H={N_HEAD}  T={T}  "
        f"params={nparam}  tokens={len(ids)}  train-win={len(train_w)}  test-win={len(test_w)}",
        flush=True,
    )
    print(
        "mask  train 35% cont-prefix + 35% r=50-85% + 30% r=12-35%  "
        f"span~half  eval {MASK_P:.0%}  corpus={path}",
        flush=True,
    )
    print(
        f"opt  AdamW lr={LR} cosine  wd={WD}  drop={DROP}  "
        f"bidir SDPA+ALiBi  tied head  B={BATCH} stride={STRIDE}",
        flush=True,
    )
    print("--- train ---", flush=True)
    t0 = time.perf_counter()
    best = 0
    for ep in range(epochs):
        acc, nll, hit, n, maj = run_epoch(model, opts, train_w, True, sched)
        tacc, tnll, th, tn, tmaj = run_epoch(model, opts, test_w, False)
        print(
            f"epoch {ep}  mask-acc ~{acc}%  maj ~{maj}%  nll {nll:.3f}  hit {hit} / {n}",
            flush=True,
        )
        print(
            f"         test mask-acc ~{tacc}%  maj ~{tmaj}%  nll {tnll:.3f}  hit {th} / {tn}",
            flush=True,
        )
        extra = ep % 4 == 0 or ep == epochs - 1
        if extra:
            a50 = eval_rate(model, test_w, 0.5)
            a90 = eval_rate(model, test_w, 0.9)
            ac = eval_cont(model, test_w)
            print(f"         test r=50% ~{a50}%  r=90% ~{a90}%  cont ~{ac}%", flush=True)
            print_k_curve(model, stoi, device)
        save_ckpt(CKPT, model, vocab, opts, sched, ep)
        if tacc >= best:
            best = tacc
    dt = time.perf_counter() - t0
    print(f"--- after train  {dt:.1f}s  best-test {best}%  {CKPT} ---", flush=True)
    ch, nh = probe(model, test_w, device)
    print(
        f"probe  scramble-vis  pred-changed {ch} / {nh}  (0 = ignores context)",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
