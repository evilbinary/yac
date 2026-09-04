#!/usr/bin/env python3
"""Snapshot LM: bidirectional Transformer + MaskGIT shutter.

No causal mask. Loss only on MASK holes. See DESIGN.md.

  python app/llm/snapshot/train_float.py [corpus] [epochs] [new]
  python app/llm/snapshot/train_float.py infer [ckpt] [prompt] [K] [chat]
"""
from __future__ import annotations

import math
import os
import random
import sys
import time
from collections import Counter

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

T = 64  # 底片长度（字）；一次前向固定看这么多格
D = 192  # 隐层宽
N_LAYER = 4  # 残差块数；透镜厚度，不是显影步数 K
N_HEAD = 6  # 注意力头数；d_h = D / N_HEAD
FF = 768  # MLP 宽（4 * D）
DROP = 0.05  # 残差 / 嵌入 / 注意力 dropout
BATCH = 96  # CPU 默认；CUDA 在 setup_device 里改成 256
STRIDE = 64  # 训练窗步长；64 = 不重叠（更快）
VOCAB_MAX = 4096  # 字符 cap，另加 PAD/UNK/MASK
MASK_P = 0.15  # 仅评价对照；训练用 cosine 动态 r∈[0.10,0.90]
LR = 6e-4  # AdamW；cosine 降到 0.1 * LR
WD = 0.01  # 权重衰减
FREEZE_N = 8  # 训练：散点 keep 个数上界
PHRASE_N = 12  # 训练：短语 / 说话骨架 keep 长度上界
GEN_TEMP = 0.8  # 显影温度（作用在已经算完的整页 logits 上）
GEN_REP = 2.5  # 已可见字的重复惩罚
GEN_NEAR = 4.0  # 禁止与左邻同一字（挡「你你」）
GEN_BETA1 = 1.2  # 主干：信息量指数（稀有字优先）
GEN_BETA2 = 0.2  # 细节：接近模型原概率
GEN_CONF = 0.08  # 主干过阈；无候选时回退 top-1
GEN_K = 24  # 默认显影步数；K=8 锁不住主干
GEN_NEAR_N = 2  # 只在已有真字半径内落笔，禁止远处第二岛
MASK_CH = "[MASK]"  # 未曝光格
UNK_CH = "[UNK]"  # 词表外
PAD_CH = "[PAD]"  # 短窗垫字
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
        # RoPE on Q/K so identical MASK slots still attend different neighbors.
        half = self.dh // 2
        inv = 1.0 / (10000 ** (torch.arange(0, half).float() / half))
        ang = torch.arange(T).float()[:, None] * inv[None, :]
        self.register_buffer("rope_cos", ang.cos(), persistent=False)
        self.register_buffer("rope_sin", ang.sin(), persistent=False)

    def _rope(self, x: torch.Tensor) -> torch.Tensor:
        t = x.size(-2)
        half = self.dh // 2
        cos = self.rope_cos[:t].to(dtype=x.dtype)[None, None, :, :]
        sin = self.rope_sin[:t].to(dtype=x.dtype)[None, None, :, :]
        x1, x2 = x[..., :half], x[..., half:]
        return torch.cat((x1 * cos - x2 * sin, x1 * sin + x2 * cos), dim=-1)

    def forward(self, x: torch.Tensor, key_pad: torch.Tensor | None):
        b, t, d = x.shape
        h = self.ln1(x)
        qkv = self.qkv(h).view(b, t, 3, self.n_head, self.dh).permute(2, 0, 3, 1, 4)
        q, k, v = qkv.unbind(0)
        q, k = self._rope(q), self._rope(k)
        pad = None if key_pad is None else key_pad[:, None, None, :]
        a = F.scaled_dot_product_attention(
            q, k, v, attn_mask=pad,
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


def _keep_scatter(valid: torch.Tensor, n_lo: int, n_hi: int) -> torch.Tensor:
    """Leave n_lo..n_hi gold chars at their native indices (not moved to a template)."""
    b, t = valid.shape
    device = valid.device
    score = torch.rand(b, t, device=device).masked_fill(~valid, -1.0)
    nk = torch.randint(n_lo, n_hi + 1, (b, 1), device=device)
    order = score.argsort(dim=-1, descending=True)
    rank = torch.empty_like(order)
    rank.scatter_(1, order, torch.arange(t, device=device).expand(b, t))
    return valid & (rank < nk)


def _keep_prefix(valid: torch.Tensor, n_lo: int, n_hi: int) -> torch.Tensor:
    """Infer-like: keep a left prefix at native indices, rest MASK. No PAD."""
    b, t = valid.shape
    nk = torch.randint(n_lo, n_hi + 1, (b, 1), device=valid.device)
    pos = torch.arange(t, device=valid.device).view(1, t)
    return valid & (pos < nk)


def _keep_talk(gold: torch.Tensor, valid: torch.Tensor, talk_ids: torch.Tensor, n_lo: int, n_hi: int) -> torch.Tensor:
    """Keep a real span around 道/叫/喝/问/曰/云 (XYJ quote scaffold)."""
    b, t = gold.shape
    device = gold.device
    if talk_ids.numel() == 0:
        return torch.zeros_like(valid)
    is_talk = (gold.unsqueeze(-1) == talk_ids.view(1, 1, -1)).any(-1) & valid
    has = is_talk.any(dim=1)
    rnd = torch.rand(b, t, device=device).masked_fill(~is_talk, -1.0)
    at = rnd.argmax(dim=1)
    width = torch.randint(n_lo, n_hi + 1, (b,), device=device)
    start = (at - width // 3).clamp(min=0)
    start = torch.minimum(start, (t - width).clamp(min=0))
    pos = torch.arange(t, device=device).view(1, t)
    keep = valid & (pos >= start.unsqueeze(1)) & (pos < (start + width).unsqueeze(1))
    return keep & has.unsqueeze(1)


def _keep_phrases(valid: torch.Tensor, n_lo: int, n_hi: int) -> torch.Tensor:
    """1–2 substrings that already sit in the window; start is wherever they are, not centered."""
    b, t = valid.shape
    device = valid.device
    pos = torch.arange(t, device=device).view(1, t)

    def one_span():
        width = torch.randint(n_lo, n_hi + 1, (b, 1), device=device)
        cap = (t - width.float() + 1).clamp(min=1)
        start = (torch.rand(b, 1, device=device) * cap).long()
        return valid & (pos >= start) & (pos < start + width)

    keep = one_span()
    two = torch.rand(b, 1, device=device) < 0.5
    extra = one_span()
    return keep | (extra & two.expand(b, t))


def mask_tokens(
    gold: torch.Tensor,
    pad: int,
    mask_id: int,
    p: float | None,
    talk_ids: torch.Tensor | None = None,
):
    """Train: cosine independent holes + generate-family keeps at native indices. Eval: fixed p."""
    b, t = gold.shape
    valid = gold != pad
    if p is not None:
        pick = (torch.rand(b, t, device=gold.device) < p) & valid
        canvas = gold.clone()
        canvas[pick] = mask_id
        return canvas, pick

    device = gold.device
    u = torch.rand(b, 1, device=device)
    ratio = 0.10 + 0.80 * torch.cos(u * math.pi / 2)
    pick = (torch.rand(b, t, device=device) < ratio) & valid
    is_gen = torch.rand(b, 1, device=device) < 0.40
    keep_sc = _keep_scatter(valid, 2, FREEZE_N)
    keep_ph = _keep_phrases(valid, 2, PHRASE_N)
    keep_pf = _keep_prefix(valid, 2, FREEZE_N)
    if talk_ids is None:
        talk_ids = gold.new_zeros(0)
    keep_tk = _keep_talk(gold, valid, talk_ids, 4, PHRASE_N)
    mode = torch.rand(b, 1, device=device)
    keep = torch.where(
        mode < 0.25,
        keep_pf,
        torch.where(mode < 0.45, keep_sc, torch.where(mode < 0.70, keep_tk, keep_ph)),
    )
    empty = ~keep.any(dim=1, keepdim=True)
    keep = torch.where(empty.expand(b, t), keep_ph, keep)
    pick_gen = valid & ~keep
    pick = torch.where(is_gen.expand(b, t), pick_gen, pick)
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
            canvas, pick = mask_tokens(
                gold, model.pad, model.mask, p, talk_ids=_vocab_ids(model, _TALK),
            )
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
def eval_cont(model, wins: torch.Tensor, freeze: int = 8, width: int | None = None, max_win: int = 128) -> int:
    """Infer-like canvas. If width set, score only freeze:freeze+width."""
    gold = wins[: min(max_win, wins.size(0))]
    hit = n = 0
    for i0 in range(0, gold.size(0), BATCH):
        g = gold[i0 : i0 + BATCH]
        suffix = (g != model.pad)
        suffix[:, :freeze] = False
        if not suffix.any():
            continue
        canvas = g.clone()
        canvas[suffix] = model.mask
        if width is None:
            pick = suffix
        else:
            pick = suffix.clone()
            pick[:, freeze + width :] = False
            pick[:, :freeze] = False
        if not pick.any():
            continue
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
    g = [c for c in prompt if c != "\r"]
    return g[: T - 2]


def encode_prompt(prompt: str, stoi: dict[str, int], cap: int | None = None) -> list[int]:
    unk = stoi[UNK_CH]
    ids = [stoi.get(c, unk) for c in prompt if c != "\r"]
    n = (T - 2) if cap is None else cap
    return ids[:n] if n else ids


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


def show_canvas(
    model, ids: list[int], glyphs: list[str] | None = None, start: int = 0, glyph_at: int = 0
) -> str:
    out = []
    for i, tok in enumerate(ids[start:]):
        pos = start + i
        gi = pos - glyph_at
        if glyphs is not None and 0 <= gi < len(glyphs):
            out.append(glyphs[gi])
        else:
            out.append(show_tok(model, tok))
    return "".join(out)


_FLESH = set("的了是在着过和与也又都就还把被让从到得地吗呢吧啊呀么呵之乎亦，。！？：；、\"'（）“”\n")


def _info_weight(model) -> torch.Tensor:
    """词表按频次降序：下标越大越稀有，信息量越高。"""
    v = model.V
    n = max(v - 3, 1)
    idx = torch.arange(v, device=model.tok.weight.device, dtype=torch.float32)
    rank = (idx - 2).clamp(min=1)
    w = 0.2 + 0.8 * (rank / n).sqrt()
    w[model.pad] = 0
    w[model.unk] = 0
    w[model.mask] = 0
    for i, c in enumerate(model.vocab):
        if c in _FLESH:
            w[i] = w[i] * 0.15
    return w


_TALK = set("道叫喝问曰云")
_STOP = set("。！？")
_QUOTE_OPEN = set("\"“")
_QUOTE_CLOSE = set("\"”")
_PUNCT_RUN = set("，。！？：；、\"'“”")


def _vocab_ids(model, chars: set[str]) -> torch.Tensor:
    ids = [i for i, c in enumerate(model.vocab) if c in chars]
    if not ids:
        return torch.zeros(0, device=model.tok.weight.device, dtype=torch.long)
    return torch.tensor(ids, device=model.tok.weight.device, dtype=torch.long)


def _is_in(ids: torch.Tensor, table: torch.Tensor) -> torch.Tensor:
    if table.numel() == 0:
        return torch.zeros(ids.shape, dtype=torch.bool, device=ids.device)
    return (ids.unsqueeze(-1) == table).any(-1)


def _ban_ids(model) -> torch.Tensor:
    """工程防护：UNK/PAD/MASK、脚注括号与 ASCII 数字不进正文。"""
    ids = [model.pad, model.unk, model.mask]
    for i, c in enumerate(model.vocab):
        if i in (model.pad, model.unk, model.mask):
            continue
        if c in "[]*#<>" or (len(c) == 1 and c.isascii() and c.isdigit()):
            ids.append(i)
    return torch.tensor(ids, device=model.tok.weight.device, dtype=torch.long)


def _gen_logits(model, canvas: torch.Tensor) -> torch.Tensor:
    """一次快门：整页 logits。过滤 + 重复惩罚都作用在这张已经算完的分上。"""
    logits = model(canvas)[0] / GEN_TEMP
    logits[:, _ban_ids(model)] = -1e9
    vis = canvas[0]
    vis = vis[(vis != model.mask) & (vis != model.pad)]
    if vis.numel():
        counts = torch.bincount(vis, minlength=model.V).to(logits.dtype)
        logits = logits - GEN_REP * counts
    if "\n" in model.vocab:
        logits[:, model.vocab.index("\n")] = logits[:, model.vocab.index("\n")] - 3.0
    left = canvas[0, :-1]
    ok = (left != model.mask) & (left != model.pad)
    if bool(ok.any()):
        idx = torch.arange(1, T, device=logits.device)
        logits[idx[ok], left[ok]] = logits[idx[ok], left[ok]] - GEN_NEAR
    return logits


def blank_canvas(prompt: str, stoi: dict[str, int], model, device):
    """提示按原词序写在下标 0..n，其余 MASK，不搬格。"""
    ids = encode_prompt(prompt, stoi)
    canvas = torch.full((1, T), model.mask, dtype=torch.long, device=device)
    n = min(len(ids), T - 2)
    if n:
        canvas[0, :n] = torch.tensor(ids[:n], device=device)
    return canvas, 0


def _n_take(n_ok: int, steps_left: int) -> int:
    if n_ok <= 0:
        return 0
    sl = max(1, steps_left)
    return max(1, min(n_ok, (n_ok + sl - 1) // sl))


def _near_visible(vis: torch.Tensor, radius: int) -> torch.Tensor:
    """True on cells within `radius` of any already-visible char (one blob, not a second island)."""
    t = vis.numel()
    device = vis.device
    idx = torch.arange(t, device=device)
    vis_idx = idx[vis]
    if vis_idx.numel() == 0:
        return torch.zeros(t, dtype=torch.bool, device=device)
    dist = (idx.unsqueeze(1) - vis_idx.unsqueeze(0)).abs().min(dim=1).values
    return dist <= radius


def _cut_after_utterance(
    ids: torch.Tensor,
    vis: torch.Tensor,
    stop_ids: torch.Tensor,
    open_ids: torch.Tensor,
    close_ids: torch.Tensor,
) -> torch.Tensor:
    """Holes to the right of the first finished sentence/quote — do not start a second scene."""
    t = ids.numel()
    device = ids.device
    after = torch.zeros(t, dtype=torch.bool, device=device)
    opened = False
    n_vis = 0
    cut = t
    for i in range(t):
        if not bool(vis[i]):
            continue
        n_vis += 1
        tok = ids[i]
        closed = close_ids.numel() and bool((close_ids == tok).any())
        stopped = stop_ids.numel() and bool((stop_ids == tok).any())
        if opened and closed:
            if i + 1 >= t or not bool(vis[i + 1 :].any()):
                cut = i
                break
        if open_ids.numel() and bool((open_ids == tok).any()):
            opened = True
        if n_vis >= 2 and stopped:
            if i + 1 >= t or not bool(vis[i + 1 :].any()):
                cut = i
                break
    if cut < t:
        after[cut + 1 :] = True
    return after


@torch.no_grad()
def denoise_ids(
    model, prompt: str, stoi: dict[str, int], k: int, device, frames: bool = False,
):
    """Stage1: parallel core near the blob. Stage2: flesh. Cut after first utterance."""
    model.eval()
    canvas, at = blank_canvas(prompt, stoi, model, device)
    frozen = canvas[0] != model.mask
    orig = canvas[0].clone()
    locked = frozen.clone()
    k = max(1, k)
    n_prompt = int(frozen.sum().item())
    info = _info_weight(model)
    talk_ids = _vocab_ids(model, _TALK)
    flesh_ids = _vocab_ids(model, _FLESH)
    stop_ids = _vocab_ids(model, _STOP)
    open_ids = _vocab_ids(model, _QUOTE_OPEN)
    close_ids = _vocab_ids(model, _QUOTE_CLOSE)
    trunk_end = max(1, k // 3)
    snaps = [canvas[0].clone()] if frames else None
    for step in range(k):
        logits = _gen_logits(model, canvas)
        probs = F.softmax(logits, dim=-1)
        trunk = step < trunk_end
        beta = GEN_BETA1 if trunk else GEN_BETA2
        score = probs * info.unsqueeze(0).pow(beta)
        top2 = score.topk(2, dim=-1)
        pred = top2.indices[:, 0].clone()
        alt = top2.indices[:, 1]
        run = pred[1:] == pred[:-1]
        pred[1:] = torch.where(run, alt[1:], pred[1:])
        pred = torch.where(frozen, orig, pred)
        conf = probs[torch.arange(T, device=device), pred]
        hole = (~locked) & (canvas[0] == model.mask)
        vis = (canvas[0] != model.mask) & (canvas[0] != model.pad)
        n_talk = int((_is_in(canvas[0], talk_ids) & vis).sum().item())
        n_open = int((_is_in(canvas[0], open_ids) & vis).sum().item())
        if (not trunk) and n_talk >= 1:
            pred = torch.where(_is_in(pred, talk_ids), alt, pred)
        if (not trunk) and n_open >= 1:
            pred = torch.where(_is_in(pred, open_ids), alt, pred)
        conf = probs[torch.arange(T, device=device), pred]
        near = _near_visible(vis, GEN_NEAR_N)
        past = _cut_after_utterance(canvas[0], vis, stop_ids, open_ids, close_ids)
        core = ~_is_in(pred, flesh_ids)
        sc = conf * info[pred].pow(beta)
        sc[~hole | ~near | past] = -1.0
        any_right = torch.zeros(T, dtype=torch.bool, device=device)
        if T > 1:
            any_right[:-1] = vis.flip(0).cumsum(0).flip(0)[1:] > 0
        sc[_is_in(pred, stop_ids) & any_right] = -1.0
        if trunk:
            sc[~core | (conf < GEN_CONF)] = -1.0
            if int((sc >= 0).sum().item()) == 0:
                sc = conf * info[pred].pow(beta)
                sc[~hole | ~near | past | ~core] = -1.0
        else:
            sc[conf < (GEN_CONF * 0.5)] = -1.0
        n_ok = int((sc >= 0).sum().item())
        steps_left = (trunk_end - step) if trunk else (k - step)
        m = _n_take(n_ok, steps_left)
        if m > 0:
            top = torch.topk(sc, k=m).indices
            take = torch.zeros(T, dtype=torch.bool, device=device)
            take[top] = sc[top] >= 0
            canvas[0, take] = pred[take]
            if step < k - 1 and int(take.sum().item()) > 1:
                left_ok = torch.zeros(T, dtype=torch.bool, device=device)
                right_ok = torch.zeros(T, dtype=torch.bool, device=device)
                left_ok[1:] = vis[:-1]
                right_ok[:-1] = vis[1:]
                edge = take & (~(left_ok & right_ok)) & (~frozen)
                n_edge = int(edge.sum().item())
                if n_edge > 0:
                    n_rm = max(1, n_edge // 3)
                    conf_rm = conf.clone()
                    conf_rm[~edge] = 1e9
                    low = torch.topk(conf_rm, k=min(n_rm, n_edge), largest=False).indices
                    canvas[0, low] = model.mask
        if step == trunk_end - 1:
            filled = (canvas[0] != model.mask) & (~frozen)
            if int(filled.sum().item()) >= max(2, n_prompt):
                locked = canvas[0] != model.mask
        if snaps is not None:
            snaps.append(canvas[0].clone())
    out = canvas[0]
    if frames:
        return out, snaps, at
    return out, at


def denoise(model, prompt: str, stoi: dict[str, int], k: int, device) -> str:
    ids, at = denoise_ids(model, prompt, stoi, k, device)
    return show_canvas(model, ids.tolist(), prompt_glyphs(prompt), glyph_at=at)


def print_frames(model, prompt: str, stoi: dict[str, int], k: int, device):
    glyphs = prompt_glyphs(prompt)
    _ids, snaps, at = denoise_ids(model, prompt, stoi, k, device, frames=True)
    last = len(snaps) - 1
    for i, snap in enumerate(snaps):
        tag = "final" if i == last else f"step {i}"
        print(f"{tag:<8}{show_canvas(model, snap.tolist(), glyphs, glyph_at=at)}", flush=True)


def print_k_curve(model, stoi: dict[str, int], device):
    print("--- generate (trunk then detail) ---", flush=True)
    torch.manual_seed(1)
    print(f"K={GEN_K} 悟空            {denoise(model, '悟空', stoi, GEN_K, device)}", flush=True)
    torch.manual_seed(1)
    print(f"K={GEN_K} 美猴王          {denoise(model, '美猴王', stoi, GEN_K, device)}", flush=True)
    torch.manual_seed(1)
    print(f"K={GEN_K} 孙悟空打妖怪    {denoise(model, '孙悟空打妖怪', stoi, GEN_K, device)}", flush=True)


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
    ids, _at = denoise_ids(model, prompt, stoi, k, device)
    u = n_freeze(prompt)
    print(f"reply  {show_canvas(model, ids.tolist(), start=u)}", flush=True)


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
    k = int(argv[3]) if len(argv) > 3 and argv[3].lstrip("-").isdigit() else GEN_K
    chat = "chat" in argv
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
        print("--- generate (trunk then detail) ---", flush=True)
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
        "mask  train cosine-r[0.10,0.90] + 40% keep-at-native (prefix/scatter/talk/phrase)  "
        f"eval r=15/50/90%  corpus={path}",
        flush=True,
    )
    print(
        f"opt  AdamW lr={LR} cosine  wd={WD}  drop={DROP}  "
        f"bidir SDPA+RoPE  tied head  B={BATCH} stride={STRIDE}",
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
            f"         test r=15% ~{tacc}%  maj ~{tmaj}%  nll {tnll:.3f}  hit {th} / {tn}",
            flush=True,
        )
        a50 = eval_rate(model, test_w, 0.5)
        a90 = eval_rate(model, test_w, 0.9)
        print(f"         test r=50% ~{a50}%  r=90% ~{a90}%", flush=True)
        extra = ep % 4 == 0 or ep == epochs - 1
        if extra:
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
