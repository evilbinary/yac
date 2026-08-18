# Yac

一个用 C 实现的小型纯函数式语言，核心设计目标是在**两种显式求值顺序的中间表示（IR）上直接运行**：

- **ANF（A-Normal Form）**：求值顺序编码进 `let` 绑定，控制流仍然隐式 —— "直接风格"的机器。
- **CPS（Continuation-Passing Style）**：求值顺序编码进续延（continuation），控制流完全显式，`callcc`/`throw` 原生支持。

同一个源程序可以被翻译成 ANF 或 CPS，由两个解释器分别执行，二者共享同一套值表示与语义。唯一的语义差异：**ANF 解释器无法表达一流续延，CPS 解释器可以**。

## 特性

- CBV、n 元函数、词法作用域、递归 `let`（bind-then-fill）
- 整数（int64）、浮点、布尔、字符串、`()` 单元值
- 原语：`+ - * / % == != < <= > >= and or not print`
- **尾调用优化**：两个解释器都不让 C 调用栈增长，长尾递归不爆栈（10⁷ 级压测通过）
- `callcc` / `throw`（仅 CPS 机器支持）
- mark-sweep GC（运行时闭包/环境/帧/参数数组），可 `--no-gc` / `--limit-nodes` 调试
- ANF ↔ CPS 转换，以及受限的 **un-CPS**（CPS → ANF 往返）

## 构建

需要 gcc（msys2 mingw64 或任意 C11 编译器）。

```sh
make            # 编译，产物 yac，中间文件在 build/
make test       # 运行回归测试（29 项）
make clean
```

## 用法

```
usage: yac [options] file.yac
  --cps            run via the CPS interpreter
  --dump-anf       print the ANF and exit
  --dump-cps       print ANF->CPS and exit
  --both           run both interpreters and compare
  --uncps          run ANF->CPS->ANF (un-CPS round trip) and execute
  --dump-uncps     print the un-CPS'd ANF and exit
  --ast            dump the parsed AST and exit
  --no-gc          disable garbage collection (arena-style growth)
  --limit-nodes N  abort when live objects exceed N (0 = unlimited)
```

默认走 ANF 解释器；`--cps` 走 CPS 解释器。对普通程序两者结果一致，可用 `--both` 交叉验证。

## 语言示例

```yac
-- 阶乘：普通程序，ANF/CPS 都能跑
let fact(n) =
  if n <= 1 then 1 else n * fact(n - 1)
in
fact(10)                          -- 3628800
```

```yac
-- 使用 callcc：只能跑 CPS 机器（yac --cps file.yac）
let k = callcc(fun (k) -> k) in
throw k 42                        -- 直接跳到程序出口，输出 42
```

```yac
-- 用 callcc 提前退出（跳出多层递归）
let exit = callcc(fun (k) -> k) in
let f(n) = if n > 100 then throw exit 999 else f(n+1) in
f(0)                              -- 999
```

## 目录结构

```
src/
  main.c          CLI 驱动
  lexer.c/h       词法
  parser.c/h      语法 → AST
  ast.c/h         AST 节点与 dump
  anf.c/h         AST → ANF 归一化
  cps.c/h         ANF → CPS
  uncps.c/h       CPS → ANF（受限，可失败）
  value.c/h       Value/Closure/Prim 与原语实现
  env.c/h         环境
  eval_anf.c/h    ANF 解释器（帧栈 + 蹦床，拒绝 callcc）
  eval_cps.c/h    CPS 解释器（纯蹦床，支持 callcc/throw）
  gc.c/h          mark-sweep GC（运行时对象 + 值根栈）
  gcobj.h         GC 对象头
  arena.c/h       区域分配（IR 与字符串字面量）
tests/            回归测试（run_tests.sh + *.yac）
DESIGN.md         完整设计文档
```

## 管线

```
源程序 → lexer → parser → AST → ANF → CPS
                              │        │
                              ▼        ▼
                          eval_anf  eval_cps
                              └──┬───┘
                          （--both 比对）
```

每层都可以独立 dump（`--dump-anf` / `--dump-cps` / `--dump-uncps`）。

## 测试

`make test` 运行 29 项回归测试：

- 普通程序在 ANF 与 CPS 下结果一致（golden + parity）
- 10⁷ 次尾调用不爆栈（ANF / CPS / un-CPS 各一遍）
- `callcc` 四例：逃逸、递归提前退出、捕获后使用、函数内逃逸
- un-CPS 往返一致；callcc 被 un-CPS 正确拒绝
- GC：循环分配垃圾时内存有界；`--no-gc` 触发 `--limit-nodes` 保护

## 状态

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M1 | lexer、parser、AST→ANF、ANF 解释器 | ✅ |
| M2 | ANF→CPS、CPS 解释器、`callcc`/`throw` | ✅ |
| M3 | mark-sweep GC、un-CPS、`--dump-*` | ✅ |
| M4 | 扁平环境快照、CPS 化简、属性测试、文档 | ⏳ |

详见 `DESIGN.md`。