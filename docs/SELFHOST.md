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


| 阶段  | 内容                                           | 用谁运行            | 产物              |
| --- | -------------------------------------------- | --------------- | --------------- |
| L0  | 现有 C 解释器 `yac`                               | —               | 引导器（bootstrap）  |
| L1  | 用 yac 写前端 `lexer.yac` `parser.yac` `anf.yac` | C 解释器           | yac 源码          |
| L2  | 用 yac 写后端 `backend.yac` `emit.yac`           | C 解释器           | `yc.yac`（完整编译器） |
| L3  | `yc.yac` 编译普通程序 → 机器码二进制                     | C 解释器跑 `yc.yac` | 原生二进制           |
| L4  | `yc.yac` **编译** `yc.yac` **自身**              | C 解释器跑 `yc.yac` | 第一个原生 `yc`      |
| L5  | 用原生 `yc` 再编译 `yc.yac`                        | 原生 `yc`         | 自举验证（两产物同构）     |
| L6  | 测试 harness 由原生 `yc` 编译执行；C 仅 `make yc` 的 L4 与 interp（cps/callcc/float/bignum） | 原生 `yc` | 测试不再经 C 解释 harness |


**验证关键点（L4/L5 差分）**：

- 用 C 解释器跑 `yc.yac` 编译 `yc.yac` → `yc_a`
- 再用 `yc_a` 编译 `yc.yac` → `yc_b`
- 要求 `yc_a` 与 `yc_b` 对同一输入产生逐字节相同输出 → 证明编译器"已稳定/自举正确"。



## 4. yac 语言能力现状与缺口

写编译器需要：字符串处理、任意结构（List/AST）、字节缓冲、文件读写、整数/大整数。


| 能力                        | 现状            | 缺口与对策                                                   |
| ------------------------- | ------------- | ------------------------------------------------------- |
| 整数/大整数                    | ✅ 已支持（bignum） | —                                                       |
| 布尔/比较                     | ✅             | —                                                       |
| 不可变 List                  | ✅             | 用 List 存 token/AST/LIR                                  |
| 闭包 / map / filter / foldl | ✅             | 编译器大量使用高阶函数                                             |
| TCO                       | ✅             | 只对 self；`parse_expr`/`parse_program` 不做 TCO            |
| 字符串                       | ✅             | `str_len` `str_ref` `str_cat` `str_slice` `==`          |
| 字节缓冲/可变数组                 | ✅             | `bytes_*`（append/ref/put/extend），用于拼机器码                 |
| 文件 I/O                    | ✅             | `read_file` `write_file`                                |
| 位运算                       | ✅             | `bshl` `band`/`bor`/`bxor`/`bnot`（机器码编码需要）             |
| 系统调用 / 进程退出               | ✅             | LIR `syscall`；`_start` 以 `untag` + `syscall 60` 退出；`argc`/`argv` |


> 这些原语在 C 解释器走 `value.c` 的 `PRIMS`；自举后的原生 `yc` 走 `runtime.yac` + emit kernel。两者签名一致，前端无需区分。



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

```
src-self/
  lib/           log / pass / map
  front/         lexer → parser → anf → lir
  rt/            runtime.yac（进 bundle）；num.yac（客 stdlib，不 cat）
  back/
    pack/        target + elf / pe / macho
    encode/      单条目标指令 → 字节
    emit/        LIR → .text（调用 encode）
    lower.yac    按 target 调 emit_*
    profile.yac
    backend.yac  管线 + compile_file
  lang/          scheme.yac
  yc.yac         CLI（--arch / --format）
  drivers/       各阶段测试驱动（不进 bundle）
```

没有单独的 `regalloc.yac` / `link.yac`：M3 值为栈槽（`[rbp+off]`），符号/入口在 emit+pack 里完成。


> **架构约束**：yac 无相互递归/前向引用，故每个解析器/转换器实现为一个
> 顶层**自递归函数** + 其内部的**嵌套闭包 helper**（闭包可引用外层函数）。
> 这是本项目反复使用、验证可行的模式。
> 原生 emit 对大帧 `let` 会丢绑定：热点函数拆小、少用 `let k = nth(...)`。



