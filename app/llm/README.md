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
# 默认西游记中段 80KB；词表 top-256 + MASK，必须 new。
./app/llm/snapshot/train.exe app/llm/xyj.txt app/llm/snapshot/snapshot.w 8 new
./app/llm/snapshot/infer.exe app/llm/snapshot/snapshot.w 悟空 8
```
