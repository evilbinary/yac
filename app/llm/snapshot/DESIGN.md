# 快照并行（Snapshot LM）

`app/llm` 下的第三个实验：不写自回归「打印机」，写扩散式「快门」。

形态由本文定。**先在浮点里证明能填空、能快门**（`train_float.py`），再 port 到 `snapshot.yac`。Yac 侧的袋特征 / 只训 Wout / T=16 小词表已经证伪：mask-acc 贴着 maj，probe 可为 0，生成塌成标点。那些是运行时约束下的旧实现，不是目标架构。

架构按「长度增加时迭代次数不跟着涨」来设计：K 是超参，与 T 无关。

## 1. 要证明什么

自回归（trigram、现有 causal Transformer）是：已知左边，猜下一个字，长度 N 就要 N 次前向。

快照并行是：先铺一整张底片（长度固定的噪声序列），再对**所有位置同时**显影若干次。N=50 和 N=200 都是 K 次**全局曝光**（一次前向看完整张画布），不是从上到下、从左到右扫一遍。

成功标准不是流畅闲聊，而是三件可观察的事：

1. 一次前向写出整段 logits，而不是 `argmax` 出一个字再拼回去。
2. 增加 K，整段从「雪花」变成可读；K 太小则糊，K 够用后收益变平。
3. 提示词只约束底片的一部分（左侧若干字冻结），其余位置按置信度并行填实。

做不到这三条，就还是换皮的自回归。

主指标是 **MASK 位填空 acc**（必须明显高于 maj），不是高掩码 generate 好不好看。生成另看 K 曲线。不要把 BERT 的 10% keep 算进 acc——那是抄输入，会把 8% 的真挖洞吹成 15%。

## 2. 快门对照

| 相机 | 本实验 |
| --- | --- |
| 取景框 | 输出长度 T（当前 Python：64） |
| 白纸底片 | 长度 T 的画布：提示区真字，生成区 `[MASK]` |
| 按下快门 | 一次双向 Transformer 前向，看整张画布 |
| 显影次数 | 去噪步数 K（4 / 8 / 16） |
| 淡影 → 清晰 | 每步只把一部分仍为 MASK 的格子写成词表里的字 |
| 成片 | K 步之后没有 MASK |
| 连拍 | 第二阶段：语义帧（第一版不做） |

### 2.1 扫描方向：没有方向

- 一步之内，位置 0 和位置 T-1 **一起**出 logits。
- 揭哪些 MASK 只按**置信度**，不按阅读顺序、不按行号。先揭左边再揭右边 = 换皮自回归。
- 终端打成一行字，只是人读的序列化。
- 训练挖洞各位置独立随机，不按扫描线，**不要把「挖后缀」当一半训练任务**（那是因果监督，混进来快门就假了）。

### 2.2 一张快门的硬规则

\[
\mathrm{logits} = f(\text{整张画布})
\]

`forward` 只读 `ids`，写出 `logits[T][V]`。揭 MASK 发生在返回之后，**本步选中的格子一次提交**。禁止：算完位置 i 写回 `ids[i]`，再用新画布去填位置 j（步内卷帘）。重复惩罚、温度只允许作用在**已经算完的这一张 logits** 上，再同时写下本步的 m 格。

步与步之间才有时间：第 k 步的输入包含第 k−1 步已揭开的字。层数是透镜厚度，不是扫行。causal Transformer 不能当这张快门。

## 3. 第一阶段：单帧

离散 MASK 扩散 / MaskGIT：词表是整数下标。Python 用浮点 CE；Yac 仍用整数下标 + `BYTES` 存盘，不要 `str_cat` 拼权重。

### 3.1 画布

`ids[0..T)`。

- 特殊字：`[PAD]` `[UNK]` `[MASK]`。OOV 编成 UNK，不要丢掉导致提示空画布。
- 推理：提示编进左侧（最长 `FREEZE_N`，当前 8），这些下标永不改写；其余开场全 `[MASK]`。
- T 固定。短则 PAD（训练窗）或截断；不做「先预测这篇有几个 token」。

### 3.2 噪声

