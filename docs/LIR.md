# LIR.md — LIR 权威定义（跨架构，接近机器）

> **本文是 LIR 的唯一权威定义。** `DESIGN.md` §2 的 LIR 语法节已改为指向本文。
> 本文同时是一份**清点报告**：每条指令标注**现状**（是否有生产者）。
> 定义与实现不一致的地方，以本文为准去改实现。

## 0. 为什么需要本文

在立本文之前，LIR 有**三份互不相同的"指令集"**：

| 来源 | 内容 |
|---|---|
| `DESIGN.md` §2 语法 | 列了 `nop` / `save` / `restore` / `neg` / `lnot` / `print`（**emit 里没有这些分支**）；又漏了 `gvar` / `xcall` / `throwk` / `$`-族 等 |
| `emit_x86_64.yac` 分派链 | 含 **32 条无生产者的 handler**（见 §7.2） |
| `lir.yac` + `rt/runtime.yac` 生产 | 约 70 条真正在用的 |

**一个 IR 如果允许"实现了但没人用"和"用了但没实现"同时存在，它就不是规范，只是一堆约定。**

## 1. 定位与不变量

**定位**：LIR 是**槽机**。`s` 是虚拟槽号（整数），后端映到帧上 8 字节格；值统一 tagged
（int = `n<<1`，堆指针为奇数）。LIR 是**跨架构**的 —— `emit_x86_64` / `emit_arm64` /
`emit_riscv64` 吃同一套。

**不变量**（⚠️ 标注现状违反处）：

| # | 不变量 | 现状 |
|---|---|---|
| 1 | 线性指令序列；只有 `label` / `jmp` / `cmpjmp` / `$jcc` 制造控制流 | ✅ |
| 2 | 每条指令的 dst 是槽号；**`dst = 0` 不表示"无结果"**，槽号从 1 起 | ⚠️ `lir_atom` 的兜底用 `0` 当哨兵（§7.3） |
| 3 | **LIR 不认识镜像边界** —— 只发名字；槽位分配、跨镜像解析、打补丁是 emit 的事 | ⚠️ `xcall` 把"跨镜像"写进了 opcode（§8 A1） |
| 4 | **LIR 不认识"顶层"** —— 静态名集合由调用方算好传入 | ❌ `topfn_has`（全局 box）出现在 `lir.yac` 里 |
| 5 | **闭包值表示统一**：`nenv=0` 的静态单元与堆闭包同形，`icall` 无需判别 | ✅（这是静态 cell 布局的价值） |
| 6 | **没有魔法值** | ❌ `apply_ncap` 用 `-1` 表"动态"（§8 A1） |
| 7 | **每条指令都有生产者，每个生产者都有消费者** | ❌ 32 条单向（§7.2） |
| 8 | **未知指令 / 未知形式必须报错**，不得静默跳过 | ❌ 三处静默兜底（§7.3） |

第 7、8 条最要紧 —— 它们是"这份规范能不能被机械校验"的前提。

## 2. 程序与过程

```
prog      ::= ["prog", [proc*], entryName]

proc      ::= ["proc",  name, nparams, ncap, [insn*], srcname]
              ; 客过程。name = 码名（可能带包前缀 / #uid 后缀），srcname = 源级名（profiler 用）
              ; ncap = 真捕获数；nparams = 形参个数
              ; 槽布局：1..ncap 捕获，ncap+1..ncap+nparams 形参，之后是局部

$proc     ::= ["$proc", name, nparams, ncap, [insn*], srcname]
              ; 原生过程（rt/runtime.yac 手写）。帧不做 GC 扫描；用 $ 家族指令
```

**识别原生过程**：`nth(f,0) == "$proc"`，或首条指令是 `["$local", …]`
（`emit.fun_is_raw`）。

**指令序列必须以帧指令开头**：`["local", nslots, nparams]` 或 `["$local", …]`。
`local` 是读 argc/argv、建帧、写 GC `stack_hi` 的地方，**必须排在所有发布
（`gset` / `gfnst`）与分配之前**。

## 3. 指令集（权威）

每条的"生产"列：**A** = `front/lir.yac`，**R** = `rt/runtime.yac`，**—** = 无生产者。

### 3.1 帧与栈

| 指令 | 形态 | 生产 | 说明 |
|---|---|---|---|
| `local` | `["local", nslots, nparams]` | A R | 建帧 + 登记 GC 扫描范围 |
| `$local` | `["$local", nslots, nparams]` | R | 原生帧，**不做 GC 扫描**（`smap_set_nslots(0)`） |
| `$sp` | `["$sp", dst]` | R | `dst = rsp` |
| `$fp` | `["$fp", dst]` | R | `dst = rbp` |
| `$carg` | `["$carg", argreg, src]` | R | 把槽 `src` 装进参数寄存器 **`argreg`**（注意首操作数是寄存器号，不是槽号） |
| `$smap` | `["$smap", dst]` | R | 栈图地址（GC 用） |

