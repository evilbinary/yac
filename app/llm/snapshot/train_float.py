#!/usr/bin/env python3
"""Float counterpart of snapshot.yac MLM: bags + skip-visible + pos, MASK CE, Wout only.

  python app/llm/snapshot/train_float.py [corpus] [epochs]
"""
from __future__ import annotations

import math
import sys
import time
from collections import Counter

import numpy as np

T = 16
CAP = 256
D = 784  # L1 + L2 + R1 + T pos
BOOK_CAP = 80000
WIN_CAP = 1000
FOV_R = 16
FREEZE_N = 6
MASK_CH = "░"
UNK_CH = "<unk>"
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
    top = [c for c, _ in freq.most_common(max(1, cap - 1))]
    return top + [UNK_CH, MASK_CH]


def encode(text: str, stoi: dict[str, int]) -> np.ndarray:
    mid = stoi[MASK_CH]
    uid = stoi[UNK_CH]
    ids = []
    for c in chars_of(text):
        k = stoi.get(c)
        if k is None:
            ids.append(uid)
        elif k != mid:
            ids.append(k)
    return np.array(ids, dtype=np.int32)


def stride_pos(i: int, n: int, nuse: int) -> int:
    if nuse < 1 or n <= T:
        return 0
    span = max(n // nuse, 1)
    p = i * span
    return n - T if p + T > n else p


def skip_idx(ids: np.ndarray, j: int, step: int, mid: int, uid: int) -> int:
    while 0 <= j < T:
        v = int(ids[j])
        if v != mid and v != uid:
            return j
        j += step
    return -1


def features(canvas: np.ndarray, mid: int, uid: int) -> np.ndarray:
    """H[t]: nearest L, second L, nearest R, pos. MASK/UNK transparent."""
    H = np.zeros((T, D), dtype=np.float64)
    ids = canvas
    for t in range(T):
        i1 = skip_idx(ids, t - 1, -1, mid, uid)
        i2 = -1 if i1 < 0 else skip_idx(ids, i1 - 1, -1, mid, uid)
        ir = skip_idx(ids, t + 1, 1, mid, uid)
        if i1 >= 0:
            L = int(ids[i1])
            if 0 <= L < CAP:
                H[t, L] = 1.0
        if i2 >= 0:
            L2 = int(ids[i2])
            if 0 <= L2 < CAP:
                H[t, CAP + L2] = 1.0
        if ir >= 0:
            R = int(ids[ir])
            if 0 <= R < CAP:
                H[t, 2 * CAP + R] = 1.0
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


def mask_suffix_cut(gold: np.ndarray, mid: int, cut: int):
    canvas = gold.copy()
    flags = np.zeros(T, dtype=np.bool_)
    canvas[cut:] = mid
    flags[cut:] = True
    return canvas, flags


def apply_mask(gold: np.ndarray, mid: int, rng: np.random.Generator):
    k = int(rng.integers(0, 5))
    if k >= 3:
        span = max(1, T - 4)
        cut = 4 + int(rng.integers(0, span))
        return mask_suffix_cut(gold, mid, cut)
    return mask_random(gold, mid, rng, 15 + k * 15)


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
        self.W = np.zeros((D, self.Vout), dtype=np.float64)
        self.m = np.zeros_like(self.W)
        self.v = np.zeros_like(self.W)
        self.uid = vocab.index(UNK_CH) if UNK_CH in vocab else -1
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


def train_window(m: Model, gold: np.ndarray, rng: np.random.Generator, update: bool, freq=None):
    canvas, flags = apply_mask(gold, m.mid, rng)
    holes = []
    for t in range(T):
        if flags[t] and vis_dist(flags, t) <= FOV_R and int(gold[t]) != m.uid:
            holes.append(t)
    if not holes:
        return 0, 0, 0.0, 0.0, 0
    H = features(canvas, m.mid, m.uid)
    logits = m.logits(H)
    if m.uid >= 0:
        logits = logits.copy()
        logits[:, m.uid] = -1e6
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
        if freq is not None and 0 <= y < len(freq):
            freq[y] += 1
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
        H = features(canvas, mid, stoi[UNK_CH])
        logits = m.logits(H)
        if m.Vout > 0:
            logits = logits.copy()
            logits[:, stoi[UNK_CH]] = -1e30
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
    a = np.argmax(m.logits(features(canvas, m.mid, m.uid)), axis=1)
    vis = canvas != m.mid
    canvas = canvas.copy()
    canvas[vis] = rng.integers(0, m.Vout, size=int(vis.sum()))
    b = np.argmax(m.logits(features(canvas, m.mid, m.uid)), axis=1)
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
    nwin = max(1, n - T + 1)
    nuse = min(nwin, WIN_CAP)
    ntr = nuse - nuse // 5
    rng = np.random.default_rng(1)
    m = Model(vocab, rng)
    print(
        f"snapshot-float  V={m.V}  T={T}  tokens={n}  train-win={ntr}  test-win={nuse - ntr}"
    )
    print(f"vocab  top-{CAP - 1} + UNK + MASK  corpus={path}")
    print(f"opt  float softmax CE  Adam lr={ADAM_LR}  L1+L2+R1+pos  Wout=0")
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
            h, k, nl, inv, _ = train_window(m, gold, rng, True, freq)
            hit += h
            nm += k
            nll += nl
            invp += inv
            # maj: train_window doesn't return freq; approximate from a second pass skip
            _ = k
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