## 6. 编译器后端（纯 yac 实现）



### 6.1 LIR（低级中间表示，跨架构共享）

ISA 以 `docs/DESIGN.md` §2.1 为准。构造器与 emit 同一套标签（`local`/`add`/`cmpjmp`/`fcall`/`mref`/…）。手写 boot LIR 可带 `exit` 糖。

槽机：值为栈槽，临时值走返回寄存器。没有独立寄存器分配。`nth`/`cons`/`len`/`str_cat` 不是 LIR 指令，走 `fcall yac_*`。`ccall("name", …)` 走 C ABI。

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


| 测试       | 做法                                                             |
| -------- | -------------------------------------------------------------- |
| 前端单元     | 解释器跑 `lexer.yac/parser.yac/anf.yac`，对固定输入比对 token/AST/ANF dump |
| 后端单条指令   | `emit-<arch>` 对单指令产出，与汇编器 `as` 产出逐字节比对                         |
| 端到端差分    | `yc.yac` 编译 `fact.yac` → 二进制，运行输出 == 解释器输出                     |
| **自举同构** | `yc_a` 与 `yc_b` 编译同一输入，输出逐字节相同（§3 L4/L5）                       |
| 跨架构      | `--arch arm64/riscv64` 生成目标 ELF，用 `qemu-<arch>` 运行比对           |
| 全量回归     | 把 `tests/*.yac` 全部经 `yc` 编译运行，与解释器比对                           |




## 10. 里程碑



### 10.1 当前进度（已提交）


| 阶段   | 内容                                                                                | 状态     |
| ---- | --------------------------------------------------------------------------------- | ------ |
| M1   | yac 语言补缺：字符串/字节缓冲/位运算/文件 IO/argv/exit/int_to_str 原语（进 `PRIMS`）                    | ✅ 完成   |
| M2   | 前端：`lexer.yac` `parser.yac` `anf.yac`（用 List 表示 IR）                               | ✅ 完成   |
| M3.1 | ELF 打包器 `elf.yac`（最小 x86-64 ELF64，exit(42) 跑通）                                    | ✅ 完成   |
| M3.2 | x86-64 指令编码库 `encode_x64.yac` + LIR `lir.yac` + 代码发射 `emit.yac`（算术/比较/if/函数调用/递归） | ✅ 完成   |
| M3.3 | `lower.yac` ANF→LIR + **源码→ELF 端到端**（递归 fact 跑通）                                  | ✅ 完成   |
| M3.4 | 多参数调用、print（十进制输出）                                                                | ✅ 完成   |
| M3.5a | tagged value（int=`n<<1`，ptr=`p\|1`）                                                   | ✅ 完成   |
| M3.5b | 闭包 + 捕获环境 + 精确 free_vars                                                         | ✅ 完成   |
| M3.5c | 保守 GC（`yac_alloc` + 链表；暂不回收）                                                    | ✅ 完成   |
| M3.5d | 一等函数/高阶、列表/字符串/bytes/IO 原语、`yc.yac` 驱动                               | ✅ 完成*  |


**已落地文件**：`src-self/{lib,front,rt,back,lang}/`，CLI `yc.yac`，驱动 `drivers/`。
全量：`make test`（原生 `yc_a` 编译 `tests/run.yac` 再执行）。C 仅作为子进程跑 interp_suite（cps/callcc/float/bignum）。`yc_a` 复用 Makefile L4 产物（拷贝，不再在 harness 里用 C 编一遍）。compiler/qemu/boot/`yc_b`/iso 走原生。
**CLI（原生 yc）**：默认仍 `yc file.yac` 写出可执行文件（引导测试依赖）。`--cps`/`--both`/`--uncps`/`--scheme`（无 `-o`）以及 `--repl` 在同一进程里编译到 `0x200000000`、mmap RWX 后 `call` `_eval`（普通函数 ABI；`prog` 的进程入口名仍是 `_start`，镜像里没有该过程）。`--ast`/`--dump-anf` 打印列表。`--dump-cps` 暂打印 ANF。scheme 子集在 `scheme.yac`。

**LIR 运行时（M6 的 yac 化，已尽量推进）**

