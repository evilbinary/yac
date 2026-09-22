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

## 3. 框架：emit.yac 持有 op 注册表并负责运转（已落地模式）

实测（2026-09）确定：**由 `emit.yac` 持有注册表、arch 反过来调 `emit_reg` 注册**，
比"arch 造表传参"更好：`emit.yac` 是框架的**运转者**，arch 只提供 op 实现。且
**不需要统一各 arch 的 patch 表示**（patch 格式封在 op 内部，骨架不碰）。

```
back/emit/emit.yac（框架）
  ├─ emit_opmaps_box / emit_opmap_of(arch)        # arch -> (name -> fn)
  ├─ emit_reg(arch, name, fn)                     # arch 调用它注册
  ├─ emit_op_get(arch, name)
  ├─ emit_insn_disp(st, insn)                     # 框架分派：查 emit_arch 的槽
  ├─ emit_i_core_base(st, insn, is_entry, ops)    # core 子集（三后端共用）
  ├─ emit_funs_loop(funs, entry, T, op_insn, op_resolve, op_skip)
  └─ 其余 helpers（apply/reloc/ids/patch/gref/strlit/…）

back/emit/emit_{x86_64,arm64,riscv64,arm32}.yac（arch）
  ├─ <arch>_op_<name>(st, insn) -> st            # 每条 LIR op 一个函数
  ├─ <arch>_reg_ops = emit_reg("<arch>", …) …    # 包加载时注册（顶层副作用）
  └─ emit_program_<arch> / emit_<arch>_i_*       # 逐步瘦身：先 emit_insn_disp，未命中落 rest
```

- **注册式**：arch 顶层写 `let <arch>_reg_ops = emit_reg("<arch>", "<name>", fn) …`；
  `emit.yac` **不 import** arch（循环 import 会崩，§1.1），只被 arch import。
- **按 arch 分槽**：表键 = arch 字符串（`emit_arch`），一个进程里多个 arch 包共存不
  互相覆盖。`emit_arch` 由 `lower.yac` 在选目标时 `emit_arch_set(a)`。
- **分派**：`emit_insn_disp(st, insn)` 用当前 arch 查表，命中即 `(f)(st, insn)`；
  未命中返回 0，arch 的 `*_rest` 兜底；逐条搬走后 rest 清空，最终
  `emit_insn = emit_insn_disp`。
- **op 粒度 = 每条 LIR op**，签名 `(st, insn)`；`st = [b, cur, labels, patches, ids, ctx]`，
  需要 `is_entry/is_raw` 的 op（`local/ret/tcall/exit`）从 `ctx = [is_entry, is_raw]` 取。
- arch 特有 op（x86 `$bt/$smap/$sp…`）同样**注册**（实现放 arch）——框架只是没有它们
  的共享默认实现。

---

## 4. op 表（`name → 实现`）

每条 LIR op 一个 arch 函数 `(st, insn) -> st`；`st = [b, cur, labels, patches, ids, ctx]`。
op 函数体 = 原 arch 的该 `case` 体：`let b = nth(st, 0) in` … 末尾
`emit_st4(st, b, nth(st, 1), nth(st, 2), nth(st, 3))`。arch 顶层把 `name → fn` 注册进
`<arch>_opmap`（`fmap`，构建一次）。

**已迁移**（注册进 opmap）：

| 组 | op | 后端 |
| --- | --- | --- |
| core 数据/控制 | `mov_imm mov` + arith + `icmp jmp cmpjmp label local $local cmp` | ✅ x86/arm64/riscv（注册式） |
| core 调用/终止 | `fcall ycall ccall iccall tcall ret exit` | ✅ x86/arm64/riscv（注册式） |
| heap 访存 | `mref mset tag is_int obj_sti obj_st_int mref8 mset8` | ✅ x86/arm64/riscv（注册式） |

