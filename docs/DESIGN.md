# Yac — 一个可运行 ANF 与 CPS 的语言设计

> ## 本文范围（先读这里）
>
> **本文是"语言 + 两个 IR 的设计"文档**：源语言、ANF 与 CPS 两套机器、以及把
> 两者跑起来的 C 解释器设计。它的叙述价值在于**"为什么"**（为什么两个机器、
> 为什么 CPS 里 callcc 是免费的、为什么用 C 实现）。
>
> **形式定义已经拆到各层文档**，本文只留摘要与指针：
>
> | 想要什么 | 去哪 |
> |---|---|
> | **ANF 语法 / 不变量 / ANF → LIR** | `docs/ANF.md` ← 权威 |
> | **LIR 语法 / 指令集 / LIR → 机器码 / 校验规则** | `docs/LIR.md` ← 权威 |
> | Flat ABI、静态化、闭包表示阶梯、call 收敛 | `docs/FLAT_ABI.md`（待实现的设计） |
> | 目录结构 / 包与编译单元 / 管线 / CLI / 测试 | `docs/ARCHITECTURE.md` |
> | 自举路线图 / 编译单元 / 运行时边界 / 里程碑 | `docs/SELFHOST.md` |
> | 内存映像格式 / 段 / Reloc / 冷启动·追加 | `docs/JIT_IMAGE.md` |
> | 链接模式（embed / dylib / yjit / stub） | `docs/BOOTSTRAP_LINK.md` |
>
> **本文里标了「C 解释器路线」的小节**（§4.3 / §5.3 / §6 / §8 / §10 / §11 / §12）
> 描述的是**C 解释器**这一侧的实现与工程管理；自举编译器（`src-self/`）那一侧
> 以 `SELFHOST.md` / `ARCHITECTURE.md` 为准。
>
> **CPS 只在 C 解释器里存在** —— 自举编译器 `yc` 不经过 CPS（见 §2 的路径表）。

## 1. 目标

Yac 是一个小型、纯粹的函数式语言，其核心设计目标是**在两种显式求值顺序的中间表示（IR）上直接运行**：

- **ANF（A-Normal Form）**：把求值顺序编码进 `let` 绑定，控制流仍然隐式。这是"直接风格"的机器。
- **CPS（Continuation-Passing Style）**：把求值顺序编码进续延（continuation），每个函数多收一个续延参数，控制流完全显式。这是"全控制风格"的机器。

同一个源程序可以被翻译成 ANF 或 CPS，并分别由两个解释器执行。二者共享同一套值表示与语义，唯一的语义差异是：

> **ANF 解释器无法表达一流续延（callcc）；CPS 解释器可以。**

这给出了一条贯穿全文的主线：*控制流在 ANF 里是"语法结构"，在 CPS 里是"数据"*。CPS 里续延只是普通的值，因此 `callcc` 几乎不需要额外机制——它只是把"当前续延"当作参数交给函数。

### 设计原则

1. **求值顺序显式化**：程序里"先算什么、后算什么"在 IR 里一目了然。
2. **尾调用即跳转**：两个解释器都不让 C 调用栈增长，长尾递归不会爆栈。
3. **单一值表示**：ANF 与 CPS 共用一套 `Value` 结构，解释器之间可以互相转换运行。
4. **可验证**：对同一程序，`evalANF(anf(p)) == evalCPS(cps(anf(p)))`，用属性测试保证翻译正确性。
5. **用 C 实现**：作为系统语言，C 要求我们把"堆对象、闭包、续延、GC"全部显式设计出来，正适合作为教学/原型核心。



## 2. 总体架构

```
                 ┌──────────┐    ┌─────────────┐    ┌───────────┐    ┌─────────────┐
  源程序  ──────▶│ lexer    │───▶│ parser      │───▶│ AST       │───▶│ ANF 归一化  │───┐
                 └──────────┘    └─────────────┘    └───────────┘    └─────────────┘   │
                                                                                       ▼
                                                                                 ┌───────────┐
                                                                                 │ ANF IR    │──▶ eval_anf ──▶ 结果
                                                                                 └─────┬─────┘
                                           ┌───────────────────────────────────────────┴────┐
                                           │                                                │
                                           ▼ CPS 转换                                       ▼ LIR转换
                                     ┌───────────┐                                    ┌───────────┐
                                     │ CPS IR    │──▶ eval_cps ──▶ 结果               │ LIR       │
                                     └─────┬─────┘                                    └─────┬─────┘
                                           │                                                │
                                           ▼ un-CPS（受限）                                 ▼low emit-*
                                     ┌───────────┐                                    ┌───────────┐
                                     │ ANF IR    │                                    │ 机器码    │──▶ ELF / PE / Mach-O
                                     └───────────┘                                    └───────────┘
```

路径：

- `source → AST → ANF → CPS`（C 解释器：`eval_anf` / `eval_cps`）
- `CPS → ANF`（**un-CPS**，仅在「续延不逃逸」时成立）
- `source → AST → ANF → LIR → 机器码`（自举编译器 `yc`，不经过 CPS）
- 每层都可以独立 dump（`--dump-anf` / `--dump-cps`；LIR 为 yac list）



### 2.1 机器码路径：ANF → LIR → 二进制

