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
# 默认 corpus.txt（小词表）。jsonl 头仍可手动指定。
./app/llm/snapshot/train.exe
./app/llm/snapshot/train.exe app/llm/corpus.txt app/llm/snapshot/snapshot.w 8 new
./app/llm/snapshot/infer.exe app/llm/snapshot/snapshot.w 你好 8
```
