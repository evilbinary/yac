# Yac 自举编译器设计（Self-Hosted Compiler）

## 1. 目标与核心思想

用 **yac 语言本身**写一个编译器 `yc`（yac-compiler）：

- `yc` 读入 yac 源码，经词法/语法/ANF 分析，**直接生成目标机器码**，打包成可执行二进制（ELF）。
- `yc` 最终能**编译 yac 自身**（self-hosting）：`yc` 编译 `yc.yac` 得到原生 `yc` 二进制。
- 目标是**逐步脱离 C 实现**：当前 C 写的解释器只作为引导器（bootstrap），最终不再需要它。

一句话：**用 yac 写 yac 的编译器，让编译器能编译自己，从而摆脱 C。**

### 设计原则

1. **自举优先**：编译器每一层都用 yac 写，从第一天就面向"能编译自身"设计。
2. **IR 复用思想，重写实现**：复用现有 ANF/CPS 的*概念*（扁平帧、`(depth,slot)` 寻址、TCO），但用 yac 数据结构（List）重建，不调用任何 C。
3. **可验证**：每个阶段用"编译器 A 编译源码 P 的输出"与"解释器跑 P 的输出"逐字节对比（差分测试）。
4. **运行时最小化**：机器码只需一个极小的运行时（分配器 + GC + 原语 + 系统调用），其余全部在 yac 层。
5. **分架构后端**：LIR 之上共享，LIR 之下每架构一个 Emit（x86-64 / arm64 / riscv64）。

## 2. 总体架构

```
                    ┌───────────┐   ┌──────────────┐   ┌────────────┐   ┌────────────┐
  yac 源码 ────────▶│ lexer.yac │──▶│ parser.yac   │──▶│ anf.yac    │──▶│ backend.yac│──┐
                    └───────────┘   └──────────────┘   └────────────┘   └────────────┘  │
                    ┌───────────┐                       （纯 yac 实现，List 数据结构）    │
                    │ 运行时 rt │◀───────────────────────────────────────────────────────┤
                    │ (机器码)  │              ┌────────────┐  ┌────────────┐            │
                    └───────────┘◀───────────▶│ 寄存器分配  │  │ 机器码发射  │            │
                                             └────────────┘  └────────────┘              │
                                                                                         ▼
                                                                             ┌──────────────┐
                                                                             │ ELF 打包     │──▶ 可执行二进制
                                                                             └──────────────┘
```

- **前端**（lexer/parser/ANF）：纯 yac，操作 `List`/字符串，不依赖任何 C。
- **后端**（LIR → 寄存器分配 → 机器码 → ELF）：纯 yac。
- **运行时 rt**：极小的机器码/A 汇编层（分配器、GC、原语、syscall），是唯一"非 yac"部分，且被刻意限制到最小（见 §7）。

## 3. 引导阶梯（Bootstrapping Ladder）

| 阶段 | 内容 | 用谁运行 | 产物 |
|---|---|---|---|
| L0 | 现有 C 解释器 `yac` | — | 引导器（bootstrap） |
| L1 | 用 yac 写前端 `lexer.yac` `parser.yac` `anf.yac` | C 解释器 | yac 源码 |
| L2 | 用 yac 写后端 `backend.yac` `emit.yac` | C 解释器 | `yc.yac`（完整编译器） |
| L3 | `yc.yac` 编译普通程序 → 机器码二进制 | C 解释器跑 `yc.yac` | 原生二进制 |
| L4 | **`yc.yac` 编译 `yc.yac` 自身** | C 解释器跑 `yc.yac` | 第一个原生 `yc` |
| L5 | 用原生 `yc` 再编译 `yc.yac` | 原生 `yc` | 自举验证（两产物同构） |
| L6 | 全测试迁移到原生 `yc`，弃用 C 解释器 | 原生 `yc` | 完全自举 |

**验证关键点（L4/L5 差分）**：
- 用 C 解释器跑 `yc.yac` 编译 `yc.yac` → `yc_A`
- 再用 `yc_A` 编译 `yc.yac` → `yc_B`
- 要求 `yc_A` 与 `yc_B` 对同一输入产生逐字节相同输出 → 证明编译器"已稳定/自举正确"。

## 4. yac 语言能力现状与缺口

写编译器需要：字符串处理、任意结构（List/AST）、字节缓冲、文件读写、整数/大整数。