```
                       ┌───────────┐
                       │ ANF IR    │
                       └─────┬─────┘
                             │
                             ▼ anf ir-转lir
                       ┌───────────┐
                       │ LIR       │── 槽机：mov / add / mref / fcall / syscall …
                       └─────┬─────┘
                             │
                             ▼ lower emit-x86_64 / emit_arm64 / emit_riscv64
                       ┌───────────┐
                       │ 机器码    │── 1..n 条目标指令字节
                       └─────┬─────┘
                             │
                             ▼ pack
                       ┌──────────────────┐
                       │ ELF / PE / Mach-O│
                       └──────────────────┘
```

三层都是 **yac list**（除最后的容器字节）。槽号 `s` 是整数虚寄存器；emit 映到 `[rbp+off]` / FP 帧。值在 LIR/机器码里是 tagged：int 为 `n<<1`，堆指针为奇数。

#### ANF（`anf.yac` 实际产出）

> **ANF 的权威定义见 `docs/ANF.md`**：完整语法、8 条不变量、**ANF → LIR 的完整转换
> 规则**、本次修正、`letrec` 预留、与经典 ANF 及 Chez 的对照。本节只留轮廓。

一个表达式 = **绑定序列 + 尾**；顶层程序 = 这种 body 的列表。

```
body      ::= [ bind*, tail ]
tail      ::= ["atom", atom] | ["call", atom, [atom*]]
bind      ::= let | letbin | letcall | letif | letfun | letthrow | letcallcc | letrec
atom      ::= int | float | str | bool | unit | nil | var | qvar
```

四点要知道的（细节全在 `ANF.md`）：

1. **`atom` 的求值是空操作**，所以**不含函数字面量** —— 函数字面量在会分配闭包的
   编译器里是一次**计算**。`anf.yac` 把它直接降成 `["letfun", tN, ps, body]` +
   `["var", tN]`（旧版曾在 `atom` 里留一个 `["fun", …]`，但三个消费者对它不一致：
   生产者不产出、`free_vars` 处理有 bug、`lir_atom` 静默返回槽 0 —— 已删，见
   `ANF.md` §5.1）。
2. **尾位置是结构，不是模式。** `tail` 只有两种形态，因此**不需要**旧的 `tail(x)`
   谓词（"最后一条 `letcall` + 尾原子是它"），LIR 层的 `maybe_tcall` 事后改写也一并
   作废 —— 顺带消掉 `letif` 两支里那段永不执行的 `mov` / `jmp`（`ANF.md` §4.1）。
3. **`letif` 是一个 `bind` 而不是尾形式。** 它天然是 join point，避免经典 ANF 里
   "取一个条件表达式的值就得复制后续代码"的问题。**这是相对教科书形式最实质的一处
   偏离**（`ANF.md` §6）。
4. **`["letrec", [fnbind*], body]` 是预留的互递归函数组**，`anf.yac` 暂不产出；
   消费端遇到它**必须报错**，不能落进"未知 bind 静默跳过"的兜底（`ANF.md` §5.3）。

`print e` 在 parser 里降成 `call print(e, true)`。

#### LIR（跨架构，接近机器）

> **LIR 的完整定义见 `docs/LIR.md`（权威）**：定位与不变量、完整指令集
> （逐条标注生产者）、`$` 家族规格、闭包在 LIR 的落点、后端契约、
> 校验规则、一致性问题清单与改动清单。
>
> 本节旧版曾在此列一份 LIR 语法，与 `emit_x86_64.yac` 的分派链**双向漂移**
> （列了 `nop`/`save`/`restore`/`neg`/`lnot`/`print` 这些 emit 没有分支的，
> 又漏了实际在用的）。已改为单一来源。

本节只记 **ANF → LIR 的翻译约定**：

- `mov_imm` 写入**已经编码好的** 64 位模式（翻译时完成 tag：int 为 `n<<1`，`nil` 为 `1`）。
- 源级 `nth` / `cons` / `len` / `str_cat` / `foldl` / `map` / `bytes_*` / `argc` / `argv` /
  `time_*` / `read_file` / `write_file` **不是 LIR 指令**，一律 `fcall yac_*`。
  例外（emit 直接展开）：`str_len` / `str_ref` / `bytes_len` 是对象字段读取（同 `mref`）。
- 用户 `+` / `-` / `*` / `/` 走 `yac_num_*`（整数 insn 快路径，否则 `yac_num_slow`）。
  backend 把 `num.yac` 的 `let` ANF→LIR 后经 `runtime_add` 挂进客镜像。
- `ccall("name", …)` 降成 LIR `ccall`（C ABI / libc）。
- `exit` 是 emit 糖（untag + 架构 exit）。`_start` 走 `untag` + `syscall 60`；
  `syscall` 的 `nr = 60` 表示进程退出（arm64/riscv64 映成 93）。
- `apply` / `tailapply` 带**已知** `ncap`；`icall` / `ticall` **无 ncap**
  （运行时读 `[obj+24]`）。

> ⚠️ **上面"不是 LIR 指令，一律 `fcall yac_*`"这条与 emit 现状矛盾**：emit 里有
> 一整套内联原语实现（`cons` / `len` / `nth` / `map` / `str_cat` / `bytes_*` …
> 共 26 条），但**全仓没有任何生产者**。这是一个必须先做的决策 ——
> 见 `docs/LIR.md` §4.10。

