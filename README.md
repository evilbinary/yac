# Yac

用 C 实现的小型纯函数式语言，可在两种显式求值顺序的 IR 上直接运行：

- **ANF**：求值顺序编码进 `let`，控制流隐式 —— 直接风格机器。
- **CPS**：求值顺序编码进续延，控制流完全显式，`callcc`/`throw` 原生支持。

同一源程序可翻译为 ANF 或 CPS 分别执行，共享同一套值语义。唯一语义差异：**ANF 无法表达一流续延，CPS 可以**。

## 特性

- CBV、n 元函数、词法作用域、递归 `let`、真 TCO（10⁷ 尾调用不爆栈）
- 整数/浮点/布尔/字符串/`()`，原语 `+ - * / % == != < <= > >= and or not print`，**支持 UTF-8 中文标识符**
- `callcc` / `throw`（CPS 机器）
- mark-sweep GC + **扁平环境帧**（`(depth,slot)` 下标寻址，`let` 直写槽位）
- ANF ↔ CPS 双向转换、受限 un-CPS、CPS 化简（`--opt`）
- 运行时持久化：编译产物 dump/load、执行中间快照（checkpoint/resume）、REPL

## 构建与测试

```sh
make          # 编译，产物 yac
make test     # 回归测试（29 项）
make prop     # 属性测试：随机程序比对 ANF/CPS/--opt
```

## 用法

```sh
yac file.yac              # 默认走 ANF 解释器
yac --cps file.yac        # 走 CPS 解释器
yac --both file.yac       # 两个解释器各跑一遍并比对
yac --dump-anf file.yac   # dump ANF / --dump-cps / --dump-uncps
yac --opt --cps file.yac  # CPS 化简（常量折叠/eta 归约）
yac --repl                # 交互式 REPL（持久全局，--cps 支持 callcc）
yac --dump-rt f.yacrt file.yac   # 序列化编译产物（ANF）到文件
yac --load-rt f.yacrt            # 直接加载运行，跳过解析
yac --checkpoint-at 100 file.yac # 在第 100 步保存运行时快照并暂停
yac --resume yac.ckpt            # 从快照恢复执行
```

## 示例

```yac
let fact(n) =
  if n <= 1 then 1 else n * fact(n - 1)
in
fact(10)                          -- 3628800
```

```yac
let k = callcc(fun (k) -> k) in
throw k 42                        -- 只有 CPS 机器能跑：yac --cps file.yac
```

## 文档

- [docs/DESIGN.md](docs/DESIGN.md) — 语言与 IR 的完整设计
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — 模块结构、管线、测试、里程碑

## 编辑器

VS Code / Cursor 语法高亮扩展见 [tools/vscode-yac](tools/vscode-yac)：

```sh
./tools/vscode-yac/install.sh cursor   # 或 code
# 然后 Developer: Reload Window
```