`lir_expr` 把一项 ANF 编成槽指令；整表 `["prog", [_start] · runtime · procs, "_start"]` 在 backend 里拼出。runtime 约 **41** 个过程。手写 `gen_*` helper 已清空。

`yac_gc` / `yac_gc_mark` 是 `$proc`：清 mark、可选 mmap 对象 bitmap（1 bit / 8 字节槽）。x86 用 `$smap`/`$fp` 按 rbp 栈图扫槽；arm64/riscv64 的 `$smap` 为 0，从 `$sp` 扫到 `stack_hi`。热路径用 `$jcc`（裸 cmp+jcc），不物化 tagged bool。无 bitmap 时回退链表查找。

**已知编译器限制**：小程序里 33 元列表 / 33 个 `let` 都正常；**编译器自己的 `_start`（bundle 几百条顶层 let 合成一帧）里**放 33 元 LIR cons 字面量会被错编（`bytes_to_str` → SIGSEGV 139）。对策：`runtime.yac` 的 insn 列表做成 `rt_*_ins(_)` 小函数。`emit_insn` 已拆成按 op 分组的小函数；不要再把大块逻辑塞回 dispatcher。

**riscv64 / arm64**：与 x86 同源 `COMPILER_CASES` 在 qemu 下全过。`rv_li32_at` 必须保留原 `lui` 的 rd（`yac_alloc` 的 bump 用 t0）；若写成固定 t1，堆指针读到垃圾，用户函数/闭包会挂死或 SIGSEGV。

**近期完成（perf）**：全量 bundle 自编译曾 **~182s → ~35s**（emit patches 隔离、`bytes_extend`、lower/resolve hash map）。C 解释器 GC：收集后阈值为 `max(8MiB, 2×live)`，本轮几乎不回收则 `4×live`（避免每 1MiB 重扫大堆）。L4 客机约 **34s**、~257 次收集。`emit_insn` 已按 core/heap/clos/prim 拆开。`str_cat` / `str_slice` / `bytes_extend` / `bytes_to_str` 走 kernel `memcpy`（x86 `rep movsb`）。

### 10.2 计划（按用户确定的路线图）

> 顺序：M3 → M4 → M5 → M6 → 之后再完善 callcc / scheme。


| 阶段  | 内容                                                                                                                 | 验收                                   |
| --- | ------------------------------------------------------------------------------------------------------------------ | ------------------------------------ |
| M3  | x86-64 后端：**整数子集端到端**（整数算术/比较/if/尾递归函数 → 可运行 ELF，输出与解释器一致）→ 随后**补齐闭包/GC/字符串/列表/全部原语**（见 §10.3）。**闭包与 GC 是必需项，不省略** | `yc` 编译 fact/fib/闭包/GC 程序 → 输出与解释器一致 |
| M4  | 自举：`yc.yac` 编译 `yc.yac` → 原生 `yc`；L4 烟测 42/letfun/fact；L5 `yc_a`/`yc_b` 对同一输入同构 | L4/L5 ✅ |
| M5  | arm64 / riscv64 后端 + `--arch` 交叉编译（qemu 验证）                                                                        | 三架构同源跑通 ✅ |
| M6  | rt yac 化 + GC 栈图；测试 harness 迁到原生 `yc`（L6）                                                                       | `make test` 由 `yc_a` 编跑 harness；C 只留 L0/interp |
| M7  | 完善 callcc / CPS（ANF→CPS 转换、续延原生实现）与 scheme 前端                                                                      | callcc 四例原生已通；JIT REPL/`--cps` 已接；`cps.yac` + `--dump-cps`；C 级 `--both`/un-CPS 仍走解释器 |




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
- **M3.4**：多参数调用（rdi..r9）、`print`（十进制输出）。
- **M3.5**（**必需，不省略**）：**闭包 + GC**。详细实现要点：

  **M3.5a 值表示（引入 tagged value）**
  当前后端用裸 int（64 位寄存器/栈槽）。引入闭包后需区分类型：
  ```
  int   : (n << 1) | 0    低 bit=0
  ptr   : (ptr | 1)       低 bit=1，指向 GC 堆对象
  ```
  所有槽/寄存器操作需解 tag（int 右移 1；指针清低 bit）。比较/算术先判 tag。

  **M3.5b 闭包对象（GC 堆）**
  ```
  [GObj hdr: next, mark, kind=CLO]
  [fnptr]             函数代码地址
  [nenv]              捕获变量个数
  [env0..envN-1]      捕获的值（tagged）
  ```
  LIR 新增：
  - `["closure", dst, fnName, [capSlots...]]`：分配闭包，填 fnptr+捕获值
  - `["apply", dst, closSlot, [argSlots...]]`：解包闭包调用
  函数调用约定统一：**rdi = 闭包指针（含环境），rsi,rdx,... = 用户参数**。函数 prologue 从闭包解包捕获环境到帧槽。

  **M3.5c 基本 GC（先无完整栈图）**
  - 堆分配经 rt 的 `yac_alloc`（malloc/brk）。
  - 标记-清除：根集 = 全局闭包表 + 当前活跃闭包。首版保守做法：所有创建后仍可达的闭包加入一个根表，不自动回收；后续加栈图精确扫描。
  - 栈图（§8）：每个调用点记录帧内哪些偏移是 tagged 指针，GC 时扫描原生栈。

  **M3.5d 字符串/列表/全部原语**
  - 堆对象（tagged ptr），GC 管理。
  - 全部 `PRIMS` 接入 rt。

  **验收**：`tests/gc.yac`、`tests/list.yac`、高阶函数（map/filter/foldl）、柯里化在原生 `yc` 上输出与 C 解释器一致。