#### ANF → LIR（`lir.yac` 的 `lir_expr`，对标 `anf_expr`）

> **完整规则见 `docs/ANF.md` §3** —— 原子 / 绑定 / `letif` join / 调用 / `letfun` /
> body·程序 的完整判断表（含 `tail?` 与 `callι`）。本节不再重复列举。

三句话概括这条转换：

1. **`atom` → 一次装载。** `int` / `bool` / `unit` / `nil` → `mov_imm`（tag 在翻译期
   完成，int 为 `n<<1`）；`str` → `strlit`；**`var` 不产生指令** —— 直接复用 `Γ(x)`
   的槽，所以 `let y = t` 这类别名是**零开销**的。
2. **`bind` → 一条计算 + 一次绑槽。** `letbin` 展开成两个操作数 + 一条 `bin(op,…)`；
   `letcall` 展开成被调者 + 各实参 + 一条 `callι`；`letif` 展开成 `cmpjmp` + 两支 +
   join —— **分支以 `["call", …]` 结尾时 join 是死代码，直接省略**。
3. **`letfun` → 一个 `proc` + 一次闭包分配。** `caps = FV(body) \ ({f} ∪ ps)` 决定
   `ncap` 与槽布局（`1..ncap` 捕获、`ncap+1..ncap+|ps|` 形参），`I_out` 在外层帧发
   `["closure", s, f, [Γ(caps_i)]]`。

转换所需的上下文：`Γ : name → slot`（词法环境）、`self`（当前过程码名，用于 TCO
判定）、`Σ`（已定义过程 → `{ncap}`）。

**尾位置由结构给出** —— `tail?` 为真当且仅当该调用出现在 `tail` 非终结符里，因此旧
的 `tail(x)` 谓词与 LIR 层的 `maybe_tcall` 事后改写**都不再需要**（见 `ANF.md` §4.1）。

三处待改（细节在 `ANF.md` §3.4 与 `LIR.md` §5.1 / §9.3 / §11 C2）：

- `callι` 现在还产出 `fcall` / `xcall` / `icall` / `apply` / `ticall` / `tailapply`
  六种形态；**目标是收敛成 `call` / `tcall` / `ccall` + `caps` 字段**。
- `+ - * /` 每次都走一次运行时过程调用（`yac_num_*`）；将来要做内联的 int 快路径
  + 溢出检查。
- `Σ` 里找不到名字时落到 `xcall`，这会把**同单元的前向引用**误判为跨镜像引用。



#### 机器码（不是第三种 IR）

> **完整内容见 `docs/LIR.md` §7「LIR → 机器码」**：三段流水线（指令选择 + 框架 →
> patch 求解 → 容器打包）、编码层的语法范式、**patch 语言（tag 1–23）**、
> 指令选择表、**TCO 的三条路径**、`fn_entry` 策略点。
>
> 权威分工：**镜像 / 容器格式** → `docs/JIT_IMAGE.md`（Header v1、
> TEXT/RODATA/DATA/LINK 段、Reloc、冷启动 / 追加、W^X）；**链接模式**
> → `docs/BOOTSTRAP_LINK.md`。

三条最容易踩的约定（细节以 `LIR.md` §7.4 为准）：

- 槽 `s` → 帧上 8 字节格；临时值走返回寄存器（x86 `rax`，arm `x0`，riscv `a0`）。
- 内部 yac ABI：x86_64 前 6 个寄存器（`rdi rsi rdx rcx r8 r9`）其余入栈
  （callee 见 `[rbp+16+…]`）；arm64/riscv64 前 8 个。
- `-g` / `--syms` 时 pack 写 `.symtab` / `.strtab`；默认不写，无 DWARF 行号。



## 3. 源语言

调用约定为 **call-by-value**，n 元函数，无高阶类型变量（核心阶段不设类型系统，见 §9）。

### 3.1 语法

```
program   ::= top*
top       ::= package | import | export
            |  let name = expr          -- 全局绑定（可为递归）
            |  expr                    -- 表达式，最后一个表达式的值即程序结果

expr      ::= integer | float | true | false | string | ()
            |  name
            |  e e ...                 -- n 元函数应用
            |  fun (name*) -> e        -- n 元 lambda
            |  if e then e else e
            |  let name = e in e'
            |  e binop e               -- 中缀原语：+ - * / % == != < <= > >= and or
            |  not e
            |  print e
            |  callcc e                -- 捕获当前续延（CPS 特性）
            |  throw e e'              -- throw k v：向续延 k 投值 v（CPS 特性）

binop     ::= + | - | * | / | % | == | != | < | <= | > | >= | and | or
```

注释：`--` 单行，`/* ... */` 块注释。

### 3.2 语义要点

- 整数为 64 位（`int64_t`），浮点为 `double`，布尔为真/假，字符串为字节串。
- `print` 打印并返回原值（保持表达式性质）。默认换行；`print(e, false)` 不换行。
- `callcc f`：`f` 是一元函数，收到一个**当前续延**（一个一等值）；调用 `throw k v` 即以 `v` 作为整个 `callcc` 表达式的结果跳回。
- 顶层最后一条 `tail` 的结果就是程序退出值（CPS 侧对应 `halt`）。

### 3.3 包（语言层：`package` / `import` / `export`）

