# EMIT_BACKEND.md — emit 骨架 + arch 后端（抽取与 arm32）

> 目的：把 LIR→机器码的**语义骨架**从各 arch 的 emit 里抽出来，让新增/维护一个
> arch 只需写"原语实现"（`op_*`）。本文先给 **LIR 指令分类**，再给**骨架切法**、
> **`op_*` 接口**、**抽取顺序**，最后是 **arm32 后端**的落点。
>
> 状态（2026-09）：设计。当前 `back/emit/` 是"每 arch 一份全量 emit"。

---

## 0. 现状与动机

| 文件 | 行数 | 角色 |
| --- | --- | --- |
| `back/emit/emit.yac` | 1276 | 已共享的 helpers（apply/reloc/ids/tcall_tab/prof/cimport/smap/strlit/gref/extsym/yjit/resolve_loop/glob_data…） |
| `back/emit/emit_x86_64.yac` | 2282 | x86_64：`emit_program_x86_64` + `emit_x86_i_*` + `emit_insn_go` |
| `back/emit/emit_arm64.yac` | 1987 | arm64：`emit_program_arm64` + `emit_a64_i_*` + `emit_insn_a64_go` |
| `back/emit/emit_riscv64.yac` | ~1900 | riscv64 |
| `back/encode/encode_*.yac` | 200–300 | 指令字编码 |

三个后端**结构高度同构**（约 60–70%）：prog 组装、`emit_insn_go` 的分类、各 `i_*`
的语义分解、调用约定形态、patch/label/resolve 流程。差异只在**指令编码、寄存器、
ABI、帧/立即数规则**。

新增一个 arch（如 arm32）现在要**再写 ~2000 行**。本文的目标是把它降到"只写
`op_*` 原语"（~500–800 行）。

---

## 1. 约束（实测，决定方案形态）

1. **循环 import 会崩**：`package a` ⇄ `package b` 互 import，编译出二进制但运行
   `SIGSEGV`（rc `-1073741819`）。⇒ `emit.yac` **不能** import 各 arch 做 `if`
   分派；"骨架调 arch"只能走**动态调用**。
2. **高阶调用可行**：顶层函数作参数传入并在另一个顶层函数（含嵌套递归）里调用，
   简单与嵌套用例都正常（测得 `3/20/7`、`5/42`）。⇒ 骨架可用**函数参数**接 arch
   入口。**但自举（yc 编译 yc）下的稳定性必须实测**——文档历史里动态调用/嵌套
   多次触发过段错误。
3. **各 arch 的 patch 表示不同同构**：`resolve_local_labels`(x86) 用 `[pos,name]` +
   `apply_patch`；`a64_resolve_fun_patches`(arm64) 用 `[tag,…]`（`is_int` /
   `"cclab"` / `"beq"` / `b26`）。抽公共框架要先统一 patch 表示（改两侧生成点）。
4. **x86 寄存器约束**：x86 数据操作多经 `rax`（`emit_load_rax_slot` → `mov rdi…`）。
   若语义统一成"逻辑寄存器 `op_*`"，x86 原语要来回 `mov`，**会改变 x86 的发射
   字节/性能**。⇒ 回归标准是"测试绿"，不是"字节一致"。
5. **yac 无对象/泛型/宏/动态绑定**。原语"接口"只能靠：函数参数（高阶）或
   全局 box 回调。**不推荐 box 回调**。
6. **差异不止指令**（读代码后确认）——这些是"历史演化出的 arch 差异"，不是
   同一骨架换指令：
   - `emit_insn` **签名不同**：x86 `emit_insn(st, insn, is_entry, is_raw)`（4 参），
     `emit_insn_a64` / `emit_insn_rv` `(st, insn, is_entry)`（3 参）。
   - x86 的 `emit_fun` 带 **JIT-only** 的 `skipat`（`emit_jsess_skip`）与保留 `cur`；
     arm64/riscv **没有**（它们不参与 JIT，`jit_sess_ok` 只 `x86_64`）。抽共同
     `emit_fun` 要先统一这套 JIT 逻辑。
   - `resolve_local_labels` 的 patch 表示不同（见 3）。
   ⇒ "抽骨架"**不是纯机械搬运**：要先统一签名 / JIT 逻辑 / patch 表示，这会
   **改动现有后端**，回归只能靠 `make test`。