## 11. 风险与对策


| 风险                | 对策                                |
| ----------------- | --------------------------------- |
| 自举验证不充分导致"编译器悄悄错" | L4/L5 逐字节同构是硬门槛；配合差分测试            |
| List 不可变导致编译器性能差  | 热点用 `bytearray`/`vector` 可变缓冲     |
| 字符串/IO 原语缺失阻塞前端   | M1 先补齐，且在 PRIMS 表统一，前端不感知         |
| 机器码有 bug 难排查      | `--emit-asm` 输出 + 与 `as` 逐字节比对    |
| 完全去 C 工程量大        | 分层：先"编译器 yac 化"，再"运行时 yac 化"，逐层推进 |




## 12. 一句话总结

用 yac 写一个能编译 yac 自身的编译器：**前端（lexer/parser/ANF）与后端（LIR/regalloc/emit/ELF）全部用 yac 实现**，运行时保留极小的机器码层，按 M1→M6 阶梯自举，用差分测试与 L4/L5 同构验证保证正确性，最终脱离 C 解释器。callcc/CPS 与 scheme 作为 M7 在完全自举后再完善。

## 13. 已解决问题记录


| 问题                       | 解法                                                    |
| ------------------------ | ----------------------------------------------------- |
| yac 无相互递归/前向引用           | 单自递归顶层函数 + 内部嵌套闭包 helper                              |
| yac `let` 是 letrec       | 避免 `let x = ...x...` 自遮蔽（换变量名），否则死循环                  |
| yac `and`/`or` 非短路       | 用 `if` 保护边界检查，避免 `str_ref` 越界                         |
| yac 无 `++`/`int_to_str`  | 用 `append`；M1 新增 `int_to_str` 原语                      |
| 顶层多项被当连续实参               | 顶层表达式间用 `;` 分隔（与 C 版 `parse_program` 一致）              |
| 零参调用 `f()` 解析为 `f(unit)` | `argc`/`bytes_new` 按 arity 1 处理，忽略 unit 参数            |
| 十进制无十六进制字面量              | 所有常量用十进制（如 `0x400000`→`4194304`）                      |
| ELF 段加载失败                | `p_offset=0`、`p_vaddr=0x400000`（对齐一致），映射整个文件          |
| GitHub 无法 push           | SSH 远程 `git@github.com` + 代理 `http://127.0.0.1:10809` |



### M3.4 已完成补充

**多参数调用**（已实现）：System V 约定 rdi,rsi,rdx,rcx,r8,r9。
- `emit.yac` call 加载参数到 6 个寄存器，prologue 存入槽 1..6
- **r8/r9 存栈需 REX.R（0x4C）而非 REX.B（0x49）**——REX.B 会把 rbp 变 r13（踩坑，已解决）
- 验证：`add3(1,2,3)=6`、6 参求和=21、混合运算=68