> **权威见 `docs/ARCHITECTURE.md` §包与编译单元**（一包一文件、`import` 图、
> 查找根、三层库）；编译单元的物理切分见 `docs/SELFHOST.md` §5.3。
> 本节保留**语言层**的规则与理由。

包是**命名空间与信息隐藏**，不是链接或版本边界。物理切分（CRP/CCP、是否进镜像）见 `docs/SELFHOST.md` 的「编译单元」；语言里没有 `unit` 关键字。不要把 `import` 和 PE/ELF 的 `cimport` 混为一谈。

**一包一文件（硬规则）。** 一个 `package` 名严格对应一个 `.yac`。禁止粗包：不得多个文件写同一个 `package emit` / `package front` 并指望同名可见。编译器源（`src-self`）与客程序同一套规则；`yc` 是正经程序，用 `import` 组装，不靠 `cat` 当语言语义。

规则：

- `package` 名 = 相对包根的路径（`.` → `/`），且必须与文件一致：`package back.emit.emit_x86_64` 只属于 `back/emit/emit_x86_64.yac`。`import P` 只打开那一个文件。目录只是路径前缀，不是「一目录多文件同一包」。
- 跨文件名字只能 `import`。包内可前向引用**该文件**里的顶层绑定；另一文件即使曾共用短名，也不是同包。
- 未 `export` 的名字只在该文件内可见。LIR 键是 `包/名`（`包` 即 `package` 行，与 import 路径相同）；`rt.*` 仍用裸名（内核 `fcall yac_num_slow`）。
- `import` 只引入该文件的导出集。没有默认 `import *`。不要按包名扫描其它 `.yac`，不要在绑定时合并多个文件的环境。
- `import P as a` 只绑定模块名 `a`，不把导出放进当前作用域；用 `a.x`（AST `qvar`）。
- `import P { x as y, z }` 把 `y`/`z` 放进当前作用域（`z` 未改名则仍叫 `z`）。
- 查找根（每个根下再拼 `rt/os.yac` 这类相对路径）：`--pkg DIR[,DIR...]`（从左到右）、当前目录 `pkg/`、再是 `yc` / `yac` 可执行文件所在目录。空段（`a,,b`）为错误；`--pkg` 只能出现一次。本仓库开发用 `--pkg src-self`（提供 `rt.*` 与编译器包）；`./pkg` 提供常见库。独立项目把 `rt/` 放在 `yc` 旁边，或 `--pkg` 指向含 `rt/` 的根。
- 原语名（`cons`、`ccall`、`str_cat` 等）走 `is_prim_name`，不是包。客库不得再导出这些名字。
- 没有 `package` 的文件属于匿名主包（入口，如 `yc.yac` 或客的 `main.yac`）。主包通过 `import` 拉依赖；默认仍链 `kernel`+`rt.num`。

三层库（不要混）：

| 层 | 名字 | 位置 | 用法 |
|---|---|---|---|
| 内核 | 无包名 | `src-self/rt/runtime.yac` 进镜像 | `print` / `cons` / `read_file` 等原语 |
| 语言运行时 | `rt.*` | `src-self/rt/{num,os,ffi}.yac` | `import rt.os`；客默认还链 `rt.num` |
| 常见库 | 短名，不用 `rt` 前缀 | 仓库根 `pkg/*.yac` | `import path`；项目自己的库也放 `./pkg` |

`src-self/lib`（`lib.log` / `lib.map` / `lib.pass`，各一文件）只给编译器用，不是客库。不要用包名 `os`（与 `rt.os` 冲突）。编译器后端同理：`back.emit.emit`、`back.emit.emit_x86_64`、`back.encode.encode_x64` 各是一包；交叉编译器的入口 import 它需要的 arch 文件，而不是一个叫 `emit` 的粗包。

`rt.*` 维持现状：`rt.num`（慢路径算术，默认链）、`rt.os`（`uname` / `host_*`）、`rt.ffi`（`cload` / `csym`）。不要再拆 `rt.str` / `rt.list`（已是原语）。

常见库按需增加，不一次写完：

- 已落地：`path`、`str`、`io`、`list`、`hash`、`fmt`、`log`、`test`、`net`、`bytes`、`ffi`、`json`、`env`、`cli`、`http`（`import http` 会链 `net`）、`yui`、`math`（整数 gcd/pow/isqrt；f64 用牛顿/泰勒，不链 libm）。`json` tagged：`["N",n]` / `["S",s]` / `["A",xs]` / `["O",pairs]` / `["T"]` `["F"]` `["Z"]`。
- 先不要：`re` / `crypto` / `thread`。

包查找器（`backend.yac` 的 `pkg_src`）本身不能 `import path` / `import io`，否则加载 `path.yac` 会循环。

语法（`program` 的顶层还可出现下列形式；`as` 不是关键字）：

```
package   ::= package ident ("." ident)*
import    ::= import ident ("." ident)*
            | import ident ("." ident)* "{" import_spec ("," import_spec)* "}"
            | import ident ("." ident)* as ident
import_spec ::= ident | ident as ident
export    ::= export ident ("," ident)*
```

`import P as a` 只引入模块别名：`a.x` 解析为 `P/x`。`import P { x as y, z }` 只引入列出且确为导出的名字（`y` 是本地名）。不要写 `import P as a { ... }`。

