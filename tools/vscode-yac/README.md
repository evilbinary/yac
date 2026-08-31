# Yac Language Support

VS Code / Cursor 扩展：为 `.yac` 提供语法高亮、括号配对与注释切换。

## 高亮内容

- 关键字：`let` `in` `fun` `if` `then` `else` `callcc` `throw` `package` `export` `import` `and` `or` `not` `print`
- 字面量：整数 / 浮点 / `true` `false` / 字符串 / `()`
- 注释：`--` 行注释、`/* */` 块注释（空白后的 `/*` 可嵌套，因此 `rt/*.yac` 不会误开一层）、`/** **/`
- 运算符：`+ - * / % == != < <= > >= = -> =>`
- 内建：`cons` `map` `foldl` `str_len` `bytes_new` 等原语
- 标识符：ASCII 与 UTF-8（含中文名）

## 安装（本仓库）

在仓库根目录执行其一：

```sh
# Cursor
./tools/vscode-yac/install.sh cursor

# VS Code
./tools/vscode-yac/install.sh code
```

或手动软链到扩展目录后重载窗口（`Developer: Reload Window`）：

```sh
ln -sfn "$(pwd)/tools/vscode-yac" \
  "$HOME/.cursor/extensions/yac-lang.yac-0.1.1"
# 若仍无高亮：打开 ~/.cursor/extensions/.obsolete，删掉 yac-lang.yac-* 后再重载
```

## 开发

改 `syntaxes/yac.tmLanguage.json` 或 `language-configuration.json` 后重载窗口即可。