| 能力 | 现状 | 缺口与对策 |
|---|---|---|
| 整数/大整数 | ✅ 已支持（bignum） | — |
| 布尔/比较 | ✅ | — |
| 不可变 List | ✅ | 用 List 存 token/AST/LIR |
| 闭包 / map / filter / foldl | ✅ | 编译器大量使用高阶函数 |
| TCO | ✅ | 编译器和它生成的代码都不爆栈 |
| 字符串 | ⚠️ 只有字面量 | **需加**：`len` `substr` `concat` `==` 字符遍历 |
| 字节缓冲/可变数组 | ❌ | **需加**：`bytearray`（append/at/set），用于拼机器码 |
| 文件 I/O | ❌ | **需加**：`read-file` `write-file` `open/read/write/close` |
| 位运算 | ❌ | **需加**：`shift` `land` `lor` `lxor`（机器码编码需要） |
| 系统调用 / 进程退出 | ❌ | **需加**：`syscall` `exit` `argv/argc` |

> 这些原语加在现有 `value.c` 的 `PRIMS` 表（`value.c:557`），对 L0 的 C 解释器是新增内建；对自举后的 yac 则是运行时 `rt` 提供的机器码函数。两者签名一致，前端无需区分。

## 5. 编译器前端（纯 yac 实现）

### 5.1 数据结构（用 List 表示）

```yac
/* token: [kind, text, line] */
let tk = ["ident", "fact", 1]

/* AST 节点: [node-type, ...fields] */
let ast = ["call", ["var","fact"], [["int",5]]]

/* ANF: [let/call/if/tailcall..., fields...] */
let anf = ["tailcall", ["var","fact"], [["int",5]]]
```

`List` 不可变，故编译过程用"构建+组合"，与现有 C 版 ANF 构造逻辑对应。flat-frame 变量寻址 `(depth,slot)` 用 `[depth, slot]` 二元 List 表示。

### 5.2 模块划分（全部 .yac）

**已实现**：

| 文件 | 职责 | 状态 |
|---|---|---|
| `src-self/lexer.yac` | 字符流 → token 流（空白/注释/标识符/数字含科学计数法/字符串/操作符） | ✅ |
| `src-self/parser.yac` | token 流 → AST（递归下降 + 优先级爬升） | ✅ |
| `src-self/anf.yac` | AST → ANF（`[bindings, tailExpr]`，求值顺序显式） | ✅ |
| `src-self/elf.yac` | 机器码 → 最小 x86-64 ELF64 可执行文件 | ✅ |
| `src-self/driver_{lex,parse,anf,elf}.yac` | 各阶段测试驱动 | ✅ |

**规划中**：

| 文件 | 职责 |
|---|---|
| `src-self/encode_x64.yac` | x86-64 指令 → 字节流（mov/add/sub/imul/cmp/jcc/syscall 等） |
| `src-self/lower.yac` | ANF → LIR（指令选择） |
| `src-self/regalloc.yac` | LIR → 寄存器/栈槽分配 |
| `src-self/emit-<arch>.yac` | LIR → 目标机器码字节流（每架构一个） |
| `src-self/link.yac` | 符号、重定位、入口 `_start` → ELF |
| `src-self/driver.yac` | `main`：`--emit` 选项、调用管线、错误报告 |
| `src-self/rt.asm`（非 yac） | 运行时最小层（见 §7） |

> **架构约束**：yac 无相互递归/前向引用，故每个解析器/转换器实现为一个
> 顶层**自递归函数** + 其内部的**嵌套闭包 helper**（闭包可引用外层函数）。
> 这是本项目反复使用、验证可行的模式。

## 6. 编译器后端（纯 yac 实现）

### 6.1 LIR（低级中间表示，跨架构共享）

与 C 版设计同构，但用 yac 数据表示：

```yac
/* 指令: [op, dst, a, b, imm, off, sym] */
let ins = ["mov", "r0", "r1", [], 0, 0, []]
```

op 集合：`mov/add/sub/mul/div/and/or/xor/cmp/ldr/str/call/jmp/bcc/faddr/enter/leave`。

### 6.2 后端流程

```
ANF（[bindings, tailExpr]）→ lower（指令选择，绑定展开成栈槽 load/store）
    → regalloc（M3 用栈槽 [rbp+off]，临时值走 rax；后续再引物理寄存器）
    → emit-<arch>（每指令一个字节编码函数，编码库 encode_x64.yac）
    → link（入口 _start → elf.yac 打包 ELF）
```

- **M3 简化**：值为栈槽（`[rbp+off]`），临时值走 `rax`，函数用 rbp 栈帧，
  返回 rax。无需寄存器分配（正确性优先，后续再优化）。
- **尾调用**：统一生成 trampoline 循环，保证 O(1) 栈，`tco` 语义与 C 版一致。
- **print/exit**：M3 先用 syscall `write` / `exit`；后续接入 rt。

### 6.3 分架构支持

每个架构只实现 `emit-<arch>.yac`（LIR 指令 → 字节序列）与一个 `Target` 描述（寄存器集/参数寄存器序/帧对齐/链接寄存器）。Lower 与 RegAlloc 完全共享。