> **ctx 不放 `st`**：`st` 的第 6 项是 `goff`（`apply_u64_rel` 的 tag 14 用），第 7 项是
> `gsess`。`is_entry/is_raw` 因此走**全局 box**（`emit_ctx_set/get`，由 `emit_funs_loop`
> 每函数设置一次）。`emit_*_i_core` 现在就是 `emit_insn_disp`（`*_core_rest` 已删）。

**待迁移**（仍在各 arch 的 `*_rest`）：

| 类 | op | 说明 |
| --- | --- | --- |
| A 帧 | `local $local` | 需 ctx（is_entry/is_raw）+ 帧/ABI |
| B core 余 | `cmp` | 通用值比较（含字符串，arch 差异大） |
| B' 调用 | `fcall ycall ccall iccall tcall` | 形态共享，寄存器/ABI 各异 |
| B'' | `ret exit` | 需 ctx |
| C raw | `$and $add $addi … $icall $f64*` | 逐 op；arch 特有（`$bt/$smap/$sp/$fp`）也进 opmap |
| D 余 | `kind alloc alloc_s ld64 st64 write1 clock glob gst time_ms time_str argc argv` | `kind` 有分支；`glob/gst/gvar/gval/gset` 的 tag 协议封在 op 内 |
| E | `closure icall` | 复用 alloc/store |
| F | `memcpy` | |
| G | `mkcont throwk cc_recv` | |
| H | `untag syscall` | |
| I | prim 兜底 | |

寄存器抽象：op **内部自行用 arch 物理寄存器**（arm64 x0/x1/x2、x86 rax/rbx/rdi…）；
共享骨架不引入逻辑寄存器。唯一例外是 `emit_i_core_base`——它用逻辑 `T0/T1`，由各
arch 的 `*_ops` 映射（x86 T0=rax/T1=rbx，arm64 T0=x0/T1=x1，riscv T0=t1/T1=t3）。

---

## 5. 迁移状态：**全部完成**（2026-09）

三后端的**每一条 LIR op** 都已按 §3 的注册式迁完：arch 只写 op 实现（
`<arch>_cop_*` / `<arch>_op_*`）并在包加载时 `emit_reg`，`emit.yac` 持有注册表并分派。

| 组 | 内容 | 状态 |
| --- | --- | --- |
| prog 循环 | `emit_funs_loop` | ✅ 三后端 |
| core | 数据/控制 + 调用/终止 + `label local $local cmp` | ✅ 三后端（`i_core = emit_insn_disp`） |
| heap | `kind mref mset tag is_int alloc alloc_s ld64 st64 write1 clock glob gst gvar gval gset obj_sti obj_st_int mref8 mset8 argc argv time_ms time_str` | ✅ 三后端（`i_heap = emit_insn_disp`） |
| prim | `nil cons strlit str_len str_ref bytes_* len nth tail append list_* str_cat str_slice int_to_str drop foldl map read_file write_file` | ✅ 三后端；arm64/riscv 走共享 `emit_rt_call` + 表驱动注册，x86 手写注册 |
| raw | `$and $addi $add $sub $bts $bt $or $clamp0 $st64 $st8 $ld64 $ld8 $sp $fp $smap $gbase $glob $icmp $jcc $memcpy $memset $shr $carg $icall` | ✅ 三后端（`i_raw = disp → syscall rest`） |
| f64 | `$f64fromstr $f64binop $f64rel $f64print` | ✅ 三后端 |
| memcpy / cc / clos / apply / sys | `memcpy mkcont throwk cc_recv closure icall untag syscall` | ✅ 三后端 |

- 入口：`emit_insn = emit_insn_disp`，未注册返回 0 由各 arch 的 `*_rest`（仅剩 syscall
  兜底）处理。
- 复用点：`emit_rt_call(st, insn, name, argn, op_slot, op_rt)` + `emit_rt_ops` 表——
  arm64/riscv **零重复**（各一个 `*_op_rt` + `*_rt_slot`，注册遍历共享表）；arm32 将来
  同样只写这两样。
- 验收：每步 `make yc`（两趟自举）+ `make test-compiler` 201/0 + `make test-iso` 321/0。

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