绑定检查（客与 `yc` 相同）：内核名（`runtime_funs`）加 `import` 的本地名（别名或选出的导出）。未 import 则 `host_os` / `cload` 为未绑定。链接按 import **及被 import 的那一个文件里的 import**（`http` 会链 `net`）：默认 `kernel`+`rt.num`，再 DFS 依赖。`os_has` 留在包 `rt.os` 且不导出。imap / LIR 用 `package` 名，不用「粗包短名」。


### 3.4 示例

```
-- 阶乘：普通程序，ANF/CPS 都能跑
let fact(n) =
  if n <= 1 then 1 else n * fact(n - 1)
in
fact(10)                          -- 3628800
```

```
-- 使用 callcc：只能跑 CPS 机器
let k = callcc(fun (k) -> k) in   -- k 绑定到"当前续延"
throw k 42                        -- 直接跳到程序出口，输出 42
```

```
-- 用 callcc 提前退出（跳出多层递归）
let exit = callcc(fun (k) -> k) in
let f(n) = if n > 100 then throw exit 999 else f(n+1) in
f(0)                              -- 999
```



## 4. 核心 IR 之一：ANF

> **权威定义见 `docs/ANF.md`**（语法 / 不变量 / ANF→LIR / 与经典 ANF 的差异）。
> 本节只保留叙述性的"为什么"，形式定义以 `ANF.md` 为准。

ANF 的核心理念：**"计算"与"绑定"分离**。原子值（Atom）无副作用、无需再求值；一切计算都绑定到变量后再继续。

### 4.1 与经典 ANF 的差异

**完整对照见 `docs/ANF.md` §6**（含经典 ANF 的原文引用与逐条理由）。一句话：yac 有意
偏离教科书形式三处，其中最关键的是 **`letif` 是一个 `bind`，不是尾形式** —— 经典 ANF
的 `if` 只在尾位置，要"取一个条件表达式的值"就必须复制后续代码
（`if A then (let x=… in E) else (let x=… in E)`），而 `letif` 天然是 **join point**。

另两处：

- `bind` 的种类就是运算种类（`letbin` / `letcall` / `let`），省掉"这个 callee 是不是
  原语"的判断。
- `halt A` 换成 `tail = ["atom", a] | ["call", f, as]`，于是**尾位置成为结构**，而不是
  事后辨认的模式 —— 旧版的 `tail(x)` 谓词与 LIR 层的 `maybe_tcall` 一并作废。

`letrec` 已预留（`["letrec", [fnbind*], body]`），`anf.yac` 暂不产出、消费端须报错；
**单**递归用 `letfun` + 按名自引用（`ANF.md` §5.3）。

### 4.2 说明

- **原子（Atom）不会触发求值**：变量、字面量求值结果立即可得。
- `bind` 的**右侧操作数全是原子**，所以被绑的名字一定是一个"已算好的值" —— 求值顺序
  在语法里被写死。**别名 `let y = t` 因此是零开销的**：共享槽、不发指令。
- `tail` 里的 `["call", …]` **不绑定结果**，直接转移控制权 —— 尾调用优化在这里是
  **语法事实**，不需要任何分析。
- 闭包由 `letfun` **绑定**，不作为 atom 出现（理由见 `ANF.md` §5.1）。
- 8 条不变量的完整清单见 `ANF.md` §2。



### 4.3 ANF 解释器（eval_anf）

一个直接的树遍历器：按顺序求值每条 `bind`、扩展环境，最后求值 `tail`；尾调用直接替换状态、进入循环，**C 调用栈不增长**。

```
eval(body = [binds, tail], env):
  for b in binds:                                   ; 顺序固定 = 求值顺序
    case b:
      ["let", x, a]           → env ⊢ x = atom(a)
      ["letbin", x, op, a, b] → env ⊢ x = do_bin(op, atom(a), atom(b))
      ["letcall", x, f, as]   → env ⊢ x = apply(atom(f), atom(as))
      ["letif", x, c, bt, be] → env ⊢ x = eval(atom(c) ? bt : be)    ; 两支是 body
      ["letfun", f, ps, bd]   → env ⊢ f = closure(ps, bd, env)       ; 捕获当前 env
      ["letthrow", x, k, v]   → 见下：ANF 机器拒绝
      ["letcallcc", [k, r], f, bd] → 见下：ANF 机器拒绝
  case tail:
    ["atom", a]     → atom(a, env)
    ["call", f, as] → tail jump: apply(atom(f), atom(as))            ; 替换状态，不压栈
```

**ANF 机器不能跑的构造**：`callcc` / `throw`。源程序中若出现它们，ANF 归一化阶段会为它们生成 `callcc(A)` / `throw(A,A')` 节点；ANF 解释器遇到时**直接报错**："callcc/throw 只能在 CPS 模式下运行"。这就是两套机器语义差异的落点。

## 5. 核心 IR 之二：CPS

CPS 的核心理念：**求值顺序即续延，续延即值**。每个函数多收一个续延参数 `k`；函数从不"返回"，只调用 `k` 传递结果。

### 5.1 语法

```
CVal V ::= x | lit | prim | λ(x*, k).C            -- 值；k 是续延参数
CExp C ::= let x = V in C
        |  V V*                                    -- 尾调用：f a₁…aₙ k，或 k v
        |  if V then C else C
        |  halt V
```