---

## 2. LIR 指令分类（按 `emit_insn_go`，`emit_x86_64.yac:1834`）

| 类 | 指令 | 语义 | arch 关系 |
| --- | --- | --- | --- |
| **A 帧** | `local`, `$local` | 建帧/声明槽 | `op_local` |
| **B core** | `mov_imm`, `mov`；arith `add sub mul div rem land lor xor bnot shl shr`；`icmp cmp`；`label jmp cmpjmp` | 槽算术、分支 | 3-操作数语义 → `op_*`；**x86 的 2-操作数在此吸收** |
| **B' 调用** | `fcall ycall ccall iccall tcall` | 调用约定 | 形态共享；寄存器/ABI → `op_arg/op_call/op_ret` |
| **B'' 终止** | `ret exit` | 返回/退出 | `op_ret/op_exit` |
| **C raw** | `$and $add $addi $sub $or $bts $bt $clamp0 $st64 $st8 $ld64 $ld8 $sp $fp $smap $gbase $glob $icmp $jcc $memcpy $memset $shr $carg $icall $f64*` | IR 直译的机器级动作 | 多数可映射 `op_*`；`$bt/$bts/$smap/$fp/$sp` 是 x86/位图特有 |
| **D 对象/堆** | `kind mref mset tag is_int alloc alloc_s obj_sti obj_st_int mref8 mset8 ld64 st64 write1 clock glob gst time_ms time_str argc argv` | 按 kind 分派的对象访问 | **语义固定** → 抽；`op_load/op_store/op_alloc/op_tag` |
| **E 闭包/调用** | `closure`, `icall` | 闭包布局 + 对象 ABI 调用 | 语义固定 → 抽（复用 `op_alloc/op_store/op_call_reg`） |
| **F** | `memcpy` | 块拷贝 | 语义固定 → `op_memcpy` |
| **G 续延** | `mkcont throwk cc_recv` | CPS 续延 | 语义固定 → `op_mkcont/op_throwk/op_cc_recv` |
| **H 系统** | `untag`, `syscall` | 去 tag / 系统调用 | `op_untag/op_syscall` |
| **I prim 兜底** | 其它 | 运行时调用 | 多数走 `op_call`；少量静态内建 |

分类谓词在 `emit.yac`：`is_arith` / `is_raw_op` / `is_frame_op`（arch 无关，保留）。

**结论**：B/D/E/F/G/H/I 的**语义 arch 无关**（kind 分派 / 闭包布局 / 块拷贝 / 续延），
只有 **A** 与 **C** 天然 arch 相关；**B'** 是"形态共享 + 寄存器/ABI 各异"。

---

## 3. 分层与分派

```
back/emit/emit.yac                       公共骨架（新）
  ├─ emit_insn_go(t, st, insn, is_entry, is_raw)     分类 dispatch（表 §2）
  ├─ emit_i_core / emit_i_heap / emit_i_memcpy /
  │  emit_i_cc / emit_i_clos / emit_i_apply /
  │  emit_i_call / emit_i_sys / emit_i_raw / emit_i_prim
  └─ emit_program_common(t, prog, load_vaddr, text_off, fmt,
                         op_begin, op_insn, op_resolve)
                                      ↑ 函数参数（高阶）
back/emit/op_x86_64.yac                  原语实现
back/emit/op_arm64.yac
back/emit/op_arm32.yac
```

- **分派**：arch 自己的 `emit_program_xxx` 只做"设置 + 调 common"，把
  `op_begin`（`win_begin`/`linux_begin`）、`op_insn`（`emit_*_i_*` 的 arch 半边）、
  `op_resolve`（`resolve_local_labels`/`a64_resolve_fun_patches`）作为**参数**传给
  `emit_program_common`。**不用全局 box 回调**（历史不稳定）。