对一条长 T 的真窗口（书上滑动切片，**训练时没有冻结提示区**）：

1. 抽掩码率 r。不要只用 15%：推理几乎是整页 MASK，只训小洞会学成 BERT cloze，generate 仍塌成高频字。
2. 当前有效配比（Python）：约 75% 行 r∈[12%, 35%]（句法），约 25% 行 r∈[45%, 75%]（接近第一帧）。不要 r=100%（没有可见上下文）。
3. 每个有效位置独立以概率 r 变成 `[MASK]`。损失只在这些洞上。不要把 BERT 的 10% keep 算进 mask-acc。随机替换可选，不是主路径。
4. 评价固定 r=15%，与 BERT cloze 可对照；另可抽查 r=50%/90% 看 generate 是否同分布。

### 3.3 对焦：真双向 Transformer

必须改掉 causal 的两点：位置 t 能看见整张画布；**每个位置各自一份 logits**。

已证明有效的结构（`train_float.py`）：

| 项 | 值 |
| --- | --- |
| 语料 | 全书 `app/llm/xyj.txt`（约 80 万字），不要 80KB 切片当主实验 |
| 词表 | 频次 cap 4096 + PAD/UNK/MASK；稀有字 UNK |
| T | 64 |
| d | 192 |
| 层 / 头 / FF | 4 / 6 / 768 |
| 注意力 | 双向 SDPA；**对称距离偏置（ALiBi 绝对值）** |
| 位置 | 学到的 `pos[T]`，与 token 相加 |
| 读出 | 与 token embedding 绑定：`logits = h @ Eᵀ` |
| 优化 | AdamW，整网都训（不要冻 QKV 只训 Wout） |
| 初值 | embedding / Linear std=0.02 |

ALiBi 距离偏置的原因：所有 MASK 格共享同一个 token 向量。注意力若接近均匀，MASK 隐向量会塌成同一个方向（cosine≈1），argmax 永远是「，」。近邻偏置让不同洞一开始就看不同邻居。这不是卷帘，QK 仍是全对全，只是近的权更大。

不要再做：784 维邻字 one-hot 上的假 QKV、左右可见袋当唯一特征、只训 Wout。那些在 T=16、V=257 上 acc≈maj，生成标点循环。

前向次数 = K，不要和「每步墙钟」混为一谈。复杂度 O(K·(T²·d + T·d·V))。训练可只对洞做 V 向读出（`logits_pick`），推理仍要整表。

### 3.4 显影（推理）

1. 画布 = 提示 + 全 MASK。
2. 重复 K 次：一次双向前向；只在仍为 MASK 的格子上取候选和置信度（softmax 概率，不要生 logit——高频字 logit 普遍更大）；本步揭 m = ceil(剩余 / 剩余步数) 格；**m 格同时写入**。
3. 最后一步揭完剩余 MASK。已揭开的字不再改（第一版不做重涂）。

K=1：一次揭完全图，更容易整页同一个高频字。K 增大应出现更多局部搭配（如「猴」→「猴王」），不是更多随机词沙拉。

`infer` 必须 dump 中间帧：step 0（提示 + ░）、中间一步、final。只打 final 看不见快门。

### 3.5 训练与指标

滑动窗口切 T 字（stride 可等于 T）。不要因果 `train_step`。只在洞上 CE。

每个 epoch：

```
epoch 3  mask-acc ~20%  maj ~7%  nll 4.93  hit 48842 / 235618
         test mask-acc ~24%  nll 4.71  hit 2919 / 12126
K=1 ...
K=8 ...
```

| 字段 | 含义 |
| --- | --- |
| `mask-acc` | 猜对的洞 / 挖的洞。主指标。乱猜 ≈ 100/V |
| `maj` | 每个洞都填本批金标众数。**acc 必须明显高于 maj** |
| `nll` | 洞上平均 CE（nat） |
| `hit / n` | acc 的分子分母 |
| `test` | 后 10% 窗口，不更新 |
| `probe` | 打乱可见字后，MASK 位预测改变的比例。0 = 忽略上下文 |

