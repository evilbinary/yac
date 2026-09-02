## 运行

```bash
make -C app llm-trigram
./app/llm/trigram/trigram app/llm/corpus.txt 你好
```

```bash
make -C app llm-transformer
./app/llm/transformer/train.exe app/llm/corpus.txt
./app/llm/transformer/infer.exe app/llm/transformer/transformer.w 你好
```

实验性「快照并行」（MaskGIT / 双向填空）见 [snapshot/DESIGN.md](snapshot/DESIGN.md)。

```bash
make -C app llm-snapshot
# 默认 chat：冻用户、填回复。已有 snapshot.w 则接着训；每个 epoch 写盘。
./app/llm/snapshot/train.exe app/llm/data.jsonl
./app/llm/snapshot/train.exe app/llm/data.jsonl app/llm/snapshot/snapshot.w 3
# 丢掉旧权重从头来
./app/llm/snapshot/train.exe app/llm/data.jsonl app/llm/snapshot/snapshot.w 3 new
./app/llm/snapshot/infer.exe app/llm/snapshot/snapshot.w 怎么办 8
# 旧整库 MLM
./app/llm/snapshot/train.exe app/llm/data.jsonl app/llm/snapshot/snapshot.w 3 mlm
./app/llm/snapshot/infer.exe app/llm/snapshot/snapshot.w 怎么办 8 mlm
```