## 7. 运行时 rt 与"不依赖 C"的边界

**必须澄清**：生成的二进制仍需要底层服务（堆分配、GC 标记、`malloc`、`write` 系统调用）。这部分不可能用纯 yac 表达到底层寄存器——它是自举的"硬核"。

因此设计分两层：

- **rt（机器码/A 汇编，唯一非 yac 部分）**：提供
  - `yac_alloc`（GC 对象分配）、`gc_collect`（标记-清除）
  - `prim_*`（算术/比较/列表/字符串/字节/IO，即现有 `value.c` 原语表的机器码版）
  - `yac_main(argc, argv)`、`exit`、`write` syscall
- **编译逻辑（全部 yac）**：lexer/parser/ANF/后端/ELF 打包，完全在 rt 之上用 yac 表达。

**逐步解耦 C 的路线**：
1. **rt 先用 C/汇编写**（最小、稳定，作为可信基）。此时"不依赖 C 的编译器"指：编译前端/后端全部 yac，只有运行时是 C。
2. **rt 逐步 yac 化**：分配器/原语逻辑用 yac 重写，仅保留一层薄 A 汇编做"分配/系统调用"（malloc/brk/write）。
3. 最终：C 只存在于"从源码生成二进制"这最后一环的引导器里；运行时主体、编译器主体都是 yac。彻底去 C 需要手写 A 汇编实现整个运行时（M6，见 §10）。

## 8. GC 与原生栈扫描

自举编译生成的机器码仍要有 GC。设计：

- 复用 C 版的标记-清除思想（`gc.c`）。
- **根集扫描**：每个调用点生成**栈图（stack map）**——记录当前帧内哪些偏移是 Value 指针。GC 标记时按栈图扫描原生栈（同 JVM 栈图思路）。
- 栈图由 `emit` 阶段在已知 `(depth,slot)` 布局时生成，随 LIR 一起交给 `link` 放进只读数据段。

## 9. 验证策略

| 测试 | 做法 |
|---|---|
| 前端单元 | 解释器跑 `lexer.yac/parser.yac/anf.yac`，对固定输入比对 token/AST/ANF dump |
| 后端单条指令 | `emit-<arch>` 对单指令产出，与汇编器 `as` 产出逐字节比对 |
| 端到端差分 | `yc.yac` 编译 `fact.yac` → 二进制，运行输出 == 解释器输出 |
| **自举同构** | `yc_A` 与 `yc_B` 编译同一输入，输出逐字节相同（§3 L4/L5） |
| 跨架构 | `--arch arm64/riscv64` 生成目标 ELF，用 `qemu-<arch>` 运行比对 |
| 全量回归 | 把 `tests/*.yac` 全部经 `yc` 编译运行，与解释器比对 |

## 10. 里程碑

### 10.1 当前进度（已提交）

| 阶段 | 内容 | 状态 |
|---|---|---|
| M1 | yac 语言补缺：字符串/字节缓冲/位运算/文件 IO/argv/exit/int_to_str 原语（进 `PRIMS`） | ✅ 完成 |
| M2 | 前端：`lexer.yac` `parser.yac` `anf.yac`（用 List 表示 IR） | ✅ 完成 |
| M3.1 | ELF 打包器 `elf.yac`（最小 x86-64 ELF64，exit(42) 跑通） | ✅ 完成 |
| M3.2 | x86-64 指令编码库 `encode_x64.yac` + LIR `lir.yac` + 代码发射 `emit.yac`（算术/比较/if/函数调用/递归） | ✅ 完成 |
| M3.3 | `lower.yac` ANF→LIR + **源码→ELF 端到端**（递归 fact 跑通） | ✅ 完成 |
| M3.4 | 多参数调用、print（十进制输出） | 🔄 进行中 |
| M3.5 | **闭包 + GC**（必需项，见 §10.3） | ⏳ 待开始 |

**已落地文件**：`src-self/{lexer,parser,anf,lower,lir,emit,elf,backend,encode_x64,driver_*}.yac`；
测试套件 `tests/selfhost_{lexer,parser,anf,elf,emit,e2e}.sh`（49 项全绿，`make test`）。
**M3.3 完成**：源码→ELF 端到端（递归 fact(5)=120 等 9 例）。
GitHub 推送已通过 SSH 远程 + 代理 `http://127.0.0.1:10809` 解决。

### 10.2 计划（按用户确定的路线图）

> 顺序：M3 → M4 → M5 → M6 → 之后再完善 callcc / scheme。

