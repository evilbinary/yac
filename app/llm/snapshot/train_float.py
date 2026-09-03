#!/usr/bin/env python3
"""Float counterpart of snapshot.yac MLM: same bags/adj/pos, MASK CE, Wout only.

  python app/llm/snapshot/train_float.py [corpus] [epochs]

Softmax / NLL / Adam are float64. Features match vis_left/right + mark_adj + mark_pos.
"""
from __future__ import annotations

import math
import sys
import time
from collections import Counter

import numpy as np

T = 16
CAP = 256
D = 784  # Lbag + Rbag + adj + T pos
BOOK_CAP = 80000
WIN_CAP = 400
FOV_R = 16
FREEZE_N = 6
MASK_CH = "░"
ADAM_LR = 0.02
ADAM_B1 = 0.9
ADAM_B2 = 0.999
ADAM_EPS = 1e-8


def take_mid(raw: bytes, cap: int) -> bytes:
    n = len(raw)
    if n <= cap:
        return raw
    return raw[n // 5 : n // 5 + cap]


def chars_of(s: str):
    for c in s:
        if c in "\n\r":
            continue
        yield c


def build_vocab_top(text: str, cap: int) -> list[str]:
    freq = Counter(chars_of(text))
    top = [c for c, _ in freq.most_common(cap)]
    return top + [MASK_CH]


def encode(text: str, stoi: dict[str, int]) -> np.ndarray:
    ids = [stoi[c] for c in chars_of(text) if c in stoi]
    return np.array(ids, dtype=np.int32)


def stride_pos(i: int, n: int, nuse: int) -> int:
    if nuse < 1 or n <= T:
        return 0
    span = max(n // nuse, T)
    p = i * span
    return n - T if p + T > n else p


def features(canvas: np.ndarray, mid: int) -> np.ndarray:
    """H[t]: left bag 0..CAP, right CAP..2CAP, adj 2CAP..3CAP, pos 3CAP+t."""
    H = np.zeros((T, D), dtype=np.float64)
    vis = canvas != mid
    ids = canvas
    for t in range(T):
        left = [int(ids[j]) for j in range(t) if vis[j] and ids[j] < CAP]
        right = [int(ids[j]) for j in range(t + 1, T) if vis[j] and ids[j] < CAP]
        if left:
            for i in left:
                H[t, i] += 1.0
            H[t, :CAP] /= len(left)
        if right:
            for i in right:
                H[t, CAP + i] += 1.0
            H[t, CAP : 2 * CAP] /= len(right)
        if t > 0 and vis[t - 1] and ids[t - 1] < CAP:
            H[t, 2 * CAP + int(ids[t - 1])] = 1.0
        if t + 1 < T and vis[t + 1] and ids[t + 1] < CAP:
            H[t, 2 * CAP + int(ids[t + 1])] += 0.5
        H[t, 3 * CAP + t] = 1.0
    return H


def vis_dist(flags: np.ndarray, t: int) -> int:
    best = 99
    for j in range(T):
        if flags[j]:
            continue
        best = min(best, abs(j - t))
    return best


def mask_random(gold: np.ndarray, mid: int, rng: np.random.Generator, rate: int):
    canvas = gold.copy()
    flags = np.zeros(T, dtype=np.bool_)
    for i in range(T):
        if gold[i] == mid:
            continue
        if int(rng.integers(0, 100)) < rate:
            canvas[i] = mid
            flags[i] = True
    return canvas, flags


def mask_suffix(gold: np.ndarray, mid: int, rng: np.random.Generator):
    canvas = gold.copy()
    flags = np.zeros(T, dtype=np.bool_)
    cut = 1 + int(rng.integers(0, T - 1))
    canvas[cut:] = mid
    flags[cut:] = True
    return canvas, flags


def apply_mask(gold: np.ndarray, mid: int, rng: np.random.Generator):
    if int(rng.integers(0, 2)) == 0:
        return mask_random(gold, mid, rng, 50)
    return mask_suffix(gold, mid, rng)


def softmax_rows(logits: np.ndarray) -> np.ndarray:
    x = logits - logits.max(axis=-1, keepdims=True)
    e = np.exp(x)
    return e / e.sum(axis=-1, keepdims=True)


class Model:
    def __init__(self, vocab: list[str], rng: np.random.Generator):
        self.vocab = vocab
        self.V = len(vocab)
        self.mid = self.V - 1
        self.Vout = self.V - 1
        self.W = rng.uniform(-2.0, 2.0, size=(D, self.Vout))
        self.m = np.zeros_like(self.W)
        self.v = np.zeros_like(self.W)
        self.tstep = 0

    def logits(self, H: np.ndarray) -> np.ndarray:
        return H @ self.W

    def adam(self, gW: np.ndarray):
        self.tstep += 1
        self.m = ADAM_B1 * self.m + (1.0 - ADAM_B1) * gW
        self.v = ADAM_B2 * self.v + (1.0 - ADAM_B2) * (gW * gW)
        mhat = self.m / (1.0 - ADAM_B1 ** self.tstep)
        vhat = self.v / (1.0 - ADAM_B2 ** self.tstep)
        self.W -= ADAM_LR * mhat / (np.sqrt(vhat) + ADAM_EPS)

    def show(self, canvas: np.ndarray) -> str:
        out = []
        for i in canvas:
            c = self.vocab[int(i)]
            if c == "\n":
                c = "↵"
            elif c == "\r":
                c = "␍"
            out.append(c)
        return "".join(out)


def train_window(m: Model, gold: np.ndarray, rng: np.random.Generator, update: bool):
    canvas, flags = apply_mask(gold, m.mid, rng)
    holes = []
    for t in range(T):
        if flags[t] and vis_dist(flags, t) <= FOV_R:
            holes.append(t)
    if not holes:
        return 0, 0, 0.0, 0.0, 0
    H = features(canvas, m.mid)
    logits = m.logits(H)
    P = softmax_rows(logits)
    nll = 0.0
    invp = 0.0
    hit = 0
    gW = np.zeros_like(m.W) if update else None
    for t in holes:
        y = int(gold[t])
        if y >= m.Vout:
            continue
        py = float(P[t, y])
        py = max(py, 1e-12)
        nll += -math.log(py)
        invp += 1.0 / py
        pred = int(np.argmax(logits[t]))
        if pred == y:
            hit += 1
        if update:
            g = P[t].copy()
            g[y] -= 1.0
            gW += np.outer(H[t], g)
    n = len(holes)
    if update and n:
        m.adam(gW / n)
    return hit, n, nll, invp, 1


def eval_window(m: Model, gold: np.ndarray, rng: np.random.Generator):
    return train_window(m, gold, rng, update=False)


def win_at(ids: np.ndarray, pos: int, mid: int) -> np.ndarray:
    out = np.full(T, mid, dtype=np.int32)
    sl = ids[pos : pos + T]
    out[: len(sl)] = sl
    return out


def prompt_ids(prompt: str, stoi: dict[str, int], mid: int) -> list[int]:
    ids = []
    for c in prompt:
        k = stoi.get(c)
        if k is None or k == mid:
            continue
        ids.append(k)
    if len(ids) > FREEZE_N:
        ids = ids[-FREEZE_N:]
    return ids


def canvas_from_prompt(m: Model, prompt: str, stoi: dict[str, int]) -> np.ndarray:
    pids = prompt_ids(prompt, stoi, m.mid)
    c = np.full(T, m.mid, dtype=np.int32)
    n = min(len(pids), T)
    c[:n] = pids[:n]
    return c


def denoise(m: Model, prompt: str, stoi: dict[str, int], k: int) -> np.ndarray:
    canvas = canvas_from_prompt(m, prompt, stoi)
    mid = m.mid
    for step in range(k):
        left = k - step
        nm = int((canvas == mid).sum())
        if nm <= 0:
            break
        H = features(canvas, mid)
        logits = m.logits(H)
        pred = np.argmax(logits, axis=1)
        conf = logits[np.arange(T), pred].astype(np.float64)
        conf[canvas != mid] = -1e30
        focus = int(np.argmax(canvas == mid)) if nm else 0
        rad = max(1, (step + 1) * T // k)
        if left != 1:
            for i in range(T):
                if canvas[i] == mid and abs(i - focus) > rad:
                    conf[i] = -1e30
        ntake = nm if left == 1 else (nm + left - 1) // left
        for _ in range(ntake):
            i = int(np.argmax(conf))
            if conf[i] < -1e20:
                break
            canvas[i] = int(pred[i])
            conf[i] = -1e30
    return canvas


def probe(m: Model, ids: np.ndarray, rng: np.random.Generator) -> tuple[int, int]:
    gold = win_at(ids, 0, m.mid)
    canvas, _ = mask_random(gold, m.mid, rng, 50)
    n0 = int((canvas == m.mid).sum())
    if n0 < 1:
        return 0, 0
    a = np.argmax(m.logits(features(canvas, m.mid)), axis=1)
    vis = canvas != m.mid
    canvas = canvas.copy()
    canvas[vis] = rng.integers(0, m.Vout, size=int(vis.sum()))
    b = np.argmax(m.logits(features(canvas, m.mid)), axis=1)
    holes = canvas == m.mid
    changed = int((a[holes] != b[holes]).sum())
    return changed, n0


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "app/llm/xyj.txt"
    epochs = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    raw = open(path, "rb").read()
    if raw[:1] == b"{":
        print("jsonl not supported in float probe; pass a plain text book")
        return 1
    text = take_mid(raw, BOOK_CAP).decode("utf-8", errors="replace")
    vocab = build_vocab_top(text, CAP)
    stoi = {c: i for i, c in enumerate(vocab)}
    ids = encode(text, stoi)
    n = len(ids)
    nwin = max(1, n // T)
    nuse = min(nwin, WIN_CAP)
    ntr = nuse - nuse // 5
    rng = np.random.default_rng(1)
    m = Model(vocab, rng)
    print(
        f"snapshot-float  V={m.V}  T={T}  tokens={n}  train-win={ntr}  test-win={nuse - ntr}"
    )
    print(f"vocab  top-{CAP} + MASK  corpus={path}")
    print(f"opt  float softmax CE  Adam lr={ADAM_LR}  Wout only")
    print("--- train ---")
    t0 = time.perf_counter()
    for ep in range(epochs):
        hit = nm = 0
        nll = invp = 0.0
        freq = np.zeros(m.Vout, dtype=np.int64)
        for i in range(nuse):
            if i % 5 == 4:
                continue
            gold = win_at(ids, stride_pos(i, n, nuse), m.mid)
            canvas, flags = apply_mask(gold, m.mid, rng)
            holes = [t for t in range(T) if flags[t] and vis_dist(flags, t) <= FOV_R]
            if not holes:
                continue
            H = features(canvas, m.mid)
            logits = m.logits(H)
            P = softmax_rows(logits)
            gW = np.zeros_like(m.W)
            for t in holes:
                y = int(gold[t])
                if y >= m.Vout:
                    continue
                py = max(float(P[t, y]), 1e-12)
                nll += -math.log(py)
                invp += 1.0 / py
                pred = int(np.argmax(logits[t]))
                hit += int(pred == y)
                nm += 1
                freq[y] += 1
                g = P[t].copy()
                g[y] -= 1.0
                gW += np.outer(H[t], g)
            if holes:
                m.adam(gW / len(holes))
        maj = int(freq.max()) if nm else 0
        acc = 100 * hit // nm if nm else 0
        mj = 100 * maj // nm if nm else 0
        mean_nll = nll / nm if nm else 0.0
        mean_inv = invp / nm if nm else 0.0
        mean_bits = mean_nll / math.log(2)
        # test
        th = tn = 0
        for i in range(nuse):
            if i % 5 != 4:
                continue
            gold = win_at(ids, stride_pos(i, n, nuse), m.mid)
            h, k, _, _, _ = eval_window(m, gold, rng)
            th += h
            tn += k
        tacc = 100 * th // tn if tn else 0
        print(
            f"epoch {ep}  acc ~{acc}%  maj ~{mj}%  nll {mean_nll:.3f} nat "
            f"({mean_bits:.2f} bit)  invp {mean_inv:.1f}  hit {hit} / {nm}  maj-hit {maj} / {nm}"
        )
        print(f"         test acc ~{tacc}%  hit {th} / {tn}")
    dt = time.perf_counter() - t0
    print(f"--- after train  {dt:.1f}s ---")
    pr_rng = np.random.default_rng(7)
    ch, nmask = probe(m, ids, pr_rng)
    print(f"probe  scramble-vis  pred-changed {ch} / {nmask}  (0 = ignores context)")
    print("--- K curve ---")
    for k in (1, 4, 8, 14):
        print(f"K={k:<3} {m.show(denoise(m, '悟空', stoi, k))}")
    print(f"K=8 Romeo  {m.show(denoise(m, 'Romeo', stoi, 8))}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