### 3.2 搬运与算术

| 指令 | 形态 | 生产 |
|---|---|---|
| `mov` | `["mov", dst, src]` | A R |
| `mov_imm` | `["mov_imm", dst, imm]` | A R |
| `add` `sub` `mul` `div` `rem` | `["add", dst, a, b]` | A R |
| `land` `lor` `xor` | `["land", dst, a, b]` | A R |
| `bnot` | `["bnot", dst, src]` | A |
| `shl` `shr` | `["shl", dst, a, b]` | A R |
| ~~`sal`~~ ~~`sar`~~ | — | **—** 死 |
| `$and` | `["$and", dst, src, imm]` | R |
| `$addi` | `["$addi", dst, src, imm]` | R |
| `$add` `$sub` | `["$add", dst, a, b]` | R |
| `$or` | `["$or", dst, src]` | R |
| `$shr` | `["$shr", dst, src, imm]` | R |
| `$clamp0` | `["$clamp0", dst, src]` | R |
| `$bt` | `["$bt", dst, base, bit]` | R |
| `$bts` | `["$bts", base, bit]` | R |
| ~~`$lea`~~ | — | **—** 死 |

### 3.3 比较与分支

| 指令 | 形态 | 生产 |
|---|---|---|
| `cmp` | `["cmp", op, dst, a, b]`，`op ∈ {==,!=,<,<=,>,>=}` | A |
| `icmp` | `["icmp", op, dst, a, b]` | R |
| `$icmp` | `["$icmp", op, dst, a, b]`，`op ∈ {≥u, ==, <, else}` | R |
| `label` | `["label", L]` | A R |
| `jmp` | `["jmp", L]` | A R |
| `cmpjmp` | `["cmpjmp", cond, then, else]`，`cond` 非 0 → `then` | A R |
| `$jcc` | `["$jcc", op, a, b, then, else]` | R |

### 3.4 调用

**目标形态（3 条）**：

| 指令 | 形态 | 说明 |
|---|---|---|
| `call` | `["call", dst, target, args, caps]` | `target = ["name", nm]` 或 `["slot", s]` |
| `tcall` | `["tcall", target, args, caps]` | 尾位置，**无 dst**（控制转移，形状不同） |
| `ccall` | `["ccall", dst, name, args]` | C ABI 调用 |

```
caps ::= ["static", n]   ; 编译期已知 n 个前导捕获；n = 0 即 flat
       | ["dyn"]         ; 运行期 nenv：从 [obj+24] 取个数，从 [obj+32+i*8] 取值
```

**现状（10 条，待收敛）**：

| 指令 | 形态 | 生产 | → 目标 |
|---|---|---|---|
| `fcall` | `["fcall", dst, name, [args]]` | A R | `call(["name",nm], caps=["static",0])` |
| `xcall` | `["xcall", dst, tmp, name, [args]]` | A | `call(["name",nm], …)`（解析交给 emit 策略点） |
| `icall` | `["icall", dst, slot, [args]]` | A R | `call(["slot",s], caps=["dyn"])` |
| `apply` | `["apply", dst, slot, ncap, [args]]` | A | `call(["slot",s], caps=["static",n])` |
| `tcall` | `["tcall", dst, name, [args]]` | A R | `tcall(["name",nm], …)` |
| `ticall` | `["ticall", dst, slot, [args]]` | A | `tcall(["slot",s], caps=["dyn"])` |
| `tailapply` | `["tailapply", dst, slot, ncap, [args]]` | A | `tcall(["slot",s], caps=["static",n])` |
| `ccall` | `["ccall", dst, name, [args]]` | A R | 保留 |
| `iccall` | `["iccall", dst, callee, [args]]` | A | 并入 `ccall`（动态变体） |
| ~~`gcall`~~ | — | **—** | 消失（rev1 残留） |
| `$icall` | `["$icall", dst, fn_slot]` | R | 保留（raw 家族，不解包，直接 `call`） |

> **尾位置**：ANF 已改为 `tail` 结构化（见 `DESIGN.md` §2.1），因此 LIR **只发**
> `tcall` —— `lir.yac` 的 `maybe_tcall` 事后改写**应删除**。

### 3.5 名字单元（静态化）

每个顶层名字一个 32B 静态单元，放在 globals 数据区（**不在 GC 堆**）：