- 应用 `V V*`：最后一个实参是**续延**。若头部是函数，则调用它并把其余参数和续延传进去；若头部是续延值，则是一次**跳转**（等价于 `throw k v`）。
- 一个**程序** = 一个 CPS 表达式 + 一个初始续延 `halt`（打印/返回结果）。
- `let x = V in C` 中的 `V` 是原子值，不求值、不调用——所有"副作用性"计算都发生在应用位置。



### 5.2 关键观察：callcc 在 CPS 里是免费的

因为续延就是普通值：

```
⟦callcc f⟧   ≡   f k        -- k 就是本调用点的续延参数
⟦throw k v⟧  ≡   k v        -- 对续延值做一次尾调用
```

实现时，`callcc` 和 `throw` 可以就是两个原语：

- `callcc(f)`：把机器当前的续延（一个闭包值）作为参数调用 `f`。
- `throw(k, v)`：直接尾调用 `k(v)`。



### 5.3 CPS 解释器（eval_cps）

机器状态只有 `(code, env)`，配合**显式帧栈**或**直接续延值**，所有控制流都是循环：

```
eval(C, env):
  case C:
    halt V        → atom(V, env)
    let x = V in C' → C'[env ⊢ x = atom(V, env)]
    V V*          → tail jump: 求值头部与实参得值，替换 (code, env) 继续循环
    if V then C1 else C2 → if atom(V, env) then eval(C1) else eval(C2)
```

由于 CPS 中续延已经是显式参数，最简单且正确的实现是**纯蹦床（trampoline）**：

```
run(prog):
  code = prog; env = empty
  loop:
    switch code:
      LET:   env = bind(x, evalVal(V, env)); code = body;      continue
      IF:    code = evalVal(cond, env) ? then : else;          continue
      HALT:  return evalVal(V, env)
      CALL:  f = evalVal(head, env); args = evalVals(rest, env)
             if f 是原语 → 在循环内执行（见 §6.4 原语回调）
             if f 是闭包 → env = bind(闭包参数, args); code = 闭包体;  continue
```

`evalVal` 只求值原子（变量查表、字面量、闭包、原语名），是浅层操作，不会造成 C 栈递归。**所有调用都是循环迭代，C 栈永不增长。**

`callcc` 的实现：当 CALL 的头部是 `callcc` 原语时，把**当前机器续延**（即"算完这个 callcc 之后剩下的计算"）物化为一个闭包。由于 CPS 下续延就编码在调用点的参数里，这里只需要把调用点传入的续延参数重新打包为值即可——不需要额外的运行时栈。

（当我们在 §7.3 讨论"续延栈"表示时，callcc 则捕获帧栈的一个后缀。）

## 6. 求值的基础设施（C 实现）



### 6.1 值表示

```c
typedef enum { V_INT, V_FLOAT, V_BOOL, V_STR, V_FUN, V_PRIM } ValTag;

typedef struct Value {
    ValTag tag;
    union {
        int64_t  i;            /* V_INT */
        double   f;            /* V_FLOAT */
        bool     b;            /* V_BOOL */
        Str     *s;            /* V_STR  */
        Closure *clo;          /* V_FUN  */
        Prim     *prim;        /* V_PRIM */
    } u;
} Value;

typedef struct Closure {
    Value    fun;              /* 被捕获环境中的函数值（自引用，供递归） */
    CExp    *body;             /* CPS 下：λ(x*,k).C；ANF 下：λ(x*).E */
    int      nparams;
    char   **params;
    Env     *env;              /* 词法环境（被捕获的绑定） */
} Closure;
```

- 闭包在 ANF 与 CPS 之间复用同一结构；差别只在 `body` 指向的 IR 与参数约定（CPS 多一个续延参数）。
- `Env` 采用**链表作用域**或**扁平数组+快照**（见 6.3）。



### 6.2 IR 表示

CPS IR：

```c
typedef enum { C_LET, C_CALL, C_IF, C_HALT } CExpKind;

typedef struct CExp {
    CExpKind kind;
    union {
        struct { Value x, v;      CExp *body; } let;
        struct { Value head; int nargs; Value *args; } call;   /* args[nargs-1] 是续延 */
        struct { Value cond;      CExp *then, *els; } if_;     /* then/els 的续延相同 */
        struct { Value v; } halt;
    } u;
} CExp;
```

ANF IR 与之同构，差别是：

- `call`/`prim` 出现在 `let` 的右侧而不是整体节点；
- 尾调用 `call/prim` 是独立节点；
- 多出 `callcc`/`throw` 节点（ANF 机器拒绝执行，CPS 机器接受）。



### 6.3 环境（Env）

C 中的环境是一个从变量名到 `Value` 的映射。设计选择：

- **链式环境**：`typedef struct Env { char *name; Value val; struct Env *prev; } Env;` 简单，但线性查找慢。
- **扁平环境 + 闭包快照**：编译器先把自由变量编号，闭包携带一个 `Value[]` 快照，解释器用下标访问。更贴近真实编译器的做法。

设计建议：**先用链式环境（M1 快速跑通），M4 再换扁平快照**。两种方案在《目录与里程碑》中标注。

### 6.4 原语

原语签名按所在 IR 区分：