### M3.5 闭包实现要点（下次继续）

**值表示决策**：推荐全面引入 **tagged value**（非增量），因为一等函数需要统一表示：
```
int : (n << 1) | 0     低 bit=0
ptr : (ptr | 1)        低 bit=1，指向堆闭包对象
```
- 需改 emit 的 binop/cmp：算术前右移 1（解 tag），比较前比较 tagged 值
- `exit`/返回值解 tag

**闭包对象（堆）**：
```
[GObj: next, mark, kind=CLO][fnptr][nenv][env0..envN-1]
```
- LIR `["closure", dst, fnName, [capSlots]]`：分配闭包
- LIR `["apply", dst, closSlot, [argSlots]]`：解包（取 fnptr、恢复环境）
- 函数约定：**rdi=闭包（含环境），rsi..=用户参数**；prologue 解包环境到帧槽

**堆分配/GC**：
- `yac_alloc`：brk/mmap 分配
- 标记清除：根集=全局闭包表+活跃闭包（先保守，后栈图）
- 栈图：调用点记录帧内 tagged 指针偏移

**实现顺序建议**：①tagged value（改 emit 算术）→ ②闭包对象+create/apply → ③无捕获闭包测试 → ④捕获环境 → ⑤GC。每步用差分测试与 C 解释器比对。

### M3.5a 已完成补充

**tagged value**（已实现并回归）：
- int 存 `n<<1`（低 bit 0）；指针（闭包）用 `p|1`（低 bit 1，后续）
- emit 改造：`mov_imm` tag（shl）；`binop`/`cmp` 解 tag（sar）后运算再 tag（shl）；call 参数以 tagged 传递、返回值已 tagged；`exit`/`print` 解 tag；`br` 测 tagged 条件（非零即真，true=2）
- 新增 rbx 操作数编码（mov/sar/add/sub/imul/idiv/cmp）用于解 tag 运算
- 回归：算术/除/模/if/递归 `fact(10)=3628800`、print 全部正确
- **陷阱**：`in e8(...) in e8(...)` 非法（`in` 前必须是 `let _=` 绑定）——多语句函数必须 `let _=X in let _=Y in ... in C` 链

### M3.5b 闭包（下次继续）

实现顺序：
1. **堆分配器**：`yac_alloc(n in rdi)->rax`（brk syscall 12）：
   ```
   push rdi; mov rax,12; mov rdi,0; syscall   # rax=cur
   mov rbx,rax; pop rdi; lea rdi,[rbx+rdi]; mov rax,12; syscall  # brk(cur+n)
   mov rax,rbx; ret                            # 返回 cur 作分配起始
   ```
   追加到 ELF，注册 funOffsets。
2. **闭包对象**（堆）：`[fnptr][nenv][env0..envN-1]`；值=`ptr|1`。
3. **LIR**：
   - `["closure", dst, fnName, [capSlots]]`：alloc 对象，填 fnptr+捕获，存 `|1`
   - `["apply", dst, closSlot, [argSlots]]`：解包（`ptr&~1`），取 fnptr 间接 call
4. **函数约定**：rdi=闭包（含环境），rsi..=用户参数；prologue 解包环境到帧槽
5. **无捕获闭包**先验证（`let f=fun(n)->n+1 in f(5)`），再加捕获环境。
6. **GC**（M3.5c）：标记清除，根集=全局+活跃闭包；堆对象含 `next/mark` 头。

**关键**：apply 的间接调用需 `mov rax,[clos+0]; call rax`（间接 call）；tag 操作用 `test rax,1`/`and rax,~1`。

### M3.5b 已完成补充

**堆分配器 + 闭包编码**（已实现并提交）：
- `yac_alloc(n in rdi)->rax`（brk syscall 12）：返回分配起始（当前堆顶）。已追加到 ELF，注册 "yac_alloc" 到 funOffsets，可从 LIR `call`。
- 闭包相关编码：`test rax,1`/`and rax,~1`（tag 检测/清除）、`mov rbx,[rax+0]`（取 fnptr）、间接 `call rbx`、`lea rdi,[rbx+rdi]`、`mov rbx<->rax`。
- tagged value 就绪：int=`n<<1`，闭包指针=`p|1`。