```
cell + 0   : value        ← 顶层 let 的值      (gval 读这里)
cell + 8   : mark
cell + 16  : entry        ← 顶层 letfun 的入口  (gvar 用；icall 读 [+16])
cell + 24  : nenv = 0     ← 常量 0（使该单元本身即合法的零捕获闭包）
cell + 32… : env…
```

| 指令 | 形态 | 生产 | 说明 |
|---|---|---|---|
| `gvar` | `["gvar", dst, name]` | A | `dst = cell \| 1`（tagged）—— 函数当值用 |
| `gval` | `["gval", dst, name]` | — | `dst = [cell + 0]` —— 读顶层值。**待实现**（= 现有 `gvld` 改名） |
| `gset` | `["gset", name, off, src]` | — | `[cell + off] = src`，`off ∈ {0, 16}`。**待实现**（合并 `gvst` + `gfnst`） |
| `gfnst` | `["gfnst", name]` | A | 发布顶层函数入口到 G+448 运行时注册表（`yac_gfn_pub`） |
| ~~`gvld`~~ | — | **—** | rev1 残留 |
| ~~`gvst`~~ | — | **—** | rev1 残留 |

### 3.6 闭包与对象

| 指令 | 形态 | 生产 | 说明 |
|---|---|---|---|
| `closure` | `["closure", dst, fnName, [capSlots]]` | A | 堆闭包 `[next][mark][fnptr][nenv][env…]`；`capSlots` 是**外层帧**的槽号 |
| `alloc` | `["alloc", dst, nbytes]` | R | GC 堆分配 |
| `alloc_s` | `["alloc_s", dst, nbytes]` | R | 分配但跳过 GC 链表登记 |
| `obj_kind` | `["obj_kind", dst, obj]` | R | 取对象种类 |
| `obj_sti` | `["obj_sti", obj, off, src]` | R | 存字段（槽号源） |
| `obj_st_int` | `["obj_st_int", obj, off, imm]` | R | 存字段（立即数源） |
| `mref` | `["mref", dst, obj, off]` | R | 读字段，obj 先去 tag，结果原样 |
| `mset` | `["mset", obj, off, src]` | R | 写字段 |
| `mref8` | `["mref8", dst, obj, off]` | R | 按 8 位访问，**结果重新打 tag** |
| `mset8` | `["mset8", obj, off, src]` | R | |
| `ld64` | `["ld64", dst, obj, off]` | R | obj 去 tag，读 qword，**不重新打 tag** |
| `st64` | `["st64", obj, off, src]` | R | |
| `tag` | `["tag", dst, src]` | R | 打 tag |
| `is_int` | `["is_int", dst, v]` | A R | 整数判定 |
| `untag` | `["untag", dst, src]` | A R | 去 tag |

> **§8 A3 的合并依据**：`mref` / `mref8` / `ld64` 的差异只有三个维度 ——
> 偏移形式（静态 `disp32` / 动态槽号）、宽度（64 / 8）、结果是否重打 tag。
> 可实现为 `["mref", dst, obj, off, w, retag]` / `["mset", obj, off, src, w]`。

### 3.7 原生内存

| 指令 | 形态 | 生产 |
|---|---|---|
| `$ld64` | `["$ld64", dst, base, off]` | R |
| `$st64` | `["$st64", base, off, src]` | R |
| `$ld8` | `["$ld8", dst, base, off]` | R |
| `$st8` | `["$st8", base, off, imm]` | R |
| `$memcpy` | `["$memcpy", dst, src, n]` | R |
| `$memset` | `["$memset", dst, n]` | R |
| `memcpy` | 同 `$memcpy` | R |
| `$glob` | `["$glob", dst, which]` | R |
| `$gbase` | `["$gbase", dst]` | R |
| `glob` | `["glob", dst, i]` | R |
| `gst` | `["gst", i, src]` | R |
| `write1` | `["write1", …]` | R |
| `syscall` | `["syscall", s, nr, [args]]` | A |
| `$syscall` | — | R |

### 3.8 浮点

| 指令 | 形态 | 生产 |
|---|---|---|
| `$f64fromstr` | `["$f64fromstr", dst, src]` | R |
| `$f64binop` | `["$f64binop", dst, a, b, op]`，`op ∈ {0:add, 1:sub, 2:mul, 3:div}` | R |
| `$f64rel` | `["$f64rel", dst, a, b, op]` | R |
| `$f64print` | `["$f64print", dst, src]` | R |

### 3.9 控制与运行时

