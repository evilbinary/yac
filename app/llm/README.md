## 运行

```bash
make -C app llm-trigram
./app/llm/trigram/trigram app/llm/corpus.txt 你好
```

```bash
make -C app llm-transformer
./app/llm/transformer/train app/llm/corpus.txt
./app/llm/transformer/infer app/llm/transformer/transformer.w 你好
```

实验性「快照并行」（MaskGIT / 双向填空）见 [snapshot/DESIGN.md](snapshot/DESIGN.md)。当前先用 Python 证明填空，再 port Yac；不要按旧的 T=16 袋模型当目标。

```bash
python -u app/llm/snapshot/train_float.py app/llm/xyj.txt 16
python -u app/llm/snapshot/train_float.py infer app/llm/snapshot/snapshot_bert.pt 猴 8
```

```bash
make -C app llm-snapshot
./app/llm/snapshot/train app/llm/xyj.txt app/llm/snapshot/snapshot.w 8 new
./app/llm/snapshot/infer app/llm/snapshot/snapshot.w 悟空 8
```
