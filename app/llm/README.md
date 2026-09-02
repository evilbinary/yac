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
# 默认 MLM 预训练 + generate。语料是 pretrain_t2t_mini.jsonl 切出的前 40 行。
./app/llm/snapshot/train.exe
./app/llm/snapshot/train.exe app/llm/pretrain_head.jsonl app/llm/snapshot/snapshot.w 5 new
./app/llm/snapshot/infer.exe app/llm/snapshot/snapshot.w 秋天 8
```