| 指令 | 形态 | 生产 | 说明 |
|---|---|---|---|
| `ret` | `["ret", slot]` | A R | entry 过程为裸 `ret`；普通过程补 `leave` |
| `exit` | `["exit", slot]` | A | `untag` 后走 OS 退出 |
| `throwk` | `["throwk", k, v]` | A | 抛给续延 |
| `mkcont` | `["mkcont", …]` | A | 一等续延 |
| `cc_recv` | `["cc_recv", …]` | A | 续延接收 |
| `strlit` | `["strlit", dst, bytes]` | A | rodata 字面量，结果 tagged 指针 |
| `str_len` | `["str_len", dst, src]` | A | 内联（对象字段读取） |
| `str_ref` | `["str_ref", dst, src, i]` | A | 内联 |
| `bytes_len` | `["bytes_len", dst, src]` | A | 内联 |
| `clock` | `["clock", dst]` | R | |

### 3.10 ⚠️ 内联原语族（整族无生产者 —— 待决策）

以下指令在 emit 里**有完整实现**，但**全仓没有任何生产者**（前方已扫描确认）。
实际走的是 `lir_rt_*` → `["fcall", dst, "yac_xxx", …]`。

| 族 | 指令（全部 **—**） |
|---|---|
| 列表 | `nil` `cons` `len` `nth` `tail` `append` `drop` `list_new` `list_push` `list_rev` |
| 字符串 | `str_cat` `str_slice` `int_to_str` `bytes_to_str` |
| bytes | `bytes_new` `bytes_ref` `bytes_put` `bytes_append` `bytes_extend` |
| 文件/系统 | `read_file` `write_file` `time_ms` `time_str` `argc` `argv` |
| 其他 | `print` |

**这是一个必须先做的决策，它决定指令集大小：**

| | 内联指令 | `fcall yac_*`（现状） |
|---|---|---|
| 性能 | 快（`cons` / `nth` 是热路径） | 慢（每次搬 6 个参数 + `call`/`ret`） |
| emit 复杂度 | 高（每架构各写一遍） | 低（一份，在 `runtime.yac`） |
| 代码大小 | 小 | 大 |

**建议**：默认保留 `fcall yac_*`，只对最多 3~5 个最热的做内联；
**但绝不该两套同时存在**。（Chez 的 `np-expand-primitives` 也是只内联一部分。）

### 3.11 raw 家族（`$` 前缀）

**契约**：`$` 前缀的指令**只允许出现在 `$proc` 内**，操作数可以是未打 tag 的裸值。
它们不参与 GC 安全点，也不做 ABI 适配 —— 是 `rt/runtime.yac` 写内核的"汇编层"。

完整清单见 §3.1–3.9 中标注 **R** 且以 `$` 开头的条目，共 30 条。

## 4. 闭包在 LIR 里的落点（5 处）

闭包转换不是一个 pass，它的痕迹散在 LIR 的 5 个地方 —— 这是"没有 `clos` 层"的代价：

| # | 位置 | 形态 | 职责 |
|---|---|---|---|
| 1 | `proc` 头 | `ncap` | 把"捕获几个"烘焙成 ABI 字段 |
| 2 | 定义点 | `["closure", dst, gname, [capSlots]]` | 在**外层帧**分配，capSlots 由 `outer_caps` 查外层槽 |
| 3 | proc 体内（自引用） | `["closure", self_slot, gname, [1..ncap]]` | 自递归用：捕获**自己**的入参捕获槽 |
| 4 | 调用点 | `fcall` 的 `cap·ss` / `apply` 的 `ncap` / `icall` 的运行时 `nenv` | caps 前缀约定 |
| 5 | 名字当值 | `lir_clos_atom` → `["closure", dst, fnName, []]` | **0 捕获**闭包（见 §7.4） |

**四件事挤在 `lir_letfun_*` 里**：自由变量分析 / 闭包分配 / 调用约定 / 表示判定。
其中**表示判定只有一行**：`flat = topfn_has(name) and ncap == 0`。

**近期改动（轻量版）**：

```
proc ::= ["proc", name, nparams, ncap, [insn*], srcname, [fvs]]     ← 末尾追加
```

`fvs` 是自由变量表（`free_vars` 的输出），现在算出来被丢掉，只剩 `ncap` 这个数字。
**追加而非插入** —— `fun_is_raw` 读 `nth(f, 4)`（insns），插入会顶掉所有下标。

**远期（完整版）**：在 ANF 与 LIR 之间加显式 `clos` 层（Chez 的 L6）：

```
closbind ::= ["closures", [cl*, tail]]
cl       ::= [name, [fv*], [params], body]
```

届时 §4 的 5 处落点全部收敛到这一层。**触发条件**：做 well-known
（`singleton`/`borrowed`/`pair`/`vector`）时 —— 那要求闭包是可分析对象。