- `emit.yac` **不 import** arch，避免循环 import。

---

## 4. `op_*` 接口（约 40 个）

```
A 帧     : op_local(op, nslots, nparams, selfabi)
B 数据   : op_mov_imm(op, rd, v)      op_mov(op, rd, rs)
           op_arith(op, k, d, a, b)   # k = add/sub/mul/div/rem/land/lor/xor/shl/shr
           op_bnot(op, rd, rs)  op_cmp(op, ra, rb)  op_icmp(op, cond, d, a, b)
           op_label(op, id)  op_jmp(op, id)  op_jcc(op, cond, id)
           op_cmpjmp(op, cond, ra, rb, id)
B' 调用  : op_arg(op, i, opnd)  op_self_set(op, opnd)
           op_call_direct(op, name, patch)  op_call_reg(op, reg)
           op_ret(op)  op_exit(op, reg)
C raw    : op_st8 op_ld8 op_st64 op_ld64 op_shr op_or op_and op_addi op_clamp0
           op_glob op_gbase op_smap op_sp op_fp op_jcc_raw op_memset op_carg
D 对象   : op_kind(op, d, rs)  op_load_word(op, d, base, off)
           op_store_word(op, base, off, rs)  op_load_u8/op_store_u8
           op_tag(op, rd)  op_untag_ptr(op, rd)
           op_alloc(op, d, nbytes)  op_alloc_s(op, d, nbytes)
           op_obj_sti(op, base, off, slot)  op_glob(op, d, off)
           op_time/op_clock/op_argc/op_argv
E 闭包   : op_closure(op, d, code, caps…)     # 复用 op_alloc/op_store
F        : op_memcpy(op, dst, src, len)
G        : op_mkcont/op_throwk/op_cc_recv
H        : op_untag(op, rd)  op_syscall(op, nr, args)
I prim   : op_prim(op, name, args)            # 可选
```

寄存器抽象：逻辑名 `T0/T1/…`、`ARG0..`、`SELF`、`SP/FP/LR/PC`，arch 提供映射。
**3-操作数语义**（`op_add(d,a,b)`），x86 在 `op_*` 内部补 `mov`（必要时 spill 到槽）。

---

## 5. 抽取顺序（按语义固定度 / 风险）

1. ✅ **共享 `emit_funs_loop`（`emit_fun` 循环）已落地**（2026-09）：
   `emit.yac` 的 `emit_funs_loop(funs, entry, T, op_insn, op_resolve, op_skip)` 接
   函数参数——x86_64 传 `emit_insn/resolve_local_labels/x86_skipat`，arm64/riscv 传
   `emit_insn_a64|rv / a64|rv_resolve_fun_patches / no_skip`（T=0）。三后端的
   `emit_program_*` 已改调它。验收：`make yc` 两趟自举 + `test-compiler` 201/0 +
   `test-iso` 321/0。**这同时证明了高阶参数在自举下可用**（§8.1）。
   （`emit_program` 的其余段——begin / globals / host bake / resolve loop / cabi——
   仍在各 arch，是下一步。）
2. **D 对象/堆**（`kind/mref/mset/tag/is_int`）——语义最固定、指令最简单。
3. **F memcpy / G cc / E closure·apply**。
4. **A 帧** `op_local`。
5. **B 数据/控制**（x86 2-操作数吸收最麻烦，留后）。
6. **B' 调用**（寄存器/ABI 最复杂）。
7. **C raw**（逐 op；arch 特有可只在对应 arch 实现，其它 arch stub）。

每步验收：`make test`（compiler/interp/pkg/boot/iso）全绿 + 自举通过。

---

## 6. arm32 后端落点

目标：ARMv7-A（A32，小端）+ AAPCS32。