### 闭包剩余（下次继续）

**关键决策待定：函数地址如何 resolve。**
- 方案 A：closure 存 fnptr=绝对地址 `LOAD_VADDR+fnOff`，用**绝对 imm32 patch**（新增 apply_abs32，非 rel32）在 resolve 阶段写函数偏移。
- 方案 B：closure 存 fnOff，apply 用 `lea rbx,[rip+off]`（rel32）——但 apply 生成时函数位置未知，仍需 patch。
- 推荐 **A**：closure 生成 `mov dword [rax+0], 0` 占位 + 记录 `["closurefn", immPos, fnName]`，resolve 时 `write32(immPos, LOAD_VADDR + fnOffset)`；apply 生成 `mov rbx,[rax+0]; call rbx`（间接）。

**实现顺序**：
1. emit 加 `closure`/`apply` 指令 + `apply_abs32` patch 类型。
2. `["closure", dst, fnName, [capSlots]]`：alloc 对象 `[fnptr][nenv][env...]`，填捕获，`or 1` tag，store。
3. `["apply", dst, closSlot, [argSlots]]`：untag（`and ~1`），取 fnptr，`call rbx`，参数按 System V。
4. lower 把 letfun 值 / 函数调用改为 closure/apply。
5. **无捕获闭包先验证**（`let f=fun(n)->n+1 in f(5)` → 6），再加捕获环境。
6. GC（M3.5c）：闭包对象加 `[next][mark]` 头，标记清除。

**调用约定**：函数参数从用户参数 rdi.. 开始（closure 的环境在对象里，函数内通过额外机制解包——建议先只支持无捕获，捕获环境后续加）。

### M3.5b 已完成补充

**无捕获闭包已实现并验证**：
- `closure` LIR：alloc 对象 `[fnptr][nenv][env...]`，fnptr 用绝对 imm32 patch（`LOAD_VADDR+TEXT_OFF+fnOff`），tag 指针。
- `apply` LIR：untag 闭包指针，取 fnptr，间接 `call rbx`。
- 验证：`closure inc; apply inc 5` → 6。
- **陷阱**：apply 不能对 fnptr 做 `and ~1`（fnptr 是绝对地址，奇偶任意）。

### 捕获环境（下次继续）

**核心难点：函数如何访问闭包捕获的环境。**
候选方案（需选一）：
1. **统一函数约定**：所有函数第一参数 `rdi=闭包`，用户参数 rsi..。函数 prologue 从 rdi 解包 env 到帧槽。但需改 `call`（普通调用也传 closure，可传 NULL/忽略）——统一但改动大。
2. **捕获作前缀参数**：闭包对象存 env 值；apply 把 env 作为函数前缀参数（在用户参数前）传入。函数签名 = [捕获..., 用户...]。需 lower 区分。
3. **间接环境指针**：函数额外收 rdi=env 指针（指向闭包对象 env 区），函数内经 rdi 索引访问捕获变量。

**推荐方案 2**（改动相对局部）：
- closure 对象 `[fnptr][nenv][env0..env_{k-1}]`。
- apply 生成：读对象 env 到 rdi,rsi,...（前缀），用户参数接在后面（rdx,rcx,...）。
- 函数 LIR 参数列表 = [捕获参数..., 用户参数...]（lower 生成时已知捕获集）。
- 捕获变量在函数体内引用其参数槽。

**实现顺序**：①lower 计算每个 lambda 的自由变量（捕获集）→ closure capSlots；②函数参数前插捕获参数；③apply 传 env 前缀。④GC（M3.5c）。

### M3.5b 自由变量分析（已完成 + 剩余）

**已完成**：`collect_anf_vars`（嵌套 `atom_vars` 避免相互递归）收集 body 引用；`collect_bound` 收集绑定名；`free_vars = refs - params - bound`。对非嵌套闭包正确：`fun(n)->n+x` → `[x]`。