## 5. LIR → 机器码

**三段流水线**，每段是独立的模块层：

```
LIR insn*  ──① 指令选择 + 框架──▶  目标指令序列（字节）
           ──② 编码──────────▶  text 字节流
           ──③ 容器打包──────▶  ELF64 / PE / Mach-O 文件
```

| 段 | 模块 | 产物 |
|---|---|---|
| ① 指令选择 + 框架 | `back/emit/emit_<arch>.yac`（`emit_x86_64` / `emit_arm64` / `emit_riscv64`）+ `back/encode/encode_<arch>.yac`（逐条指令的编码器） | 字节 + 未决 patch 表 |
| ② patch 求解 | `back/emit/emit.yac` 的 `emit_resolve_loop` / `emit_resolve_patch` | 全部立即数/相对位移就位 |
| ③ 容器 | `back/pack/{elf,pe,macho}.yac` + `pack.yac` + `target.yac`；JIT 走 `pack/yjit.yac` | 可执行映像 / blob |

### 5.1 编码层的语法范式

**没有"机器码的 yac list"** —— 机器码的形态是**文件**，其语法：

```
image     ::= ELF64 | PE | MachO

ELF64     ::= ehdr phdr* text (globals…)
ehdr      ::= 64 字节 ELF header
phdr      ::= 56 字节 program header（PT_LOAD）

text      ::= encoded*                     ; 按 --arch 选 encode_*
encoded   ::= x86-64 | arm64 | riscv64 字节序列
```

**布局**：`LOAD_VADDR + TEXT_OFF` 起是 `text`，紧随其后是 `globals` 数据区；每个
顶层名字一个 **32B cell**，索引 `i` 的地址 = `globals + 448 + 32*i`（cell 布局见 §3.5）。

### 5.2 patch 语言（延迟求解）

指令选择期不知道最终地址，于是记下 **`[tag, …]`**，由 ② 段求解。这是 LIR → 机器码
之间最重要的一层抽象：

| tag | 含义 |
|---|---|
| 1 / 12 | label 相对偏移（同过程内） / 字符串池偏移 |
| 2 | 代码地址绝对 64 位：`codebase + offs[fid]` |
| 3–8 | globals 基址 + 固定偏移（`$gbase` / `$glob which=1..3` / map / 平台槽） |
| 11 / 14 | TCO `$tco` 回跳位置 / 栈图地址 |
| 15–20 | PE 平台符号偏移（`uname` / `dlsym` / `system` / `dlopen` / unimplemented stub） |
| 21 | host 槽：`id < 10` → `globals+136+8*id`；否则 → `globals+456+8*(id-10)`（extern） |
| 22 | **cell 地址**：`globals + goff`。用于 `gvar` / `gval` / `xcall` / `gcall` |
| 23 | **cell 入口 bake**：`[globals+goff+16] = codebase + offs[fid]`。**只有 `gvar` 的 AOT 路径发** |

> ⚠️ tag 23 只有 `gvar` 发。`xcall` 只发 tag 22，因此它依赖"该名字也以 `gvar`
> 出现过"来填 `[cell+16]`。修法见 §7.5。

### 5.3 指令选择（LIR insn → 目标指令序列）

形态约定（完整映射是各 `emit_<arch>.yac` 的分派链，机械对应）：

| LIR | x86_64 | 说明 |
|---|---|---|
| `mov_imm dst, imm` | `movabs rax, imm` → store `[rbp+8*dst]` | 立即数是**已编码**的 64 位模式（不再 `<<1`） |
| `mov dst, src` | 两次帧搬运 | |
| `add dst,a,b` | `mov rax,[a]` · `add rax,[b]` · store `[dst]` | 临时值走 `rax` |
| `cmp op,dst,a,b` | `cmp rax,rbx` · `setcc` · `movzx` · `shl rax,1` | 结果是 tagged bool |
| `cmpjmp c,Lt,Lf` | `mov rax,[c]` · `test rax,rax` · `jnz Lt` · `jmp Lf` | |
| `label L` / `jmp L` | 记位置 / `jmp rel32` | 偏移由 tag 1 求解 |
| `fcall dst,name,args` | 前 6 → 寄存器、其余入栈；`call rel32`（本镜像）或经全局槽 `call r11`（host/extern） | 见 §5.4 |
| `icall dst,slot,args` | 从槽取闭包 → `and rax,1` → `mov rbx,[rax+16]` → 按 `[rax+24]` 动态展开 caps → `call rbx` | **动态 nenv**：现场生成跳转表 |
| `apply dst,slot,ncap,args` | 同上，但 `ncap` 编译期已知 → 直接展开 | |
| `tcall`（self） | 参数压栈 → 弹回槽 `1..n` → `jmp $tco`（**不拆帧**） | 见 §5.5 |
| `tcall`（他函数） | `≤6` 参：搬运 → `mov rsp,rbp; pop rbp; jmp rel32`（**兄弟调用**）；`>6` 参：退化成 `call` + `leave/ret` | |
| `ccall dst,name,args` | C 协议 marshal（去 tag / 取值指针）· `call rel32` · `shl rax,1` 打 tag | |
| `ret s` | entry 过程：裸 `ret`；否则 `mov rsp,rbp; pop rbp; ret` | `_start` 是 entry |
| `closure dst,name,caps` | `yac_alloc` → 存 `[+16]` fnptr（patch）· `[+24]` nenv · `[+32+8i]` env | |
| `gvar dst,name` | `movabs cell` · `or rax,1` · store（AOT 再 bake `[cell+16]`） | |
| `local n,np` | `push rbp` · `mov rbp,rsp` · `sub rsp,8*n` · 写 GC `stack_hi` | |