不要报含可见字的 full-acc，也不要把 keep 位算进去。生成好不好看 K 画布，不计入 acc。

Python 权重：`snapshot_bert.pt`（含 vocab）。Yac：`snapshot.w`，文件头标明 bidir，禁止和 causal `transformer.w` 混用。改结构必须 `new`，不能接着袋模型的权重训。

### 3.6 和另外两个例子

| | `trigram/` | `transformer/` | `snapshot/` |
| --- | --- | --- | --- |
| 生成顺序 | 左→右 | 左→右 | 全图同时，K 步 |
| 前向次数 | O(N) | O(N) | O(K) |
| 注意力 | 无 | causal | bidirectional + 距离偏置 |
| 读出 | 下一字 | 最后位置 | 每个位置 |
| 噪声 | 无 | 无 | 随机 MASK（多档 r） |
| 先跑通 | — | Yac 整数 | **Python 浮点**，再 port Yac |

## 4. 目录与命令

```
app/llm/snapshot/
  DESIGN.md
  train_float.py      # 当前主实现（浮点、可测）
  snapshot.yac        # Yac 包：port 时对齐 Python，不要对齐旧袋模型
  train.yac / infer.yac
```

```text
python -u app/llm/snapshot/train_float.py app/llm/xyj.txt 16
python -u app/llm/snapshot/train_float.py app/llm/xyj.txt 16 new
python -u app/llm/snapshot/train_float.py infer app/llm/snapshot/snapshot_bert.pt 猴 8
```

有 checkpoint 且无 `new` 则接着训。Yac：`make -C app llm-snapshot`；Windows 函数参数 ≤5。

## 5. 第二阶段：连续帧（先写在纸上）

单帧 T 讲不了长回复。宏观取景决定下一帧，微观快门跑第一阶段。接口预留 `denoise_frame(m, prefix, t, k)`。第一版整段 = 一帧，不要先训两个网。

## 6. 超参（当前 Python 默认）

| 名 | 默认 | 含义 |
| --- | --- | --- |
| T | 64 | 底片字数 |
| d | 192 | 宽 |
| layers / heads / FF | 4 / 6 / 768 | 双向块 |
| V | ≤4096 | 字符 cap |
| K | 8 | 推理显影次数 |
| r | 75%×[0.12,0.35] + 25%×[0.45,0.75] | 训练掩码；eval 0.15 |
| freeze | 8 | 提示左侧冻结 |
| lr / wd | 3e-4 / 0.01 | AdamW |

合理现象（在 mask-acc ≫ maj、probe ≠ 0 之后才谈）：

- 提示「猴 / 唐」时生成区更常出现「王 / 僧」一类搭配，而不是均匀乱码或纯「，」。
- K=1 比 K=8 更容易整页同一个高频字。
- 冻结提示在 K 步里始终不动。

若 mask-acc 贴 maj、MASK 隐向量互相 cosine≈1：先查注意力是否袋化，再查是不是把 keep 算进了 acc。不要靠调 lr 假装在学。

## 7. 明确不做

- 连续空间扩散再 round 到词表。
- 与长度成正比的自回归循环，包括挖后缀当主任务、按左→右揭 MASK、步内写回再算下一格。
- 从 causal `transformer.w` 或旧袋模型权重热启动。
- 子词 BPE、多机数据并行（「并行」指序列维同时更新）。
- 把 trigram 计数表伪装成一步填空。
- 用邻字 one-hot 假注意力当「已经是 Transformer」。

## 8. 实现顺序

1. Python：双向块 + 每位置 logits；全 MASK 输入能跑完。
2. MLM：只计真洞的 `mask-acc` / `maj` / nll；acc 随 epoch 升且高于 maj；probe > 0。
3. MaskGIT：一步同时揭格；infer 打印中间帧；对照 K=1/8/16。
4. 训练 r 覆盖高掩码，使 generate 与训练同族。
5. 指标达标后再 port Yac（整网或等价结构，不要退回只训 Wout）。
6. （可选）帧接口。

第 2 步没过不要谈生成。第 3 步没有中间帧，快门实验不算立住。