| 阶段 | 内容 | 验收 |
|---|---|---|
| M3 | x86-64 后端：**整数子集端到端**（整数算术/比较/if/尾递归函数 → 可运行 ELF，输出与解释器一致）→ 随后**补齐闭包/GC/字符串/列表/全部原语**（见 §10.3）。**闭包与 GC 是必需项，不省略** | `yc` 编译 fact/fib/闭包/GC 程序 → 输出与解释器一致 |
| M4 | 自举：`yc.yac` 编译 `yc.yac` → 原生 `yc`，L4/L5 同构验证 | 自举成功 |
| M5 | arm64 / riscv64 后端 + `--arch` 交叉编译（qemu 验证） | 三架构同源跑通 |
| M6 | rt yac 化 + GC 栈图；迁移全测试到原生 `yc` | 弃用 C 解释器（完全自举） |
| M7 | 完善 callcc / CPS（ANF→CPS 转换、续延原生实现）与 scheme 前端 | callcc / scheme 测试通过 |

### 10.3 M3 分解（整数子集优先）

完整 x86-64 后端（闭包/GC/字符串/列表/全部原语）工程量大，故 M3 拆为子步骤：

- **M3.1**（已完成）：ELF 打包器 `elf.yac`。
- **M3.2**（进行中）：x86-64 指令编码库 `encode_x64.yac` + LIR + 代码发射。
  - 值表示：栈槽（`[rbp+off]`），临时值走 rax，无需寄存器分配（正确性优先）。
  - 函数：rbp 栈帧，prologue/epilogue，返回 rax。
  - 调用约定：简单（参数压栈、返回 rax）；尾调用用循环避免爆栈。
  - 指令集：`mov/add/sub/imul/idiv/cmp/setcc/jcc/call/ret/syscall`。
  - `print` 用 syscall `write`；`exit` 用 syscall `exit`。
- **M3.3**：`lower.yac`（ANF → LIR）+ `link.yac`（符号/重定位/入口 `_start`）。
- **M3.4**：端到端——`yc` 编译整数程序 → ELF → 运行，输出与 C 解释器逐字节一致（fact/fib）。
- **M3.5**（**必需，不省略**）：**闭包 + GC + 字符串/列表/全部原语**补齐：
  - 闭包：`letfun` 生成闭包对象（函数指针 + 捕获环境），捕获的变量随环境存储，调用时解包。
  - GC：原生标记-清除；根集扫描用**栈图**（§8）；堆分配经 rt 的 `yac_alloc`。
  - 字符串/列表：堆对象，GC 管理。
  - 原语：全部 `PRIMS`（算术/比较/列表/字符串/IO）接入 rt。
  - 验收：`tests/gc.yac`、`tests/list.yac`、闭包测试在原生 `yc` 上输出与 C 解释器一致。

## 11. 风险与对策

| 风险 | 对策 |
|---|---|
| 自举验证不充分导致"编译器悄悄错" | L4/L5 逐字节同构是硬门槛；配合差分测试 |
| List 不可变导致编译器性能差 | 热点用 `bytearray`/`vector` 可变缓冲 |
| 字符串/IO 原语缺失阻塞前端 | M1 先补齐，且在 PRIMS 表统一，前端不感知 |
| 机器码有 bug 难排查 | `--emit-asm` 输出 + 与 `as` 逐字节比对 |
| 完全去 C 工程量大 | 分层：先"编译器 yac 化"，再"运行时 yac 化"，逐层推进 |

## 12. 一句话总结

用 yac 写一个能编译 yac 自身的编译器：**前端（lexer/parser/ANF）与后端（LIR/regalloc/emit/ELF）全部用 yac 实现**，运行时保留极小的机器码层，按 M1→M6 阶梯自举，用差分测试与 L4/L5 同构验证保证正确性，最终脱离 C 解释器。callcc/CPS 与 scheme 作为 M7 在完全自举后再完善。

## 13. 已解决问题记录

| 问题 | 解法 |
|---|---|
| yac 无相互递归/前向引用 | 单自递归顶层函数 + 内部嵌套闭包 helper |
| yac `let` 是 letrec | 避免 `let x = ...x...` 自遮蔽（换变量名），否则死循环 |
| yac `and`/`or` 非短路 | 用 `if` 保护边界检查，避免 `str_ref` 越界 |
| yac 无 `++`/`int_to_str` | 用 `append`；M1 新增 `int_to_str` 原语 |
| 顶层多项被当连续实参 | 顶层表达式间用 `;` 分隔（与 C 版 `parse_program` 一致） |
| 零参调用 `f()` 解析为 `f(unit)` | `argc`/`bytes_new` 按 arity 1 处理，忽略 unit 参数 |
| 十进制无十六进制字面量 | 所有常量用十进制（如 `0x400000`→`4194304`） |
| ELF 段加载失败 | `p_offset=0`、`p_vaddr=0x400000`（对齐一致），映射整个文件 |
| GitHub 无法 push | SSH 远程 `git@github.com` + 代理 `http://127.0.0.1:10809` |