### 5.4 约定

| 约定 | 内容 |
|---|---|
| 槽号 → 帧偏移 | `[rbp - 8*s]` 量级；由 emit 决定，LIR 不关心 |
| tag | int = `n<<1`；`nil` = `1`；`true` = `2`；堆指针为奇数 |
| 参数传递 | 内部 yac ABI：x86_64 前 6 个寄存器（`rdi rsi rdx rcx r8 r9`）其余入栈（callee 见 `[rbp+16+…]`）；arm64/riscv64 前 8 个 |
| 名字 → 地址 | **策略点 `fn_entry(name)`**（见 §5.6） |
| `syscall` | `nr = 60` 表示进程退出（arm64/riscv64 映成 93）；参数是要进寄存器的位模式 |
| `_start` 构造 | `local` → 顶层绑定 → 各顶层函数的 `gfnst` 发布 → `untag` + `syscall 60` |
| 栈对齐 | `call` 前 SP 16 字节对齐（`emit_x86_c_align` / `unalign`） |

### 5.5 TCO 的两条路径

| 情形 | 实现 | 是否 TCO |
|---|---|---|
| self 尾递归 | `emit_x86_tloop`：参数压栈 → 弹回槽 `1..n` → `jmp $tco`（**不拆帧**） | ✅ arity 不限 |
| 跨函数尾调用，`≤6` 参 | `emit_x86_tcall_other`：搬运 → `mov rsp,rbp; pop rbp; jmp rel32` | ✅（**兄弟调用**） |
| 跨函数尾调用，`>6` 参 | 退化成 `call` + `add rsp` + `leave/ret` | ❌（emit 自己注释 `Not a jmp`） |

> ⚠️ **跨函数 TCO 现在从 `lir.yac` 侧无人生成** —— `maybe_tcall` 只在
> `nth(insn,2) == self` 时改写。`tcall_other` 的 `≤6` 那条兄弟调用路径是**准备好
> 但没人用**的。打通它只需要 ANF 侧的 `callι` 对任意 `tail?` 目标发 `tcall`
> （见 `ANF.md` §3.4），后端不用动。

### 5.6 现状缺陷：`fn_entry` 不存在

"装一个 callee 地址再间接调用"这段逻辑在 emit 里**写了三遍**：

| 位置 | handler |
|---|---|
| `emit_x86_64.yac` `gvar` | `movabs 0 占位 → patch[22] → or1 → store`（AOT 再加 patch[23]） |
| `emit_x86_64.yac` `xcall` | 同上，后接 `icall` |
| `emit_x86_64.yac` `fcall` 的 `via_slot` 分支 | `movabs 0 占位 → patch[21] → mov r11 → call r11` |

**应该收敛成一个策略点**：

```
fn_entry(name) -> 绝对地址 | 桩槽      ; AOT = 布局期 bake；yjit = 布局期填；blob = jsess patch
emit_callee_ref(name) -> 寄存器        ; 内部调 fn_entry
```

新增镜像形态 = 加一个 `fn_entry` 分支 + 一个填表者，**不动 LIR**。这也是 §8.A1
（`xcall` 消失）的抓手 —— **先合并三份重复，`xcall` 会自然退化成 `call` 的一个分支**。

## 6. 校验规则（应当实现的 `--verify-lir`）