- **ANF**：`Value prim_ANF(Value *args, int nargs, PrimCtx *ctx)` —— 返回结果值。
- **CPS**：`void prim_CPS(Value *args, int nargs, Machine *m)` —— 不返回，直接改写 `m->code/m->env` 继续循环（回调式原语）。`callcc`、`throw`、`print` 都是这种签名。

```c
typedef struct Prim { const char *name; int arity; PrimFn fn; } Prim;
```

算术/比较原语（`+ - * / % == …`）两套签名共用同一组计算内核，只在外层适配返回/回调。

## 7. ANF ↔ CPS 转换



### 7.1 ANF → CPS（CPS 转换）

定义 `⟦·⟧` 把 §2.1 的 `body = [bind*, tail]` 映到 CPS 表达式，同时引入续延参数 `k`。

> 早期版本这里用的是经典 ANF 记法（`halt A` / `let x = call… in E` / `letrec`）。
> 下面已改成实际形状：**单**递归由 `letfun` + 按名自引用表示；**互递归**的
> `["letrec", [fnbind*], body]` 已在 §2.1 预留（尚未产出）。

尾：

```
⟦["atom", a]⟧      = call k ⟦a⟧                    -- 把结果投给续延
⟦["call", f, as]⟧  = call ⟦f⟧ ⟦as⟧ k              -- 尾调用：续延原样传递
```

绑定序列逐条右折，"余下的 binds + tail"整体作续延体（记 `B ; rest` 为"先求值 `B`，
再把结果继续投给 `rest` 的续延"）：

```
⟦["let", x, a] :: rest⟧              = call (λx. ⟦rest⟧) ⟦a⟧
⟦["letbin", x, op, a, b] :: rest⟧    = call op ⟦a⟧ ⟦b⟧ (λx. ⟦rest⟧)
⟦["letcall", x, f, as] :: rest⟧      = call ⟦f⟧ ⟦as⟧ (λx. ⟦rest⟧)
⟦["letfun", f, ps, bd] :: rest⟧      = call (λf. ⟦rest⟧) (λ(ps*, k). ⟦bd⟧)   ; 自引用即递归
⟦["letif", x, c, bt, be] :: rest⟧    = if ⟦c⟧ then ⟦bt ; rest⟧ else ⟦be ; rest⟧
⟦["letthrow", x, k', v] :: rest⟧     = call ⟦k'⟧ ⟦v⟧         ; 不返回，rest 丢弃
⟦["letcallcc", [k', r], f, bd] :: rest⟧ = （见 §5.2）
```

注意 `letif`：**CPS 侧两支都要把 `rest` 复制进去**。反过来这正是 §4.1 里说
`letif` 在 ANF 侧的价值——ANF 侧不需要复制。

要点：

- 尾调用位置的续延就是**外层传入的 k**，因此 CPS 天然保留 TCO。
- 函数体的转换以"函数的续延参数 k"为环境入口，被调用时收到真实续延。
- `callcc`/`throw` 在转换中保持不变（CPS 机器原生支持）。



### 7.2 CPS → ANF（un-CPS）

**前提**：CPS 程序中的续延从不逃逸、只在尾位置被调用（"续延封闭"）。在此条件下可反变换，把续延吸收回语法结构。算法思路：

```
unCPS(C):  // 假设续延都形如 λx. E 且只在尾位置被调用
  把 C 中每个调用点 f a₁…aₙ (λx.E) 还原为 let x = call(f,a₁…aₙ) in unCPS(E)
  续延参数 k 在函数内部被调用的位置 ⟦k v⟧ 还原为"返回 v"（对应 ANF 的 `tail`）
```

实现上用一个**续延逆环境** `k ↦ 期望的表达式模板` 做抽象求值。凡遇到 `callcc` 或续延被多次/以值方式保存的程序，un-CPS 直接失败并报"该程序不可去 CPS 化"。

### 7.3 续延的两种运行时形态（影响解释器设计）


| 形态       | 实现                          | callcc 支持         |
| -------- | --------------------------- | ----------------- |
| A. 续延即值  | 蹦床，续延是普通闭包                  | 免费（续延已经是值）        |
| B. 显式续延栈 | 机器维护 `Frame *cont` 帧栈，续延是帧链 | `callcc` = 捕获帧链后缀 |


设计上 **先实现 A**（简单、正确）；B 作为后续优化/教学展示（更接近"控制栈"的直觉，也便于接 `setjmp/longjmp` 风格的异常原语）。两者结果应一致，可交叉测试。

## 8. 内存管理（C 解释器路线）

> **范围**：本节是 **C 解释器**的 GC（mark-sweep + 显式值栈维护根集合）。
> **自举运行时**的 GC 与原生栈扫描见 `docs/SELFHOST.md` §8。两者的共同约束
> （尤其是"**不做保守扫描**、只用显式值栈"）两边都要遵守 —— 这也是
> `FLAT_ABI.md` §7.3 要求"新增的 cell 区必须走显式登记路径"的原因。



### 8.1 为何不用引用计数

CPS 中闭包可捕获续延，续延再捕获闭包——**环**不可避免，引用计数会泄漏。因此采用**追踪式 GC**。

### 8.2 标记-清除（mark-sweep）