**剩余（精确遮蔽）**：当前对**嵌套闭包过度捕获**（`fun(n)->fun(m)->n+m` 会误捕 `[m]`）。
修复：`collect_anf_vars(anf, accum, shadow)` 带**遮蔽集**：
- 收集 var 时，若 `name ∈ shadow` 则跳过（是绑定引用，非自由）。
- `letfun` 递归 body：`shadow ∪ params`。
- `let x = e in body`：e 用当前 shadow（x 是外层），body 用 `shadow ∪ {x}`。
- `letbin/letcall/letif`：子表达式用当前 shadow（dst 名不遮蔽子表达式）。
- 顶层调用 shadow 初始 = params。

### 捕获集成（下次）
1. 精确 free_vars（上述遮蔽）。
2. lower 编译 letfun 时：函数参数 = [捕获参数..., 用户参数...]（捕获参数是自由变量）。
3. closure：对象存捕获值；apply 把捕获值作为函数前缀参数传入（rdi.. 前缀，用户参数接后）。
4. 函数体内捕获变量引用其捕获参数槽。
5. GC（M3.5c）。

### 捕获集成（已完成）

**捕获环境已实现并验证**：
- 函数参数布局 = `[捕获参数..., 用户参数...]`：捕获（自由变量）在 slot 1..ncap，用户参数在 ncap+1..。
- `closure` LIR：分配对象 `[next][mark][fnptr][nenv][env...]`，写捕获值（先 load 到 rdi 再 store，修复了旧版直接 `mov_rax_disp8_rdi` 未先装 rdi 的 bug）。
- `apply` LIR：untag 闭包指针，读对象 env 作为**前缀参数**进 rdi..，用户参数接后，间接 call。
- lower 用 `free_vars` 计算捕获集；普通命名函数（ncap=0）走直接 `call`（递归按名解析），捕获闭包走 `apply`。
- **修复**：`collect_anf_vars` 的 `atom_vars` 原来闭包捕获的是**初始** `shadow` 而非线程化的 `shdw`，导致 letbin 链的临时变量被误判为自由变量——改为把 `shdw` 作参数传入。
- **验证**：`let x=10 in (fun(n)->n+x)(5)` → **15**。回归：fact/fib/多参/打印全部通过，新增 3 例闭包捕获测试。

**已知限制**：调用**存于变量中的闭包**（高阶，如 `let g = add(3) in g(4)`）暂不支持——call 点需要运行时按 tag 分发（动态 nenv），属 M3.5d 高阶函数范畴。当前仅支持：命名函数直接调用 + 立即应用/静态捕获闭包。

### GC（M3.5c，已完成保守版）

- 闭包对象加 `[next][mark]` 头：`[next][mark][fnptr][nenv][env...]`（offset 0/8/16/24/32）。
- `yac_alloc`：brk 分配 + 把新对象链入全局链表（`[next]=旧head`，head 指向新对象）。head 存在 ELF 数据区（段改 RWX）。
- `gc_collect`：x86 按调用点栈图沿 rbp 链标记 LIR slot，再清除未标记对象到 `gc_free`。arm64/riscv 仍扫 `[sp, stack_hi)`。
- 每次分配后调用 `gc_collect`（保守、无副作用，实际被反复执行验证）。
- 验证：闭包捕获 + 全部 e2e 在 GC 开启下通过。

### M3.5b 精确自由变量（已完成）

`collect_anf_vars(anf, accum, shadow)` 线程遮蔽集：let/letbin/letcall/letif 把绑定名加入 shadow，letfun 加 params；返回 `[accum, shadow]`。`free_vars = reverse(accum)`。嵌套闭包精确无过度捕获。

### M3.5d（已完成核心；M4 前剩余原语）