| 文件 | 改动 |
| --- | --- |
| `back/pack/target.yac` | ✅ 已加 `arm32`：`arch_ids = [40,452,12,9]`（ELF `EM_ARM` / PE `ARMNT` / Mach-O `CPU_TYPE_ARM`,`ARM_V7`）；`parse_arch_name` 接 `arm32/arm/armv7` |
| `back/encode/encode_arm32.yac` | ✅ 已加：A32 编码（整数/访存/分支/栈/条件 MOV/CSET）。**VFP f64 待补** |
| `back/emit/emit_arm32.yac` | 待做：`emit_program_arm32` + `op_arm32_*`（帧/寄存器/ABI/调用约定/f64） |
| `back/emit/emit.yac` | 32 位布局（host 槽 `G+136+8*id`、cell 字段、`apply_u64`、GC 位图）→ 按 arch 参数化 |
| `back/emit/emit_cabi.yac` | AAPCS32 wrapper（参数 r0-r3 + 栈） |
| `back/lower.yac` | `emit_by_target` / `emit_apply_unres` 加 `arm32` 分支 |
| `back/pack/{elf,pe,macho}.yac` | `EM_ARM` / PE ARMNT / Mach-O ARM 重定位 |
| `rt/runtime.yac` | 字长 4、GC 位图、`$ld64/$st64`、f64（int-only 可先跳过） |
| `back/jit.yac` / `pack/yjit.yac` | JIT session 仍只 x86_64；`yjit` arch num 加 arm32 |
| `src-self/yc.yac` | usage 文本已有 `--arch`；`parse_arch_name` 已接 |
| `tests/run.yac` | `qemu-arm` 组 + `iso` 对拍 |

**字长**：两选——
- **A1 保守**：`emit_arm32.yac` 自带 4B helper/布局，`emit.yac` 不动 → 零回归，但有重复。
- **A2 参数化**：`emit.yac` 按 `target_arch` 派生"字长/布局宽度"，64 位分支保持现状 → 共享。

推荐先 A1，arm32 全绿后再评估 A2。

---

## 7. 验收

- `make test`（compiler/interp/pkg/boot/iso）**全绿**，且 `make yc`（两趟自举）通过。
- arm32：`tests/compiler/cases/l4_42.yac` 等经 `yc --arch arm32 … -o x` 后由
  `qemu-arm` 运行；逐步扩到 compiler cases 全集与 `iso`（arm32 vs 手写期望）。
- `--dump-asm` 用于定位；抽取期若发射字节变化，以**测试**为准（见 §1.4）。

---

## 8. 风险与开放问题

1. **自举稳定性**：✅ 已实测（2026-09）：`emit_funs_loop` 把
   `op_insn/op_resolve/op_skip` 作为函数参数，`make yc` 两趟自举通过，
   `test-compiler` 201/0、`test-iso` 321/0 ⇒ **高阶参数在自举下可用**。后续更大范围
   抽取若出现不稳，再转生成器路线。
2. **patch 表示统一**：抽 resolve 框架要先统一 `[pos,name]` 与 `[tag,…]`（改两侧
   生成点）。
3. **x86 发射变化**：原语化会改 x86 字节/性能，回归只能靠测试。
4. **字长参数化**：动 `emit.yac` 的共享布局需三后端回归；A1 可规避。
5. **VFP / f64**：arm32 的 IEEE f64 编码（VMOV/VADD…/VCVT）待补；int-only 先跑通。
6. **生成器 vs 动态调用**：若抽样的自举不稳，生成器是 yac 无动态绑定环境下更稳的
   长期方案（等价 LLVM TableGen / GCC .md 的位置）。

---

## 附：与既有文档的关系

- `docs/ARCHITECTURE.md`：模块结构与管线。
- `docs/LIR.md`：LIR 指令集与不变量（本文 §2 是其 emit 侧分类）。
- `docs/FLAT_ABI.md`：cell/顶层值/gvar/gval 语义；`docs/BOOTSTRAP_LINK.md`：链接模式。