- 堆对象：`Closure`、`CExp`（若动态构造）、`Env`、`Str`。
- **根集合**：机器状态——`m->code` 引用的闭包、`m->env`、正在求值的实参数组、以及原语回调里的临时值。
- 根集合用显式**值栈**（`Value *vstack`）维护：进入原语回调前 `gc_push` 保护临时值，返回后 `gc_pop`。C 局部变量不当作根（不做保守扫描），保证可移植与精确性。
- 回收：`mark(roots) → sweep(堆)`，空闲块用 free 链维护，`alloc` 优先复用。



### 8.3 简化方案（M1–M2 先用）

- **arena/无 GC 版**：解释器运行期只增不减地分配（跑完统一释放），配合 `--limit-nodes` 限制节点数以防失控。正确性测试用它跑小输入。
- M3 引入 mark-sweep 后，arena 版保留为 `--no-gc` 调试开关。



## 9. 类型系统（可选扩展，非核心）

核心设计不设类型系统（untyped）。扩展方向：

- **表层 + ANF**：Hindley-Milner 类型推断（`let` 泛化）。
- **CPS**：续延类型 `τ ⇒ ⊥`，CPS 版本的类型为 `(A→⊥)→⊥`，采用回答类型多态（answer type polymorphism）。`callcc : ((τ⇒α)→τ)→τ`。
- 该扩展不影响 §4–§8 的任何求值语义，独立成模块。



## 10. 目录结构与模块划分（C 解释器路线）

> ⚠️ **本节是该路线的历史布局**，描述 `src/*.c` 各模块的职责。**当前项目的目录
> 结构与 CLI 以 `docs/ARCHITECTURE.md` 为准**（`src-self/` 自举编译器 +
> `src/` C 解释器）。保留本节是为了理解 §4.3 / §5.3 / §6 / §8 各模块的分工。

```
yac/
  DESIGN.md
  README.md
  Makefile
  src/
    main.c            -- 驱动：CLI 选项、读取源文件、装配管线
    lexer.c/h         -- 词法
    parser.c/h        -- 语法 → AST
    ast.c/h           -- AST 节点与 dump
    anf.c/h           -- AST → ANF 归一化
    cps.c/h           -- ANF → CPS
    uncps.c/h         -- CPS → ANF（受限，可失败）
    value.c/h         -- Value/Closure/Prim、原语实现
    env.c/h           -- 环境
    eval_anf.c/h      -- ANF 解释器（含 callcc 拒绝逻辑）
    eval_cps.c/h      -- CPS 解释器（蹦床 + callcc/throw）
    gc.c/h            -- mark-sweep GC
    print.c/h         -- 值/IR dump 输出
  tests/
    run_tests.sh      -- 回归脚本
    *.yac             -- 测试用例
    props/            -- 属性测试（随机程序，ANF 与 CPS 结果比对）
```

CLI 完整参考见 `docs/ARCHITECTURE.md` §CLI。本节旧版列的
`yac --cps` / `--dump-anf` / `--dump-cps` / `--both` / `--no-gc` 仍有效，但新增选项只在那边。



## 11. 验证策略（C 解释器路线）

> **当前项目的验证策略见 `docs/ARCHITECTURE.md` §测试**（`make test` 回归、
> `make prop` 属性测试）与 **`docs/SELFHOST.md` §9**。本节是 **ANF / CPS 双机器
> 一致性测试**的历史设计（`--both` 的用途）。

1. **golden tests**：同一 `.yac` 程序分别跑 ANF 与 CPS，输出必须一致。
2. **属性测试**：随机生成 AST → 转 ANF → 转 CPS，比对 `evalANF` 与 `evalCPS` 结果；随机程序里混入 `callcc`/`throw` 时，只对 CPS 断言（ANF 应报"不支持"）。
3. **TCO 压测**：`let f(n) = if n==0 then 0 else f(n-1) in f(10000000)` 必须不爆栈（ANF 与 CPS 各一遍）。
4. **callcc 语义测试**：`callcc`+`throw` 的经典用例（提前退出、生成器式背靠背续延、K 组合子小剧场）。



## 12. 里程碑（C 解释器路线，M1–M4 已达成）


> **当前路线图见 `docs/SELFHOST.md` §10**（自举阶梯、M1–M3 分解、进度）。
> 下表是该路线（C 解释器 + 双机器）的历史里程碑。

| 里程碑 | 内容                                      | 验收                           |
| --- | --------------------------------------- | ---------------------------- |
| M1  | lexer、parser、AST→ANF、ANF 解释器（arena 分配）  | 普通程序可跑；golden 测试过            |
| M2  | ANF→CPS、CPS 解释器（蹦床）、`callcc`/`throw` 原语 | `--both` 对普通程序一致；callcc 用例跑通 |
| M3  | mark-sweep GC、un-CPS（受限）、`--dump-*`     | TCO 压测 10⁷ 级不爆栈；un-CPS 往返一致  |
| M4  | 扁平环境快照、CPS 化简（常量折叠/eta 归约）、属性测试、文档      | 性能可测量改进；随机程序比对稳定             |




## 13. 参考文献与灵感

- Andrew W. Appel, *Compiling with Continuations*（CPS 作为编译 IR 的经典）
- A. Sabry, M. Felleisen, *Reasoning about Programs in Continuation-Passing Style*（CPS 等价性、un-CPS 条件）
- C. Flanagan et al., *The Essence of Compiling with Continuations*（ANF 与 CPS 的关系）
- S. L. Peyton Jones, *Compiling Haskell by Program Transformation*（归约/化简示例）