| # | 检查 |
|---|---|
| 1 | 每个 `proc` 首条指令是 `local` / `$local` |
| 2 | 每条指令的首元素在 §3 的指令集里（**未知指令 → 报错**） |
| 3 | 每条指令的操作数个数与 §3 一致 |
| 4 | 所有槽号 ∈ `1 .. nslots`（`nslots` 来自首条 `local`） |
| 5 | 所有 `jmp` / `cmpjmp` / `$jcc` 的目标 label 在同一 `proc` 内有定义 |
| 6 | `$` 指令只出现在 `$proc` 内 |
| 7 | `closure` 的 `capSlots` 全部 ∈ 外层帧的槽范围 |
| 8 | `ncap == len(fvs)`（§4 轻量版落地后） |
| 9 | 每个 `proc` 的码名在本单元内唯一 |

**校验器的价值**：把 §1 的不变量从"文档承诺"变成"机器可验"。

## 7. 一致性问题清单

### 7.1 文档与实现漂移

`DESIGN.md` §2 的 LIR 语法列了 **`nop` / `save` / `restore` / `neg` / `lnot` / `print`**，
其中前五个在 `emit_x86_64.yac` 的分派链里**没有分支**，`print` 是自身标注的 leftover。
该节已改为指向本文。

### 7.2 死 handler（32 条）

见 §3.2 / §3.4 / §3.5 / §3.10 中标 **—** 的条目：

- 内联原语整族 **26 条**（§3.10）
- `sal` / `sar` / `$lea` / `print` **4 条**
- rev1 残留 `gvld` / `gvst` / `gcall` **3 条**

处理方式取决于 §3.10 的决策：**要么删，要么恢复生产者**。

### 7.3 三处静默兜底（必须改成报错）

| 位置 | 现状 | 后果 |
|---|---|---|
| `lir_atom` 的 `else` | 返回槽 `0` | 未知 atom → 静默产出垃圾值 |
| `lir_expr_i` 的 `else` | `pack_i(st, i + 1)`，跳过 | 未知 bind → 静默漏掉一段计算。**ANF 已预留 `letrec`，更必须报错** |
| `emit_insn_go` 的 `else st` | 什么都不做 | 未知指令 → 静默忽略 |

三者合起来：**任何"用了但没实现"的形式都会静默产出错误代码而不是报错** ——
这也是上面那些漂移长期没被发现的原因。

### 7.4 `lir_clos_atom` 的 0 捕获

`lir_clos_atom` 造 `["closure", dst, fname, []]` —— **声明 0 捕获**。
只对被引用 proc 的 `ncap == 0` 时安全。若 `ncap > 0`：

- 闭包声明"我没有捕获" → 调用时不传前导 caps
- 但函数体照样从槽 `1..ncap` 读 → **读到垃圾**

它对 `print`（ncap=0 的 runtime 名）是对的；对任意 proc 名需要**断言 `ncap == 0`**。

### 7.5 `topfn_has` 泄漏

`topfn_has` 是一个全局 box，属于**前端概念**（"哪些名字是本编译单元的顶层函数"），
却出现在 `lir.yac` 里（`free_vars` / `lir_var` / `calli` / `lir_letfun_finish`）。
且依赖调用方**记得**先调 `topfn_reset` + `topfn_scan`
（`backend.yac` 两处都调了，`dump_lir` 忘了 —— 见 §7.6）。

**改法**：静态名集合作为**参数**传入 `lir_all`，LIR 内部不认识"顶层"。

### 7.6 `--dump-lir` 不忠实

`dump_lir`（`backend.yac:857`）+ `lir_dump_items`（`:845`）与真实路径 `pass_lir`（`:767`）
有**四处语义差异**：

| 缺什么 | 后果 |
|---|---|
| `topfn_scan(anf, 0)` | `topfn_has` 恒 false → **顶层函数不 flat** |
| 跨 item 累积 `st` | Σ 不累积 → 引用前一个 item 定义的函数落到 **`xcall`** |
| `sigma_of_rt(rt0)` | Σ 里没有 runtime proc → runtime 名解析不到 |
| `start_proc` + `tco_prog` | 看不到 `gfnst` 发布序列，**完全看不到 `tcall` / `$tco`** |

实测证据（`yc_l1 --dump-lir`，输入 `let f(n) = if n<=0 then 0 else n+f(n-1)` + `f(10)`）：

```
[[closure, 1, f, []]]                        ← f 本应 flat（ncap=0），却发了 closure 分配
[[mov_imm, 1, 20], [xcall, 2, 3, f, [1]]]    ← 同单元调用落到了 xcall
```

**修法**：`dump_lir` 直接复用 `pass_lir`。

## 8. 改动清单

### A. 指令集收敛（纯重构，golden 不变）

