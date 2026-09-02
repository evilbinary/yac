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

实验性「快照并行」（扩散 / MaskGIT，尚未实现）见 [snapshot/DESIGN.md](snapshot/DESIGN.md)。