**已完成**：
1. **HO**：每个 `letfun`（含 ncap=0）分配 closure；捕获/参数走 `apply`（动态读 nenv）；命名函数直 `call`；嵌套函数合并进 funs。
2. **列表**：nil/cons/len/nth/tail/append/drop；字面量 desugar；`foldl`/`map`/`filter`（用户定义与原语）e2e。
3. **字符串**：STR 堆对象（kind=3，8 字节对齐）；`strlit` / `str_len` / `str_ref` / `str_cat` / `str_slice` / `int_to_str`；`==`/`!=` 内容比较。
4. **bytes**：BYTES 堆对象（kind=4；外层 48B，payload 在独立 STR blob，`dataptr@40`）。初容 16，按需倍增（与 C `realloc` 同语义，外层指针不变）。`bytes_new/len/ref/put/append` / `bytes_to_str`。
5. **IO**：`read_file` / `write_file`；位运算；`and`/`or`；bool 字面量。
6. **HO 原语**：`yac_apply1`/`yac_apply2` + 原生 `foldl`/`map`（含捕获）。
7. **M4 起步**：`make yc`（`yc_a`）；`make bootstrap`（`yc_b`）；L4 烟测 `l4_42` / letfun / fact。
8. **M5**：arm64/riscv64 与 x86 同源 `COMPILER_CASES`（qemu）。emit 跳过空 fun stub；`cmp ==` 走 `yac_str_eq`；`print` 区分 int/STR。`yac_alloc`：`brk`，失败则 `mmap` ANON。

**仍待**：
1. **M6 L6（已完成）**：`make test` 用原生 `yc_a` 编译 `tests/run.yac`。
2. **M7（进行中）**：原生 `callcc`/`throw` 已通；`cps.yac` 把 ANF 绑列表转成 CPS（`--dump-cps`）。ANF/CPS 双机 `--both` 与 un-CPS 仍走 C。
3. **GC**：`yac_gc` 已是 `$proc`；x86 读栈图，其它架构保守扫栈。`--emit-asm` / `regalloc.yac`：未做。

**L4 已通（原生 `yc_a`）**：`42` rc=42、`let f(n)=n+1 in f(41)` rc=42、`fact(5)` rc=120。CLI：`yc <file.yac> [-o output]`，默认输出为去掉 `.yac` 的路径（`fact.yac` → `fact`，不要 `.bin`）。引导产物命名 `yc` / `yc_a` / `yc_b`。

**L5 已通**：`make yc` 为 L4（C `yac` 编 bundle → `build/yc_tmp/yc_a`，并复制为 `yc`）。`make bootstrap` 再让 `yc_a` 编 bundle → `yc_b`（数秒）。`make yc-iso` 检查二者对同一输入 ELF 逐字节相同。`yc_a` 与 `yc_b` 自身不必相同。
**L6 已通**：`make test` 依赖 `yc_a`，原生编译并执行 `tests/run.yac`。

**关键修复**：
- **letif 丢函数**：then 臂 `letfun` 未并入 funs，闭包 fnptr 变成 `_start`。
- **`read_file` 64KiB 上限**：改为 `lseek` 按文件大小分配。
- **TCO**：只改**同名**后缀 `call` 为拆栈 `jmp`（不要 `apply`、不要把 `parse_expr` 尾调用成 `parse_binloop`）。`tco_prog` 对全部 fun（含 compiler bundle 里的 `lex`）。C lower `maybe_tailcall` 同样只认当前 gname。`loop(100000)` 用例。
- **编译速度**：`tokenize` 用 `list_push`。driver 每 pass 打 `[prof] <name>=<ms>`。**函数级 profile**：`./yac --prof-out FILE.prof …`（C 解释器）或 `yc_a --prof-out FILE.prof …`（原生 enter/leave），TSV（name/calls/incl_ns/excl_ns），按 exclusive 排序。原生 dump 的 pct 是整数百分比。
- **bytes 按需扩容**：外层 kind=4 固定小对象（len/cap/dataptr），数据在独立 STR blob。`yac_bytes_grow` 倍增 cap 并 `memcpy`，外层指针不变（C 是 `realloc` 同一 `Bytes*`）。GC 标记 kind=4 的 `+40`。`write_file` 从 blob+32 写出。
- **`and` 非短路**：`skip_block_cm` 里 `* /` 判断在含 ` * ` 的块注释上误匹配；改为嵌套 `if`。
- **空参 `let f() =`**：消耗 `)`；`has_params`（不要用 `len(params)>0`）才包装成 `fun`，否则 `f()` 会去调用整数。