| # | 现状 | 目标 |
|---|---|---|
| A1 | call 家族 **10 条** + 死代码 `gcall`，`apply_ncap` 用 `-1` 表动态 | **3 条**：`call` / `tcall` / `ccall` + `target` / `caps` 字段 |
| A2 | 名字单元 **4 条** | **3 条**：`gvar` / `gval` / `gset`（`off ∈ {0,16}`） |
| A3 | 内存访问 **8 条** | **2 条**：`mref` / `mset` + 宽度 / 偏移形式 / retag 字段 |
| A4 | 内联原语族两套并存 | **决策**（§3.10）后统一 |

### B. 结构与不变量

| # | 内容 | 依据 |
|---|---|---|
| B1 | 删 `maybe_tcall`（尾位置由 ANF 的结构给出） | §3.4 |
| B2 | `proc` 追加 `[fvs]`（轻量版闭包显式化） | §4 |
| B3 | `lir_clos_atom` 加 `ncap == 0` 断言 | §7.4 |
| B4 | 三处静默兜底改报错（`lir_atom` / `lir_expr_i` / `emit_insn_go`） | §7.3 |
| B5 | 实现 `--verify-lir` | §6 |
| B6 | `topfn_has` 出 LIR，改成参数 | §7.5 |
| B7 | `self` 的 `"F"` 哨兵改成显式 option；`tco_name_ok` 里硬编码的 `"parse_expr"` 要么修根因要么写明注释 | §3.4 |
| B8 | 修 `--dump-lir` 复用 `pass_lir` | §7.6 |
| B9 | 实现 `fn_entry` 策略点，合并三份重复的"装 callee 地址" | §5 |

### C. 性能结构（独立于 A/B）

| # | 内容 | 现状 |
|---|---|---|
| C1 | **槽复用**（活跃性分析 + 着色） | `nslots = nth(br,3) + 2`，`ctr` 每临时 +1，**从不复用** |
| C2 | **算术内联**（int 快路径 + 溢出检查） | `bin("+",d,a,b) = ["fcall", d, "yac_num_add", [a,b]]` —— 每次 `+` 都是一次过程调用 |
| C3 | 常量折叠 | `2*3` 仍发 `mov_imm;mov_imm;mul` |

### D. 建议顺序

| 步 | 内容 | 为什么 |
|---|---|---|
| **1** | **决策 A4**（内联原语 vs `fcall`） | 决定指令集大小，后面全依赖 |
| **2** | 立本文（已完成）+ `DESIGN.md` §2 改为指向本文 | 先有唯一规范 |
| **3** | 修 `--dump-lir`（B8） | 后面每一步的验收都要它 |
| **4** | 三处静默兜底改报错（B4）+ `--verify-lir`（B5） | 让不符合规范的东西**立刻暴露** |
| **5** | 清死 handler（A4 的结论） | 规范立起来后清死代码才有依据 |
| **6** | `fvs` 落进 `proc`（B2）+ 删 `maybe_tcall`（B1） | 小改动，解锁"读 fv 表"的验证 |
| **7** | 指令集收敛（A1 / A2 / A3） | 纯重构 |
| **8** | `topfn_has` 出 LIR（B6）+ 哨兵/硬编码（B7）+ `fn_entry`（B9） | 消除概念泄漏与重复 |
| **9** | 性能（C1 → C3 → C2） | 独立，可并行 |

**关键约束**：3 → 4 → 5 必须在 6 / 7 之前。6、7 都是"改 IR 形状"的事，而现在的
`--dump-lir` 报的是假象、三处静默兜底会掩盖错误。**先有可信的观测和严格的报错，
再改 IR。**

## 9. 与 Chez 的层对照

| Chez（`s/cpnanopass.ss`） | yac |
|---|---|
| `np-convert-assignments`（把被赋值的变量装箱） | **无**（yac 无赋值；ANF 的别名天然不可变） |
| L1–L5：`lambda`，无自由变量信息 | ANF：`letfun`（无 fv 信息） |
| `np-convert-closures` L5→L6，引入显式 `(closures …)` | **无对应层** —— 压进 `lir_letfun_*` |
| `np-optimize-direct-call` / `np-identify-scc` / `np-lift` | **无**（缺调用图 → 缺 well-known） |
| `np-expand/optimize-closures` L6→L7（决定闭包表示） | 一行 `flat = topfn_has(name) and ncap == 0` |
| `np-impose-calling-conventions` L12.5→L13 | 隐含在 LIR 的 caps 前缀约定里 |
| `uncover-live` / `build-interference` / `color` / `assign-registers` | **无**（槽不复用） |
| `np-place-overflow-and-trap` | **无**（算术全走 runtime 调用） |

**yac 的 LIR 大致相当于 Chez 的 L7–L9 之间**（闭包已展开、表示已决定、尚未做
寄存器分配与指令选择）。
