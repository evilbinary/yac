# LIR.md — LIR 权威定义（跨架构，接近机器）

> **本文是 LIR 的唯一权威定义**，`DESIGN.md` §2 的 LIR 语法节已改为指向本文。
> **§4 是目标指令集**（唯一权威表）；**§5 是现状 → 目标的迁移表**。两者不混写。
> 定义与实现不一致的地方，以本文为准去改实现。

## 0. 为什么需要本文

立本文之前，LIR 有**三份互不相同的"指令集"**：

| 来源 | 内容 |
|---|---|
| `DESIGN.md` §2 语法 | 列了 `nop` / `save` / `restore` / `neg` / `lnot` / `print`（**emit 里没有这些分支**）；又漏了 `gvar` / `xcall` / `throwk` / `$`-族 等 |
| `emit_x86_64.yac` 分派链 | 含 **33 条无生产者的死 handler**（另有 6 条幽灵；§9.5） |
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
| 2 | 每条指令的 dst 是槽号；**`dst = 0` 不表示"无结果"**，槽号从 1 起 | ⚠️ `lir_atom` 的兜底用 `0` 当哨兵（§9.1） |
| 3 | **LIR 不认识镜像边界** —— 只发名字；槽位分配、跨镜像解析、打补丁是 emit 的事 | ⚠️ `xcall` 把"跨镜像"写进了 opcode（§5） |
| 4 | **LIR 不认识"顶层"** —— 静态名集合由调用方算好传入 | ❌ `topfn_has`（全局 box）出现在 `lir.yac` 里（§9.3） |
| 5 | **闭包值表示统一**：`nenv=0` 的静态单元与堆闭包同形，`icall` 无需判别 | ✅（这是静态 cell 布局的价值） |
| 6 | **没有魔法值** | ❌ `apply_ncap` 用 `-1` 表"动态"（§5） |
| 7 | **每条指令都有生产者，每个生产者都有消费者** | ❌ 32 条单向（§9.5） |
| 8 | **未知指令 / 未知形式必须报错**，不得静默跳过 | ❌ 三处静默兜底（§9.1） |

第 7、8 条最要紧 —— 它们是"这份规范能不能被机械校验"的前提。

## 2. 语法

```
prog      ::= ["prog", [proc*], entryName]

proc      ::= ["proc",  name, nparams, ncap, [insn*], srcname, [fvs]]
              ; 客过程。name = 码名（可能带包前缀 / #uid 后缀），srcname = 源级名（profiler 用）
              ; ncap = 真捕获数；nparams = 形参个数
              ; 槽布局：1..nparams 形参，之后是局部 —— 捕获【不住槽】，
              ;         经 ["cap", i] 从对象读（§4.4）
              ; [fvs] = 自由变量表（free_vars 的输出），恒有 ncap == len(fvs)
              ;   元素是【源级名字】（字符串），按 free_vars 的顺序排列；
              ;   fvs[i] 与 ["cap", i] 的 i、与定义点 ["closure",…,capSlots] 的
              ;   第 i 个槽位一一对应 —— 例：inner 的 fvs = ["f"]，ncap = 1

$proc     ::= ["$proc", name, nparams, ncap, [insn*], srcname, [fvs]]
              ; 原生过程（rt/runtime.yac 手写）。帧不做 GC 扫描；用 $ 家族指令

insn      ::= [op, operand*]        ; op 的操作数**个数**与含义由 §4 的形态列给出

op        ::= "local" | "$sp" | "$fp" | "$carg" | "$smap"                    ; §4.1
            | "mov" | "add" | "sub" | "mul" | "div" | "rem"                 ; §4.2
            | "land" | "lor" | "xor" | "shl" | "shr" | "bnot"
            | "$clamp0" | "$bt" | "$bts"
            | "cmp" | "icmp" | "label" | "jmp" | "cmpjmp" | "$jcc"          ; §4.3
            | "call" | "tcall" | "ccall"                        ; §4.4
            | "gvar" | "gval" | "gset" | "glob" | "gst" | "$gbase"          ; §4.5
            | "closure" | "alloc" | "obj_kind" | "mref" | "mset"            ; §4.6
            | "tag" | "untag" | "is_int"
            | "$ld64" | "$st64" | "$ld8" | "$st8"                            ; §4.7
            | "$memcpy" | "$memset" | "$syscall"
            | "$f64fromstr" | "$f64binop" | "$f64rel" | "$f64print"         ; §4.8
            | "ret" | "exit" | "throwk" | "mkcont" | "cc_recv" | "syscall"  ; §4.9
            | "strlit" | "str_len" | "str_ref" | "bytes_len" | "write1" | "clock"
            ; 共 67 条。§4.10 那一族内联原语
            ; **已决定全删**（三个后端都在清理）—— 它们不是"暂不入列"，而是**不存在**。

operand   ::= slot | imm | nameRef | slotRef | slotList | label | enum | capRef
slot      ::= 正整数                 ; 从 1 起；"0" 不是合法槽号（不变量 2）
imm       ::= ["imm", n]            ; 已编码的 64 位模式（int 已 <<1，不再补 tag）
nameRef   ::= ["name", nm] | ["fn", nm]     ; callee 位置的静态名字
slotRef   ::= ["slot", s]                   ; callee 位置的运行期值
slotList  ::= [operand*]                      ; 实参/槽列表 —— `call` / `tcall` 的 args、
                                            ; `closure` 的 capSlots、`$syscall` 的 srcSlots
                                            ; ⚠️ 元素可以是 ["cap", i] ✓（捕获直接当实参 ✓）
label     ::= 字符串                 ; 本 proc 内唯一
enum      ::= 裸符号 | 裸整数        ; 如 off = 16、which = 1..4、w = 8、conv = untag
capRef    ::= ["cap", i]            ; 第 i 个捕获 —— [self + 32 + i*8]（§4.4）
; ⚠️ 原 `caps ::= ["static", n] | ["dyn"] | ["raw"]` 已删除 —— 见 §4.4。
```

> **`slot` 与 `enum` 在文法上是同一形状，只有 §4 的表能区分。** `["imm", n]` 有标签
> 所以能认出来；但 `off = 16` / `which = 3` / `w = 8` 与 `slot = 16` 都是裸整数 ——
> **单看文法分不出来，必须结合该 `op` 的形态判断**（见下）。

> **`[fvs]` 是待落地的第 7 个字段（上面的文法写的是目标形态）。** 现状 `proc` / `$proc`
> 只有 6 个元素 —— `fvs` 在 `lir.yac:1145` 已经算出来、并随 `lir_letfun_begin` 的
> 元组返回（`nth(p, 6)`），到 `:1236` 构造 `proc` 时被丢掉，只剩 `ncap` 这个数字。
> 落地时**只能追加到末尾，不能插入**：`fun_is_raw` 读 `nth(f, 4)`（insns），
> 插入会顶掉所有下标。迁移与影响面见 §5.5 S2。

> **flat 形态（统一表示）。** `ncap == 0` 的过程**只有一种表示**：它的名字就是一个
> **静态 cell**（布局见 §4.5）—— `cell + 16` 是代码入口，`cell + 0` 是"函数当值"
> 时的单元地址本身，且 `nenv = 0` 使它**本身即一个合法的零捕获闭包对象**
> （不变量 5）。于是各处的形态是确定的：
>
> | 场景 | 形态 |
> |---|---|
> | 定义点 | **不发** `closure` 指令；名字即 cell |
> | 调用 | 只传**对象**作首实参；每次捕获引用经 `["cap", i]` 从对象读（§4.4） |
> | 名字当值 | `gvar`（`dst = cell \| 1`） |
> | 读顶层值 | `gval`（`dst = [cell + 0]`） |
>
> **判据只有 `ncap == 0` 这一条，与"是否顶层"无关**（`FLAT_ABI.md` 准则 2）；
> 表示判定**不得依赖名字表**（准则 5：判据必须是结构事实）。
>
> ⚠️ **落地状态**：现状只有**顶层** letfun 走 flat —— `lir_letfun_finish` 仍写着
> `flat = is_top and ncap == 0`（`lir.yac:1307`）。把判据**下沉到所有 lambda** 是
> `FLAT_ABI.md` 第 3 步；**定义点与引用点必须一起改**（`flat` 一放宽，
> `lir_var` 对非顶层的零捕获函数也必须改走静态名字，否则引用解析不到），
> 因此这两处**不可分开落地**。

> **LIR 没有"闭包转换层"（`clos`）的语法 —— 这是有意的。** 闭包的痕迹散在 LIR 的
> 5 个位置：`proc` 头的 `ncap` / `[fvs]`、`closure` 指令、`call` 的 `caps` 字段、
> 以及零捕获的 `lir_clos_atom`（§6）。**不另设一层** —— 这正是 Chez 有 L6 而 yac
> 没有的地方（§12）。将来若加显式闭包层，它的语法（`closbind` / `cl`）定义在 §6，
> **不进本文法** —— 那是 ANF 与 LIR 之间的**另一层** IR，不是 LIR。

**为什么 `op` 只列名字，不给每个 `op` 一条产生式。** 名字清单在上面（校验规则 2 用它）；
**每个 `op` 有几个操作数、各是什么，在 §4 的表里** —— 因为 LIR 的指令是**异构**的：

```
["local", nslots, nparams, gc]      3 个操作数，第 3 个是枚举
["gset",  name, off, src]           3 个操作数，但第 1 个不是槽
["call",  dst, callee, args, caps]  4 个操作数，第 4 个是列表
["tcall", callee, args, caps]       3 个操作数（无 dst）
```

若给每条指令写一条产生式，会得到 ~64 条形状各异的规则，**信息量与 §4 的表完全相同**，
而且**同一段文法里 `slot` 与枚举常量还会重载**（见上）。旧版 `DESIGN.md` 那份"LIR 语法"
正是这样漂移的 —— 列了 `nop` / `save` / `restore` / `neg` / `lnot` 这些 `emit` 里
**根本没有分支**的，又漏了实际在用的（见 §0）。所以本文的分工是：

| 部分 | 定义方式 | 谁校验 |
|---|---|---|
| **结构**（program / proc / insn / operand） | 上面的文法 | 解析层 |
| **名字**（有哪些 `op`） | 上面的 `op` 清单 —— **唯一清单** | `--verify-lir` 规则 2 |
| **形态**（每个 `op` 的操作数个数与含义） | §4 的表 —— **唯一清单** | `--verify-lir` 规则 3 |
| **语义**（`op` 做什么） | §4 的"说明"列 + §7.3 的指令选择 | — |

> **两张清单必须同步。** 增删 §4 的指令时，上面 `op` 那行要一起改 ——
> 校验规则 2（名字在清单里）与规则 3（arity 与 §4 一致）就是用来抓漏的。

**识别原生过程**：`nth(f,0) == "$proc"`（`emit.fun_is_raw`）。**只此一条判据** ——
现状还额外认"首条指令是 `$local`"，那是两个判据说同一件事（§5 合并 `$local`）。

**指令序列必须以帧指令开头**：`["local", nslots, nparams, gc]`。`local` 是读 argc/argv、
建帧、写 GC `stack_hi` 的地方，**必须排在所有发布（`gset`）与分配之前**。

## 3. 设计准则

### 3.1 KISS

| # | 准则 | 含义 |
|---|---|---|
| K1 | **一条指令做一件事，形状固定** | 操作数个数与位置不随用法变化；`["tcall", …]` 无 `dst` 就是因为它不是取值 |
| K2 | **无幽灵条目** | 每条指令必须有生产者与消费者。"文档里有、`emit` 里没有" = **幽灵**；"`emit` 里有、无人生产" = **死 handler** —— 两份清单都在 §9.5 |
| K3 | **未知即报错** | 未知指令 / 未知 bind / 未知 atom 一律编译期报错，不得静默跳过（§9.1） |

### 3.2 OCP

> **变化维度做成字段，不做成 opcode。**
> 新增一种"表示" —— 新宽度、新偏移形式、新操作数形态、新目标解析方式、
> 新 caps 布局 —— 应当只增加一个**字段取值**，**不新增 opcode，也不复制一份 emit handler**。

**判据**：两条指令若**除某个维度外逐字相同**，它们就是一条指令。下面六类维度已经被
核实泄漏成了 opcode（证据为 `emit_x86_64.yac` 行号）：

| 维度 | 取值 | 泄漏成的 opcode | 证据 |
|---|---|---|---|
| **目标解析** | 本镜像 / 跨镜像 / 运行期值 | `fcall` · `xcall` · `icall` | 三份"装 callee 地址再间接调"逐字重复（§7.6） |
| **caps 布局** | static n / dyn / **raw** | `apply` · `icall` · `$icall` | `apply_ncap` 用 `-1` 当哨兵；`$icall` 是"槽里直接是入口"这第三种取值 |
| **操作数形态** | 槽 / 立即数 | `mov`·`mov_imm`、`$add`·`$addi`、`alloc`·`alloc_s` | `alloc:1265` 用 `mov_rdi_imm`，`alloc_s:1286` 用 `mov_rdi_rax` —— 其余逐字相同 |
| **访问宽度 + 偏移形式** | 64/8 × 静态 disp32/动态槽 | `mref` · `mref8` · `ld64` | 三个维度全可字段化（§4.6） |
| **结果是否重打 tag** | 是 / 否 | `glob` · `$glob` | `glob:1347` 与 `$glob:1065` **逐字相同，只差一个 `shl rax,1`** |
| **存储转换** | 原样 / 去 tag / 立即数+零高半 | `mset` · `obj_st_int` · `obj_sti` | `1272` / `1278` / `1246` 三份同形 |

**一处"真重复"暂不消除**：`mref`（`:1239`）与 `$ld64`（`:1036`）、`mset`（`:1246`）与
`$st64`（`:1026`）**逐字相同** —— 即 `$ld64` / `$st64` 就是"去 tag 的 `mref` / `mset`"。
**本轮决定两份都留**：删掉一条、或删掉 `and_rax_1` 让它们分化，**都无法验证**
（二代自举 `yc_l1b` 坏掉，原因未定）。详见 §4.7 与 §9.6。

### 3.3 明确**不**合并的（避免"为去重而去重"）

去重不能盲做。下列几组**核实后确认语义/形状不同**，保持独立：

| 组 | 为什么不合并 |
|---|---|
| `cmp` vs `icmp` | **语义不同**：`cmp` 是多态比较（`emit_x86_i_cmp_eq`：先整数快路径，再落运行时）；`icmp` 是机器整数比较（`sar` 去 tag 后 `cmp`）。不是表示差异 |
| `memcpy` vs `$memcpy` | **签名不同**：`memcpy dstObj, off, src, n`（去 tag + 加偏移）；`$memcpy dst, src, n`（裸指针）。是两条不同指令 |
| `str_len` vs `bytes_len` | **实现不同**：`str_len` 有 nil / 整数 / 字符串三条内联快路径；`bytes_len` 是一次字段读。不是宽度差异 |
| `cmpjmp` vs `$jcc` | **形状不同**：`cmpjmp cond, then, else` 测**单槽**真值；`$jcc op, a, b, then, else` 比较**两槽** |
| `tcall` 独立 | 形状不同（无 `dst`，是控制转移不是取值）。硬并进 `call` 只会让 `dst` 变成"有时无意义" |
| `gst` vs `glob` | 不是一对：`gst i, src` 写 globals 固定槽，`glob dst, which` 读并可能 retag |
| `mref` vs `$ld64` | **明知逐字相同，本轮仍保留两份** —— 去 tag 行为一致（都被 `and_rax_1`）。不合并的理由是"删改无法验证"（§4.7 / §9.6），不是语义有差 |

## 4. 指令集（目标，权威）

**前缀契约**：`$` 前缀 = **raw 家族**，只允许出现在 `$proc` 内；操作数可以是未打 tag 的
裸值，不做 GC 安全点、不做 ABI 适配。**`$` 是一个字段（"本条不做 tagged 语义"），
不是第二个平行指令集** —— 所以凡"tagged 版 / raw 版逐字重复"的对，只留一条 + 一个字段。

**形态记法**：表中「形态」列的 `槽` = 整数槽号，`["imm", n]` = 立即数，
`["name", nm]` / `["fn", nm]` = 名字引用，`["static", n]` / `["dyn"]` = caps。
**操作数的完整文法见 §2**；本节各表只负责**给出每个 `op` 的操作数个数与含义**
（`--verify-lir` 规则 3 据此校验 arity）。

### 4.1 帧与栈

| 指令 | 形态 | 说明 |
|---|---|---|
| `local` | `["local", nslots, nparams, gc]` | `gc ∈ {scan, raw}`（合并 `$local`）。建帧 + 登记 GC 扫描范围；`raw` 即 `smap_set_nslots(0)` |
| `$sp` | `["$sp", dst]` | `dst = rsp` |
| `$fp` | `["$fp", dst]` | `dst = rbp` |
| `$carg` | `["$carg", argreg, src]` | 把槽 `src` 装进参数寄存器 `argreg`（**首操作数是寄存器号**） |
| `$smap` | `["$smap", dst]` | 栈图地址（GC 用） |

### 4.2 搬运与算术

| 指令 | 形态 | 说明 |
|---|---|---|
| `mov` | `["mov", dst, src]` | `src` 可为立即数（**合并 `mov_imm`**：立即数是已编码的 64 位模式） |
| `add` `sub` `mul` `div` `rem` | `["add", dst, a, b]` | `a` / `b` 可为立即数（**合并 `$addi` 的立即数身份**） |
| `land` `lor` `xor` | `["land", dst, a, b]` | 同上（`$or dst,src` ≡ `["lor", dst, src, ["imm", 1]]`） |
| `shl` `shr` | `["shl", dst, a, b]` | 同上 |
| `bnot` | `["bnot", dst, src]` | |
| `$clamp0` | `["$clamp0", dst, src]` | 负数夹到 0 |
| `$bt` | `["$bt", dst, base, bit]` | 位测试 |
| `$bts` | `["$bts", base, bit]` | 位测试并置位 |

> **算术保留独立 opcode**：`add` 与 `sub` 是**不同的运算**，不是同一运算的不同表示
> —— K1 的"一件事"就是"一次加法"。被合并的只有**操作数形态**这一维。

### 4.3 比较与分支

| 指令 | 形态 | 说明 |
|---|---|---|
| `cmp` | `["cmp", op, dst, a, b]` | **多态**比较，`op ∈ {==,!=,<,<=,>,>=}`；走整数快路径 + 运行时兜底 |
| `icmp` | `["icmp", op, dst, a, b]` | **机器整数**比较（去 tag 后）；`$icmp` 并入（差一个 operand tagging 字段，§5） |
| `label` | `["label", L]` | |
| `jmp` | `["jmp", L]` | |
| `cmpjmp` | `["cmpjmp", cond, then, else]` | `cond` 槽非 0 → `then` |
| `$jcc` | `["$jcc", op, a, b, then, else]` | `op ∈ {>=u, ==, <, else}`；两槽比较后跳转 |

### 4.4 调用与过程序言

**3 条 opcode**：

| 指令 | 形态 | 角色 |
|---|---|---|
| `call` | `["call", dst, callee, args]` | **求值型**：调用，结果写 `dst` |
| `tcall` | `["tcall", callee, args]` | **转移型**：**无 `dst`**，body 结尾（与 `ret` 同类） |
| `ccall` | `["ccall", dst, callee, args]` | **C ABI 家族**：独立寄存器约定、marshal、结果打 tag |

**捕获不是指令 ✗ —— 是一种操作数** ✓：`["cap", i]`（第 i 个捕获，即 `[self + 32 + i*8]` ✓），
可以出现在**任何**读值的位置（`fcall` 的实参 ✓、`cmp` 的操作数 ✓、`["closure", …, capSlots]` ✓）。

```
callee ::= ["name", nm]    ; 静态名字（fn_entry 负责变成地址，§7.6）
         | ["slot", s]     ; 运行期值（栈槽）
                           ; 两者都表示【过程对象】本身
args   ::= [operand*]      ; 真正的实参 —— 【不含捕获】
```

**对象即环境入口；捕获住在对象里，从不进槽。** 被调者（`callee`）就是过程对象：flat 时是
静态 cell、否则是堆闭包（§4.5）。调用点只传 **对象 + 真参** —— `caps` 字段**已删除**，
也**不需要任何序言载入指令**（`bindcaps` 已取消）：对捕获的**每一次引用**直接从对象读 ✓。

#### 4.4.1 槽布局与 Γ

```
["proc", name, nparams, ncap,
 [ ["local", nslots, nparams],  ; ① 建帧 —— 帧里【没有】捕获槽
   ["label", "$tco"],           ; ② 尾调用入口
   …body… ,
   ["ret", retslot] ],
 srcname]

slot 1 .. nparams   ← 形参        bind_params: ps[j] ↦ j+1
```

- **Γ 对捕获的映射从"槽"改为"操作数"**：`fvs[j] ↦ ["cap", j]` ✓ —— `lir_var` **不发指令** ✓，
  直接把这个操作数交给使用它的那条指令 ✓
- **自递归不需要自闭包**：`self` 就是**传入的对象** ✓ ⇒ `need_self` / `self_slot` /
  序言里的 `["closure", self_slot, …]` **全部退役** ✓

#### 4.4.2 `["cap", i]` 的具体内容

```
对象 = self（第 0 个实参寄存器，即 callee 位置）

["cap", 0]  ≡  [对象 + 32]          ; fvs[0]
["cap", 1]  ≡  [对象 + 40]          ; fvs[1]
["cap", i]  ≡  [对象 + 32 + i*8]    ; fvs[i]
```

- 对象布局：`+24` = `nenv`，`+32 + i*8` = 第 i 个自由变量（§4.5）
- **`i` 与 `free_vars` 的 `fvs` 下标一一对应**，与定义点 `["closure", …, capSlots]`
  的填充顺序一致 ✓
- **它不是指令** ✗ —— 是操作数 ✓：emit 在**装载该操作数的那条指令**里完成这一次
  访存 ✓（统一入口 `emit_load_operand` ✓：slot / cap / imm 三种 ✓）

**性能（vs `cval` 拆两条 ✗）** —— 以 `inner(x) = f + x` 为例：

```
cval 两条：  6 条机器指令 / 3 次访存   ✗（读对象 → 写临时槽 → 再读槽，两次纯往返 ✗）
capRef 一条：4 条机器指令 / 1 次访存   ✓（捕获直接进实参寄存器 ✓）
旧 caps 槽： 5 条机器指令 / 4 次访存   ✗（调用点每次还要搬 2n ✗）
```

**⇒ `capRef` 指令最少、访存最少 ✓；捕获用 k 次就是 k 次访存 ✓（`cval` 是 3k ✗）✓。**
帧里没有捕获槽 ✓（帧更小 ✓）、LIR 更短 ✓。

#### 4.4.3 完整例子：同一个 proc 的三处形态

源：`let f() = 1` / `let outer(n) = let f = 7 in let inner(x) = f + x in inner(n)` / `outer(10)`
（`inner` 的 `ncap = 1`，`fvs = [f]`）

```
① 定义点（outer 体内）    [mov_imm, 2, 14]                 ; 槽2 = 7
                          [closure, 3, inner, [2]]          ; 对象 ← 外层槽2   （不动 ✓）

② 调用点   现在：          [apply, 4, 3, 1, [1]]             ; 第3操作数 1 = ncap ⇒ 由调用方搬 ✗
           改后：          [call, 4, ["slot", 3], [1]]      ; 只传对象 + 真参 ✓

③ callee   现在：          [local, 6, 2]                    ; ntot=2：槽1=f(捕获) 槽2=x ✗
           改后：          [local, 4, 1]                    ; 只有形参 x（槽1）✓
                          …
                          [fcall, 3, yac_num_add, [["cap", 0], 1]]  ; f + x —— 一条 ✓
```

**⇒ 数据流：捕获值只搬一次（定义点进对象 ✓），之后每次引用直接从对象读 ✓。**

#### 4.4.4 三档归宿与硬约束

| 原 `caps` | 归宿 | 说明 |
|---|---|---|
| `["static", n]` | **callee 的 `proc` 头**（`ncap`） | 调用点不再需要知道 n |
| `["dyn"]` | **无需** | 调用点不需要捕获个数 |
| `["raw"]` | **被调符号的导出属性**（C ABI 导出） | 不再是调用点字段 |

**入参位置必须与 `ncap` 无关**：**对象固定占第 0 个实参寄存器**，真参依次其后 ✓。
**硬约束**：无对象可传的 C ABI 导出（`--shared` / `--shared-int`）**必须 `ncap == 0`**；
**编译期强制检查，违反即报错** ✓。

#### 4.4.5 `--verify-lir` 不变量

1. `["cap", i]` 的 `i` 必须 `< proc` 头的 `ncap`
2. `proc` 头 `ncap` 必须等于 `len([fvs])`（§5.5 S2 落地后可机器校验 ✓）
3. proc 内**不得**引用"捕获槽" —— 槽 `1..nparams` 全是形参 ✓

#### 4.4.6 统一调用形态（Chez 式：表示分档，调用只有一种）

**机器级约定** —— 所有调用形态共享，无一例外：

```
self  ← 对象                 ; flat = 静态 cell（cell|1）；其余 = 堆闭包 —— 两者同形（不变量 5）
真参  ← 其后的实参寄存器
入口  = [对象 + 16]           ; 与 cell 布局一致（§4.5）
```

**四种情形的机器码** —— 同一个约定，没有任何一种需要 `caps`：

| 情形 | 调用点 | 机器码 |
|---|---|---|
| 直接调用、`ncap == 0` | `["name", nm]` | `mov rdi,<cell>` ; `call <label>`（静态已知，可省一次 `[+16]` load）|
| 直接调用、`ncap > 0` | `["name", nm]` | `mov rdi,<cell>` ; `call <label>` |
| 间接调用 | `["slot", s]` | `mov rax,[s]` ; `mov rdi,rax` ; `call [rax+16]` |
| 尾调用 | `["tcall", …]` | 同上，`jmp` 代替 `call` |

⇒ **`apply` / `icall` / `tailapply` / `$icall` 全部删除** —— 它们是 `caps` 维度的产物 ✓。
对象与裸入口**同形**（不变量 5 ✓）⇒ 间接调用**一律按对象**处理 ✓，调用点无需区分 ✗。

**Chez 对照**：

| Chez | 本设计 |
|---|---|
| `(call f a b)` 单一形态 | `["call", dst, callee, args]` |
| 过程对象 = 第 1 实参（self） | `self ← 对象` |
| 入口代码从对象 load 自由变量 | `["cap", i]`（捕获不进槽 ✓） |
| `constant`（`free* = ∅`）= 带 tag 代码指针 | flat cell `cell\|1`，`nenv = 0` 同形 |
| well-known 直接调用 | `["name", nm]` → 静态入口 |

> ⚠️ **下面这一段描述的是已删除的 `caps` 字段（历史，保留作背景）。** 现行 ABI 见
> §4.4：调用点只传**对象**作首实参，捕获不住槽，引用经 `["cap", i]` 从对象读。

调用一个闭包时，调用方必须知道**要传几个前导捕获、值从哪来** —— 这就是 `caps`：

| 取值 | 什么时候 | 调用方怎么做 | 代价 |
|---|---|---|---|
| `["static", n]` | callee 编译期可知（`ncap` 就在 `proc` 头里） | 直接把这 n 个值塞进前 n 个寄存器 | 最低；`n = 0` 就是 flat |
| `["dyn"]` | callee 是运行期值（`map(xs, f)` 里的 `f`） | 读对象 `[+24]` 得 nenv，逐个取 `[+32+i*8]` | emit **现场生成跳转表** |
| `["raw"]` | 槽里直接是代码入口（`$proc` 内） | 什么都不传，直接 `call` | — |

两端是对称的：`closure` 指令的 `capSlots` 是**外层帧**的槽号（建闭包时把值**搬进**对象），
`call` 的 `caps` 是**调用时把值搬回**前 `ncap` 个槽 —— 中间靠对象的 `nenv`（`[+24]`）对齐。

> **为什么 `["static", n]` 和 `["dyn"]` 都得有。** 前者是优化（不读 `[+24]`、不生成跳转表），
> 后者是一等函数的通例。现状把这一维写成了两条 opcode（`apply` = 静态 ncap / `icall` =
> 动态），还用 `-1` 当哨兵 —— 那正是"该做成字段"的信号（§3.2）。
>
> **不变量 5 让两条路能统一**：静态 cell 的 `nenv = 0` 与堆闭包同形，所以对 flat 函数
> 走 `["dyn"]` 也只会解出 0 个 caps，结果与 `["static", 0]` **相同** —— 这就是"闭包值
> 表示统一"的价值（§1 不变量 5）。

**三条各自为什么不能再并：**

| 合并尝试 | 为什么不 |
|---|---|
| `tcall` → `call` + `tail` 字段 | `tcall` **没有 `dst`**（结果由被调者经 `rax` 直接返还**调用者**，本帧已拆）。做成字段会让 `dst` "有时无意义"，违反 K1 的"形状固定" |
| `ccall` → `call` + `abi` 字段 | ABI 家族是**真差异**：C 调用要 marshal 参数（去 tag / 取值指针）、走 PLT 或寄存器间接、结果要 `shl rax,1`。并进去会让 `caps` / `callee` 对 C 调用全部无意义 |
| `callee` 拆成两条 opcode | 那正是现状 `fcall` / `icall` 的病：**同一个维度**（callee 怎么解析）被写成了 opcode |

**`callee` 里的两态就是全部**：`["name", nm]` 的解析（本镜像 label / 全局槽 /
跨镜像补丁）是 emit 策略点 `fn_entry` 的事（§7.6），**LIR 不认识镜像边界**（不变量 3）。
`ccall` 用同一个 `callee` —— 静态符号走 `call rel32`，槽里的指针走寄存器间接
（原 `iccall` 的形态）。

> **尾位置**：ANF 已改为 `tail` 结构化（`ANF.md` §4.1），因此 LIR **只发** `tcall`
> —— `lir.yac` 的 `maybe_tcall` 事后改写**应删除**（§5.5 S1）。
>
> **`caps = ["raw"]` 是原 `$icall` 的落点。** 它把"`$` 前缀"这条约束**平移到字段取值**上：
> `["raw"]` 只允许出现在 `$proc` 内（校验规则 6），从而不必为它保留一条独立 opcode。

### 4.5 名字单元（静态化）

每个顶层名字一个 32B 静态单元，放在 globals 数据区（**不在 GC 堆**）：

```
cell + 0   : value        ← 顶层 let 的值      (gval 读这里)
cell + 8   : mark
cell + 16  : entry        ← 顶层 letfun 的入口  (gvar 用；icall 读 [+16])
cell + 24  : nenv = 0     ← 常量 0（使该单元本身即合法的零捕获闭包）
cell + 32… : env…
```

| 指令 | 形态 | 说明 |
|---|---|---|
| `gvar` | `["gvar", dst, name]` | `dst = cell \| 1`（tagged）—— 函数当值用 |
| `gval` | `["gval", dst, name]` | `dst = [cell + 0]` —— 读顶层值 |
| `gset` | `["gset", name, off, src]` | `[cell + off] = src`；`off ∈ {0, 16}`（**合并 `gvst` + `gfnst`**）；`src = 槽 \| ["fn", nm]` |
| `glob` | `["glob", dst, which, tag]` | 读 globals 固定槽，`which ∈ {1,2,3,4}`；`tag` 控制是否重打 tag（**合并 `$glob`** —— 二者 handler 逐字相同，只差一次 `shl rax,1`） |
| `gst` | `["gst", i, src]` | 写 globals 固定槽 |
| `$gbase` | `["$gbase", dst]` | globals 基址**地址本身**（不 load，patch tag 3）。**不并入 `glob`**：差别是 patch tag，不是功能 |

### 4.6 对象与内存

| 指令 | 形态 | 说明 |
|---|---|---|
| `closure` | `["closure", dst, fnName, [capSlots]]` | 堆闭包 `[next][mark][fnptr][nenv][env…]`；`capSlots` 是**外层帧**的槽号 |
| `alloc` | `["alloc", dst, size]` | GC 堆分配；`size` 可为立即数或槽（**合并 `alloc_s`**） |
| `obj_kind` | `["obj_kind", dst, obj]` | 取对象种类 |
| `mref` | `["mref", dst, obj, off, w]` | 读字段。`obj` **先去 tag**；`off` = 立即数 disp32 或槽（动态，值 = 字节偏移×2）；`w ∈ {8,64}`，`w=8` 时结果重打 tag。**合并 `mref8` / `ld64`** |
| `mset` | `["mset", obj, off, src, w, conv]` | 写字段。`obj` 先去 tag；`conv ∈ {slot, untag, imm32}`（src 原样 / 去 tag / 32 位立即数+零高半）。**合并 `mset8` / `st64` / `obj_sti` / `obj_st_int`** |
| `tag` | `["tag", dst, src]` | 打 tag |
| `untag` | `["untag", dst, src]` | 去 tag |
| `is_int` | `["is_int", dst, v]` | 整数判定 |

> **合并依据**：`mref` / `mref8` / `ld64` 三条的差异只有两个维度 —— 偏移形式
> （`mref:1243` 静态 `disp32` / `ld64:1326` 先 `sar` 再索引）与宽度
> （`mref8:1303` `movzx` / `ld64:1328` 64 位）；「结果是否重打 tag」由宽度决定
> （`mref8:1304` 有 `shl rax,1`，两条 `w=64` 的都没有）。`mset` 侧同理覆盖
> `mset8` / `st64` / `obj_sti` / `obj_st_int`（多一个"源转换"维度）。
>
> `$ld64` / `$st64` / `$ld8` / `$st8` **不在此列** —— 它们归 §4.7 的 raw 家族
> （且 `$ld64` / `$st64` 与 `mref` / `mset` 逐字相同，见 §4.7 的不对称说明）。

### 4.7 原生内存（`$` 家族）

| 指令 | 形态 | 说明 |
|---|---|---|
| `$ld64` | `["$ld64", dst, base, off]` | 读 64 位；**base 先去 tag**（`and rax,1`） |
| `$st64` | `["$st64", base, off, src]` | 写 64 位；**base 先去 tag** |
| `$ld8` | `["$ld8", dst, base, off]` | 读 8 位（`movzx`，结果不打 tag）；**base 不去 tag** |
| `$st8` | `["$st8", base, off, imm8]` | 写 8 位**立即数**；**base 不去 tag** |
| `$memcpy` | `["$memcpy", dst, src, n]` | 裸指针拷贝 |
| `$memset` | `["$memset", dst, n]` | |
| `$syscall` | `["$syscall", nr, name, [srcSlots]]` | Linux syscall（`nr=60` 退出，arm64/riscv64 映成 93）；`name` 供 Windows 侧解析 |

**它们做什么**：`rt/runtime.yac` 手写 LIR 写内核时的**裸内存读写** —— GC 标记/回收、
分配器、闭包应用（读 `[clos+16]` 入口 / `[clos+24]` nenv）、globals 注册表、
文件 / argv、JIT 槽。实测共 **132 处**（`$ld64` 95 + `$st64` 37），散在 18 个
`rt_*_ins` 里；`$ld8` / `$st8` 各只有 2~3 处。

> ⚠️ **已知不对称，本轮决定"保留"不修。** `$ld64` / `$st64` 对 base 做
> `and_rax_1`（去 tag），`$ld8` / `$st8` **不做**。副作用是 `$ld64` ≡ `mref`、
> `$st64` ≡ `mset` **逐字相同**（§3.2）—— 即 `$ld64` / `$st64` 实际上就是
> "去 tag 的 `mref` / `mset`"。
>
> **为什么保留**：runtime 本来就是"用之前自己显式去 tag"（`rt_gc_mark_ins` 里
> `["$and", 2, 1, 0 - 2]`，`rt_apply1_ins` 里 `["$and", 3, 1, 0 - 2]`），所以那两处
> `and_rax_1` 在全部调用点上**都是 no-op** —— 删掉在语义上应当无害，**实测也确实跑通了
> 自编译**（`yc_l1` 成功编出 `yc_l1b`）。但**二代自举坏掉**（`yc_l1b`，以及恢复
> `and_rax_1` 后重建的 `yc_l1c`，两者都是 `error: bad arguments`）→ 这次删改
> **无法验证**。**注意：恢复 `and_rax_1` 之后二代仍然坏，所以它不是元凶** —— 那是
> 先前就存在的阻塞。能验证之前，保留原样。
>
> **要推翻这个决定，先做这件事**：`yc_l1` → `yc_l1b` → `yc_l1c` 走满 `Makefile` 的
> **两遍**流程（那里注释："One pass leaves `[00:00:00]`"），确认二代/三代是否正常。
> 若两遍后一切正常，说明 `yc_l1b` 的毛病是 1-pass 的**既有问题**，与 `and_rax_1`
> 无关 —— 那时再删才有依据。

### 4.8 浮点

| 指令 | 形态 |
|---|---|
| `$f64fromstr` | `["$f64fromstr", dst, src]` |
| `$f64binop` | `["$f64binop", dst, a, b, op]`，`op ∈ {0:add, 1:sub, 2:mul, 3:div}` |
| `$f64rel` | `["$f64rel", dst, a, b, op]` |
| `$f64print` | `["$f64print", dst, src]` |

### 4.9 控制与运行时

| 指令 | 形态 | 说明 |
|---|---|---|
| `ret` | `["ret", slot]` | entry 过程为裸 `ret`；普通过程补 `leave` |
| `exit` | `["exit", slot]` | `untag` 后走 OS 退出 |
| `throwk` | `["throwk", k, v]` | 抛给续延 |
| `mkcont` | `["mkcont", dst, lab]` | 造一等续延：分配 48B 对象，存 `rbp`（`+24`）/ `rsp`（`+32`）/ 返回地址（`+40`，指向 `lab`）；结果 tagged 写入 `dst` |
| `cc_recv` | `["cc_recv", dst]` | 续延被调用后的续接点：把 `rax` 存入 `dst` |
| `syscall` | `["syscall", s, nr, [args]]` | 带 tag 语义的退出路径；`untag` 语境 |
| `strlit` | `["strlit", dst, bytes]` | rodata 字面量，结果 tagged 指针 |
| `str_len` | `["str_len", dst, src]` | 内联（nil / 整数 / 字符串三路快路径） |
| `str_ref` | `["str_ref", dst, src, i]` | 内联 |
| `bytes_len` | `["bytes_len", dst, src]` | 内联（单次字段读） |
| `write1` | `["write1", src]` | 把槽 `src` 的整数（去 tag）作为 1 字节写到 fd 1 |
| `clock` | `["clock", dst]` | |

### 4.10 内联原语族 → **决策：全删，只留 `call yac_*`**

以下 **27 条**在 emit 里**有分支**，但**全仓没有任何生产者**；实际走的是
`lir_rt_*` → `["fcall", dst, "yac_xxx", …]`。

| 族 | 指令（全部无生产者） |
|---|---|
| 列表 | `nil` `cons` `len` `nth` `tail` `append` `drop` `list_new` `list_push` `list_rev` |
| 高阶 | **`map` `foldl`** —— 旧文档**漏列** |
| 字符串 | `str_cat` `str_slice` `int_to_str` `bytes_to_str` |
| bytes | `bytes_new` `bytes_ref` `bytes_put` `bytes_append` `bytes_extend` |
| 文件/系统 | `read_file` `write_file` `time_ms` `time_str` `argc` `argv` |

> **`print` 不在这一族** —— 它在 emit 里**连分支都没有**，是**幽灵**（§9.5）。

#### 为什么是"全删"，而不是"部分内联"

**旧文档把它们说成"内联实现"，那是错的。** 实测它们的 handler 做的是
「装参数 → `call_user` → patch 目标到 `yac_xxx`」—— **仍然是在调用 runtime**，
只是换了个拼法：

| 写法 | 参数放哪 |
|---|---|
| `["nth", dst, a, b]` | 装进 **rdi / rsi**（SysV 寄存器约定） |
| `["fcall", dst, "yac_nth", [a, b]]` | **槽**约定 |

**而这两套约定对不上。** runtime 侧的 `yac_*` 是**客过程** —— `rt_nth_ins` 开头就是
`["local", 22, 2]`，参数**从槽 `1` / `2` 读**。所以这 27 条不只是"没人用"，而是
**陈旧**：它们一致地错在一个**已经改掉的调用约定**上。

之所以从未暴露，正因为没有生产者。**这是个陷阱** —— 谁今天给它们加个生产者，就会
生成参数传错的代码，而且**连报错都不会有**：分支存在（不是 §9.1 那个 `else` 兜底），
只是语义错。

| | 内联指令（删） | `call yac_*`（留） |
|---|---|---|
| 性质 | **陈旧转发**，约定已失效 | 唯一合法机制 |
| 性能 | **并不快** —— 它自己也是 `call` | 每次搬参数 + `call` / `ret` |
| emit 复杂度 | 高（27 × 3 份，每架构各写一遍） | 低（一份，在 `runtime.yac`） |

**结论（第 1 步）**：27 条**全部删除**，三个后端都删（第 4 步执行）。

真正的"内联"（`cons` / `nth` 这类热路径，省掉 `call` / `ret`）是**第 8 步的性能项**，
必须以「**新增生产者 + 真正的内联实现**」的形式单独做 —— **不能**拿这 27 条当起点，
它们没有可复用的价值。

> Chez 的 `np-expand-primitives` 确实也只内联一部分 —— 但那是「**从 `call` 优化成内联**」；
> yac 这 27 条是「**从 `call` 换了个拼法，还换错了约定**」，两件事。

## 5. 现状 → 目标（迁移表）

**这是全文**唯一的现状清单**，原 §3 各表里的"生产 / 待收敛"列已收拢到这里。**

生产列：**A** = `front/lir.yac`，**R** = `rt/runtime.yac`，**—** = 无生产者。

### 5.1 调用族：10 条 → 3 条

| 现状 | 生产 | 目标 |
|---|---|---|
| `fcall` | A R | `call(["name",nm], caps=["static",0])` |
| `xcall` | A | `call(["name",nm])`（解析交给 `fn_entry`） |
| `icall` | A R | `call(["slot",s], caps=["dyn"])` |
| `apply` | A | `call(["slot",s], caps=["static",n])` |
| `tcall` | A R | `tcall`（不变） |
| `ticall` | A | `tcall(["slot",s], caps=["dyn"])` |
| `tailapply` | A | `tcall(["slot",s], caps=["static",n])` |
| `ccall` | A R | `ccall`（**唯一不改名**，原地保留 handler；补上 `callee` 字段） |
| `iccall` | A | `ccall(callee=["slot",s])` —— 与 `ccall` 的差异**只在 callee 从哪来**，其余（marshal / unalign / `shl rax,1`）逐字相同 |
| `$icall` | R | `call(callee=["slot",s], caps=["raw"])` —— 槽里直接是入口，不解包 |
| ~~`gcall`~~ | — | **删**（rev1 残留） |

### 5.2 名字单元：4 条（2 活跃 + 2 幽灵）→ 3 条

| 现状 | 生产 | 目标 |
|---|---|---|
| `gvar` | A | `gvar`（不变） |
| `gvld` | — | `gval`（rev1 残留，**改名复用**：`movabs cell; mov rax,[rax]` 已是对的） |
| `gvst` | — | `gset name, 0, src`（同上） |
| `gfnst` | A | `gset name, 16, src` |
| `glob` / `$glob` | R / R | `glob dst, which, tag` |
| `$gbase` | R | 保留（与 `glob` 的差别是 patch tag 3，非功能重复） |
| `gst` | R | 保留 |

### 5.3 内存与对象

| 现状 | 生产 | 目标 |
|---|---|---|
| `mref8` | R | `mref` + `off=slot, w=8` |
| `ld64` | R | `mref` + `off=slot` |
| `mset8` | R | `mset` + `off=slot, w=8` |
| `st64` | R | `mset` + `off=slot` |
| `obj_sti` | R | `mset` + `conv=imm32` |
| `obj_st_int` | R | `mset` + `conv=untag` |
| `alloc` / `alloc_s` | R / R | `alloc` + `size` 形态 |
| `$ld64` / `$st64` / `$ld8` / `$st8` | R | **保留** —— 本轮决定不做"并成 2 条 raws"这个收敛（§4.7 / §9.6） |

### 5.4 搬运与算术

| 现状 | 生产 | 目标 |
|---|---|---|
| `mov_imm` | A R | `mov` + 立即数操作数 |
| `$addi` `$add` `$sub` `$or` `$shr` `$and` | R | `add` / `sub` / `lor` / `shr` / `land` + 立即数操作数 |
| `icmp` / `$icmp` | R / R | `icmp` + operand tagging 字段 |
| `local` / `$local` | A R / R | `local` + `gc` 字段（原生过程判据收敛为 `$proc` 一条） |
| ~~`sal`~~ ~~`sar`~~ ~~`$lea`~~ | — | **删**（emit 里根本没有 handler —— 幽灵） |

### 5.5 结构性合并

| # | 现状 | 目标 | 依据 |
|---|---|---|---|
| S1 | `maybe_tcall` 事后改写 `fcall`→`tcall` | **删** —— ANF 的 `tail` 结构已给出尾位置 | §4.4 |
| S2 | `proc` 头的 `ncap` 是"自由变量数"，`fvs` 本身被丢弃 | `proc` / `$proc` **末尾追加** `[fvs]`（形状见 §2） | 落地后 `ncap == len(fvs)` 可机器校验（§8 规则 8） |
| S3 | `topfn_has` 全局 box 进 LIR | 静态名集合作为**参数**传入 | 不变量 4 |
| S4 | 三处静默兜底 | 改成编译期报错 | K3 |

**S2 的影响面**（追加字段的代价）：`fvs` 已经在 `lir.yac:1145` 算好、并随 begin 的
元组返回（`nth(p, 6)`），所以**编译器侧只需补一个字段**；但 `proc` / `$proc` 的
**全部**构造点都要补上第 7 项：

| 构造点 | 第 7 项 |
|---|---|
| `lir.yac:1236`（真客过程） | `nth(p, 6)` |
| `lir.yac:1146`（stub）、`:1363`（`_start`） | `[]` |
| `backend.yac:795`、`jit.yac:221`（`_eval`） | `[]` |
| `runtime.yac` 的各 `proc` / `$proc` | `[]` |

恒为 `[]` 的那些是**原生 / 合成**过程（无闭包、`ncap == 0`），没有自由变量。

**汇总**：

| 家族 | 现在 | 目标 |
|---|---|---|
| call | 10 条（+1 死代码） | **3 条** |
| 名字单元 | 4 条（2 活跃 + 2 幽灵） | **3 条** + `glob` / `$gbase` / `gst` |
| 对象内存 | 8 条 | **2 条**（`mref` / `mset`）+ 字段 |
| 原生内存 | `$ld64`/`$st64`/`$ld8`/`$st8` 4 条 | **保留 4 条**（§4.7：不对称保留） |
| 搬运算术 | `mov_imm` + 6 条 `$` 算术 | 字段化，**0 条新增** |

全部是**纯重构**：IR 形状变、语义不变，验收方式统一为**各阶段 golden 不变**。

## 6. 闭包在 LIR 里的落点（5 处）

闭包转换不是一个 pass，它的痕迹散在 LIR 的 5 个地方 —— 这是"没有 `clos` 层"的代价：

| # | 位置 | 形态 | 职责 |
|---|---|---|---|
| 1 | `proc` 头 | `ncap` | 把"捕获几个"烘焙成 ABI 字段 |
| 2 | 定义点 | `["closure", dst, gname, [capSlots]]` | 在**外层帧**分配，capSlots 由 `outer_caps` 查外层槽 |
| 3 | proc 体内（自引用） | `["closure", self_slot, gname, [1..ncap]]` | 自递归用：捕获**自己**的入参捕获槽 |
| 4 | 调用点 | `call` 的 `caps` 字段 | caps 前缀约定 |
| 5 | 名字当值 | `lir_clos_atom` → `["closure", dst, fnName, []]` | **0 捕获**闭包（见 §9.2） |

**四件事挤在 `lir_letfun_*` 里**：自由变量分析 / 闭包分配 / 调用约定 / 表示判定。
其中**表示判定只有一行**：`flat = ncap == 0` —— **与"是否顶层"无关**（`FLAT_ABI.md`
准则 2）。⚠️ **现状代码比这条窄**：`flat = is_top and ncap == 0`（`lir.yac:1307`），
即只有顶层函数才可能 flat —— 属待收敛的偏差，见 §2 末"flat 形态"。

**近期改动（轻量版）**：给 `proc` 头**末尾追加** `[fvs]` —— 自由变量表（`free_vars`
的输出），现在算出来被丢掉、只剩 `ncap` 这个数字。

**形状定义在 §2**（那里也写了"只能追加不能插入"的原因 —— `fun_is_raw` 读
`nth(f, 4)`）；**迁移与逐构造点的影响面在 §5.5 S2**。本节不重复这两处。

**远期（完整版）**：在 ANF 与 LIR 之间加显式 `clos` 层（Chez 的 L6）：

```
closbind ::= ["closures", [cl*, tail]]
cl       ::= [name, [fv*], [params], body]
```

届时 §6 的 5 处落点全部收敛到这一层。**触发条件**：做 well-known
（`singleton`/`borrowed`/`pair`/`vector`）时 —— 那要求闭包是可分析对象。

## 7. LIR → 机器码

**三段流水线**，每段是独立的模块层：

```
LIR insn*  ──① 指令选择 + 框架──▶  目标指令序列（字节）
           ──② patch 求解──────▶  全部立即数/相对位移就位
           ──③ 容器打包────────▶  ELF64 / PE / Mach-O 文件
```

| 段 | 模块 | 产物 |
|---|---|---|
| ① 指令选择 + 框架 | `back/emit/emit_<arch>.yac` + `back/encode/encode_<arch>.yac` | 字节 + 未决 patch 表 |
| ② patch 求解 | `back/emit/emit.yac` 的 `emit_resolve_loop` / `emit_resolve_patch` | 全部立即数/相对位移就位 |
| ③ 容器 | `back/pack/{elf,pe,macho}.yac` + `pack.yac` + `target.yac`；JIT 走 `pack/yjit.yac` | 可执行映像 / blob |

### 7.1 编码层的语法范式与布局

**没有"机器码的 yac list"** —— 机器码的形态是**文件**：

```
image     ::= ELF64 | PE | MachO
ELF64     ::= ehdr phdr* text globals*
text      ::= encoded*                     ; 按 --arch 选 encode_*
encoded   ::= x86-64 | arm64 | riscv64 字节序列
```

`LOAD_VADDR + TEXT_OFF` 起是 `text`，紧随其后是 `globals` 数据区；每个顶层名字一个
**32B cell**，索引 `i` 的地址 = `globals + 448 + 32*i`（cell 布局见 §4.5）。

### 7.2 patch 语言（延迟求解）

指令选择期不知道最终地址，于是记下 `[tag, …]`，由 ② 段求解：

| tag | 含义 |
|---|---|
| 1 / 12 | label 相对偏移（同过程内） / 字符串池偏移 |
| 2 | 代码地址绝对 64 位：`codebase + offs[fid]` |
| 3–8 | globals 基址 + 固定偏移（`$gbase` / `glob which=1..4` / map / 平台槽） |
| 11 / 14 | TCO `$tco` 回跳位置 / 栈图地址 |
| 15–20 | PE 平台符号偏移（`uname` / `dlsym` / `system` / `dlopen` / unimplemented stub） |
| 21 | host 槽：`id < 10` → `globals+136+8*id`；否则 → `globals+456+8*(id-10)`（extern） |
| 22 | **cell 地址**：`globals + goff`。用于 `gvar` / `gval` |
| 23 | **cell 入口 bake**：`[globals+goff+16] = codebase + offs[fid]` |

### 7.3 指令选择（形态约定）

完整映射是各 `emit_<arch>.yac` 的分派链，机械对应。关键几条：

| LIR | x86_64 |
|---|---|
| `mov dst, imm` | `movabs rax, imm` → store（立即数是**已编码**的 64 位模式，不再 `<<1`） |
| `add dst,a,b` | `mov rax,[a]` · `add rax,[b]` · store（临时值走 `rax`） |
| `cmp op,dst,a,b` | 整数快路径 + 运行时兜底（`emit_x86_i_cmp_eq`） |
| `icmp op,dst,a,b` | `sar` 去 tag · `cmp rax,rbx` · `setcc` · `movzx` · `shl rax,1` |
| `cmpjmp c,Lt,Lf` | `mov rax,[c]` · `test rax,rax` · `jnz Lt` · `jmp Lf` |
| `call`（`callee=["name",·]`） | 前 6 → 寄存器、其余入栈；`call rel32`；host/extern 经全局槽 `call r11` |
| `call`（`callee=["slot",·]`, `caps=["static",n]`） | 取闭包 → `and rax,1` → 读 `[+16]` 入口 → 展开 `n` 个前导 caps → `call` |
| `call`（`callee=["slot",·]`, `caps=["dyn"]`） | 同上，但按 `[+24]` 的 nenv **现场生成跳转表** |
| `call`（…, `caps=["raw"]`） | 直接 `call` 槽里的指针（不解包）—— 原 `$icall` |
| `tcall` | 三条路径见 §7.5 |
| `ccall` | C 协议 marshal · `call rel32`（静态符号）或寄存器间接（槽）· `shl rax,1` 打 tag |
| `mref` / `mset` | 见 §4.6 的三维度 |
| `closure` | `yac_alloc` → `[+16]` fnptr（patch）· `[+24]` nenv · `[+32+8i]` env |
| `gvar` | `movabs cell` · `or rax,1` · store（AOT 再 bake `[cell+16]`） |
| `local n,np,gc` | `push rbp` · `mov rbp,rsp` · `sub rsp,8*n` · 写 GC `stack_hi`（`gc=raw` 时跳过） |

### 7.4 约定

| 约定 | 内容 |
|---|---|
| 槽号 → 帧偏移 | `[rbp - 8*s]` 量级；由 emit 决定，LIR 不关心 |
| tag | int = `n<<1`；`nil` = `1`；`true` = `2`；堆指针为奇数 |
| 参数传递 | 内部 yac ABI：x86_64 前 6 个寄存器（`rdi rsi rdx rcx r8 r9`）其余入栈（callee 见 `[rbp+16+…]`）；arm64/riscv64 前 8 个 |
| 名字 → 地址 | **策略点 `fn_entry(name)`**（§7.6） |
| `syscall` | `nr = 60` 表示进程退出（arm64/riscv64 映成 93） |
| `_start` 构造 | `local` → 顶层绑定 → 各顶层函数的 `gset(name,16,…)` 发布 → `untag` + `syscall 60` |
| 栈对齐 | `call` 前 SP 16 字节对齐（`emit_x86_c_align` / `unalign`） |

### 7.5 TCO 的两条路径

| 情形 | 实现 | 是否 TCO |
|---|---|---|
| self 尾递归 | `emit_x86_tloop`：参数压栈 → 弹回槽 `1..n` → `jmp $tco`（**不拆帧**） | ✅ arity 不限 |
| 跨函数尾调用，`≤6` 参 | `emit_x86_tcall_other`：搬运 → `mov rsp,rbp; pop rbp; jmp rel32` | ✅（**兄弟调用**） |
| 跨函数尾调用，`>6` 参 | 退化成 `call` + `add rsp` + `leave/ret` | ❌ |

> ⚠️ **跨函数 TCO 现在从 `lir.yac` 侧无人生成** —— `maybe_tcall` 只在
> `nth(insn,2) == self` 时改写。`tcall_other` 的 `≤6` 那条路径是**准备好但没人用**的。
> 打通它只需 ANF 侧的 `callι` 对任意 `tail?` 目标发 `tcall`（`ANF.md` §3.4），后端不动。

### 7.6 现状缺陷：`fn_entry` 不存在

"装一个 callee 地址再间接调用"这段逻辑在 emit 里**写了三遍**：

| 位置 | handler |
|---|---|
| `emit_x86_64.yac` `gvar` | `movabs 0 占位 → patch[22] → or1 → store`（AOT 再加 patch[23]） |
| `emit_x86_64.yac` `xcall` | 同上，后接 `call`（动态） |
| `emit_x86_64.yac` `call` 的 `via_slot` 分支 | `movabs 0 占位 → patch[21] → mov r11 → call r11` |

**应该收敛成一个策略点**：

```
fn_entry(name) -> 绝对地址 | 桩槽      ; AOT = 布局期 bake；yjit = 布局期填；blob = jsess patch
emit_callee_ref(name) -> 寄存器        ; 内部调 fn_entry
```

新增镜像形态 = 加一个 `fn_entry` 分支 + 一个填表者，**不动 LIR**。这也是
`xcall` 消失的抓手 —— **先合并三份重复，`xcall` 会自然退化成 `call` 的一个分支**。

## 8. 校验规则（`--verify-lir` v1 已落地，本轮）

`yc --verify-lir`：走**真实管线**（`pass_lir`），报出**全部**违规，而不是在 emit 时
死在第一条。分两级 —— **error 使运行失败**，**warn 只报告**。

| # | 检查 | 级别 | 状态 |
|---|---|---|---|
| 1 | 每个非空 `proc` 首条指令是 `local` / `$local` | error | ✅ |
| 2 | 每条指令的首元素在 emit 能处理的集合里 | error | ✅ |
| 5 | `jmp` / `cmpjmp` / `$jcc` 的目标 label 在同一 `proc` 内有定义 | error | ✅ |
| 6 | `$` 指针类指令只在 `$proc` 内 | **warn** | ✅（降级，见下） |
| 9 | 每个 `proc` 的码名唯一 | **warn** | ✅（降级，见下） |
| 3 | 每条指令的操作数个数与 §4 一致 | — | ❌ 需要逐 `op` 的 operand-kind 表 |
| 4 | 所有槽号 ∈ `1 .. nslots` | — | ❌ 同上 |
| 7 | `closure` 的 `capSlots` ∈ 外层帧槽范围 | — | ❌ 需要外层帧上下文 |
| 8 | `ncap == len(fvs)` | — | ❌ 等 §5.5 S2 |

**为什么 6 / 9 降为 warn**：它们报的是**真现象**，但"是 bug 还是既有设计"要人判断。
首次运行就在现有 runtime 上抓出两类（以前从未被任何检查看见过）：

| 现象 | 现状 | 待判断 |
|---|---|---|
| `$f64*` 出现在普通 `proc` | `yac_f64_from_str` / `_binop` / `_rel` / `_print` 在 `runtime.yac:2423-2426` 声明为 **`proc`**（不是 `$proc`），却用 `$f64*` | 要么改成 `$proc`，要么确认 `$f64*` 的结果不会以未打 tag 的指针落进被 GC 扫的帧 |
| `proc` 码名重复（每程序约 16 个，如 `yac_num_*` 与用户函数 `f`） | 两个成因：① **前瞻 stub + 实体**都留在 `Σ` 的列表里；② `rt.num` 被 `rt_for_link` 加了两次（`visit_go` 的 seed + `rt_base`） | ① 设计使然，但列表里有冗余；② 是**真重复**，值得查 |

> **`vops` 与 `emit_x86_64` 的分派链必须同步。** 校验器自己存了一份名字集合
> （`backend.yac` 的 `vops`）—— 因为 LIR 没有反射可用。emit 新增 handler 而没更新它，
> 校验器就会**误报** unknown op（这次就踩过：漏掉了走 `is_raw_op` 分类而非显式分支的
> `$syscall`）。反过来，改 `vops` 而不改 emit，只是把错误推迟到 emit 期的 `log_fatal`。

**校验器的价值**：把 §1 的不变量从"文档承诺"变成"机器可验"。

## 9. 缺陷清单

### 9.1 ~~三处静默兜底~~ → 已改成报错（本轮）

| 位置 | 原状 | 现在 |
|---|---|---|
| `lir_atom` 的 `else` | 返回槽 `0` | `log_fatal("LIR: unknown ANF atom …")` |
| `lir_expr_i` 的 `else` | `pack_i(st, i + 1)`，静默跳过 | `log_fatal("LIR: unknown ANF bind …")` |
| `emit_insn_go` 的 `else` | `st`，静默忽略 | `log_fatal("emit: unknown LIR insn …")` |

`log_fatal`（`lib/log.yac`）= `print("error: " + msg)` + `exit(2)` ——
**IR 构建与 emit 分派都没有返回值通道**，所以只能打印并停下。退出码约定：
**1 = 用户错误**（语法 / unbound），**2 = 编译器内部不变量违反**。

> ⚠️ 只改了 `emit_x86_64`。`emit_arm64` / `emit_riscv64` 各有自己的 `else st`，待同改。

### 9.2 `lir_clos_atom` 的 0 捕获

`lir_clos_atom` 造 `["closure", dst, fname, []]` —— **声明 0 捕获**。
只对被引用 proc 的 `ncap == 0` 时安全。若 `ncap > 0`：

- 闭包声明"我没有捕获" → 调用时不传前导 caps
- 但函数体照样从槽 `1..ncap` 读 → **读到垃圾**

它对 `print`（ncap=0 的 runtime 名）是对的；对任意 proc 名需要**断言 `ncap == 0`**。

### 9.3 `topfn_has` 泄漏

`topfn_has` 是一个全局 box，属于**前端概念**（"哪些名字是本编译单元的顶层函数"），
却出现在 `lir.yac` 里（`free_vars` / `lir_var` / `calli` / `lir_letfun_finish`）。
且依赖调用方**记得**先调 `topfn_reset` + `topfn_scan`（`backend.yac` 两处都调了，
`dump_lir` 忘了 —— §9.4）。

**改法**：静态名集合作为**参数**传入 `lir_all`，LIR 内部不认识"顶层"。

### 9.4 ~~`--dump-lir` 不忠实~~ → 已修（本轮）

原 `dump_lir` 与真实路径 `pass_lir` 有**四处语义差异**：

| 缺什么 | 后果 |
|---|---|
| `topfn_scan(anf, 0)` | `topfn_has` 恒 false → **顶层函数不 flat** |
| 跨 item 累积 `st` | Σ 不累积 → 引用前一个 item 定义的函数落到 **`xcall`** |
| `sigma_of_rt(rt0)` | Σ 里没有 runtime proc → runtime 名解析不到 |
| `start_proc` + `tco_prog` | 看不到顶层发布序列，**看不到 `tcall` / `$tco`** |

**前三处已修**：`dump_lir` 现在走 `pass_lir` 的同一套设置。修前 / 修后
（输入 `let f(n) = if n <= 0 then 0 else n + f(n-1)` + `print f(10)`）：

```
修前  [[closure, 1, f, []]]                        ← f 本应 flat（ncap=0），却发了 closure
      [[mov_imm, 1, 20], [xcall, 2, 3, f, [1]]]    ← 同单元调用落到了 xcall
修后  [[mov_imm, 2, 0]]
      [[mov_imm, 2, 0], [mov_imm, 3, 20], [fcall, 4, f, [3]],
       [mov_imm, 5, 2], [fcall, 6, print, [4, 5]]]  ← 无 closure、无 xcall
```

`tests/compiler/lir/` 的 9 个 golden **全部不变** —— 说明旧 dump 只在这类
"顶层函数 + 跨 item 引用"的程序上撒谎。

> **第四处仍是缺口。** `dump_lir` 打印**逐 item** 的 LIR，不打印合成出来的 `_start`，
> 所以看不到顶层发布序列与 `tcall` / `$tco`。**不能**简单地改成打印 `pass_lir` 的完整
> 输出 —— 那含**全部 runtime proc**，会淹掉 golden。修法：加一个 `--dump-lir-start`
> （只打 `_start`）更合适。

### 9.5 死代码：幽灵 6 条 + 死 handler 33 条

**先分清两类 —— 处理方式完全不同**（旧版把两类混在一起，计数与归类都不对）：

| 类别 | 定义 | 明细 | 条数 |
|---|---|---|---|
| **幽灵** | 文档里有、`emit` 里**没有分支** | `nop` / `save` / `restore` / `neg` / `lnot` + **`print`** | **6** |
| **死 handler** | `emit` 里有分支、**全仓无生产者** | 见下 | **33** |

死 handler 的构成：

| 组 | 条数 | 明细 |
|---|---|---|
| 内联原语族 | **27** | §4.10 —— 列表 10 + 高阶 2（`map` / `foldl`）+ 字符串 4 + bytes 5 + 文件系统 6 |
| 算术别名 | 3 | `sal` / `sar`（与 `shl` / `shr` 写在**同一个**分支里，删起来只是去掉一个 `or`）、`$lea`（`:1119` 独立 handler） |
| rev1 残留 | 3 | `gvld` / `gvst` / `gcall` |

> **旧文档写的是"32 条"，且把 `sal` / `sar` / `$lea` / `print` 归为"幽灵（emit 里
> 连 handler 都没有）"—— 实测三处不符：**
> `sal` / `sar` / `$lea` **有** handler（是死 handler，不是幽灵）；
> `print` **没有** handler（是幽灵，不是死 handler）；
> `map` / `foldl` 是**完全漏列**的死 handler。

处理方式已由 §4.10 定下：**这 27 条全删**（第 4 步执行）—— 它们是**陈旧转发**
（参数走 SysV，而 `yac_*` 是槽约定的客过程），不是可复用的内联实现。

### 9.6 `$ld64`/`$st64` 与 `$ld8`/`$st8` 的不对称（**保留不修**）

| 指令 | base 处理 | 后果 |
|---|---|---|
| `$ld8` / `$st8` | **不做** `and_rax_1` | base 按裸指针解引用 |
| `$ld64` / `$st64` | **做** `and_rax_1` | base 先被去 tag → 与 `mref` / `mset` **逐字相同** |

**这是本轮的结论：不修，两份都留。** 判断依据与推翻方法见 §4.7 的警告框，摘要：

1. runtime 的写法本来就是"用之前自己显式去 tag"（`rt_gc_mark_ins` / `rt_apply1_ins`
   里都有 `["$and", x, 1, 0 - 2]`），所以那两处 `and_rax_1` 在 **132 个调用点上都是
   no-op** —— 删掉在语义上应当无害。
2. **实测跑通了自编译**（`yc_l1` 成功编出 `yc_l1b`，20.4s）。
3. 但**二代自举坏掉**（`yc_l1b`；恢复 `and_rax_1` 后重建的 `yc_l1c` 同样坏 —— 旧记录
   为 `error: bad arguments`，本轮实测是**任何输入都 SIGSEGV**）→ 这次删改**无法验证**。
   **注意：恢复 `and_rax_1` 之后二代仍然坏，所以它不是原因。** 真正的原因已在 **§9.7**
   二分出来 —— 是**当前管线的 flat codegen**，与本次删改无关。
4. 结论：`and_rax_1` 保留；`$ld64` / `$st64` / `$ld8` / `$st8` 作为 **4 条独立指令**
   留在 §4.7（不做"并成 2 条 raw 存取"那个收敛）。

> **教训值得记一笔**：这一处"删掉一个 no-op"本来看着是最安全的改动，却因为**没有可用
> 的二代自举回路**而变得不可验证。**第 0 步（修自举链）不是仪式 —— 它是所有其它改动的
> 前置。** 该回路本身的根因见 §9.7。

### 9.7 二代自举崩溃的根因：flat 路径（本轮二分）

**症状**：`y.exe` → `yc_l1` 正常；`yc_l1` → `yc_l1c`（二代）**任何输入都 SIGSEGV**，
连 `--dump-lex` 都挂。但读文件是好的（`src_len=39` 与输入长度一致），崩在读取之后。

一个**内建的诊断线索**：二代编译时**漏出**了本该被压住的 `[yc] start` / `[yc] src_len=…`
日志行 —— `log_hush` 是顶层 box、初值 `true`，漏出来就证明**顶层 box 的初始化没生效**。
这与"`pkg_extra_box` 启动即脏值、`pkg_set` 不可用"是**同一病根**。

**二分**（每次只改一个变量，重建两代，用 `let g = 10` / `let f(x) = x + g` / `print f(1)`
这个 39 字节的用例跑二代）：

| 对照 | 二代 |
|---|---|
| 原样 | 崩 |
| 恢复 `and_rax_1`（§9.6 那处） | 崩 |
| `start_proc` 的 `pubs` 置空 | 崩 |
| 退回 `fvs` 强制 0 捕获 + `flat = topfn_has(name)` | 崩 |
| **`topfn_scan` 置空 → 没有任何顶层函数走 flat** | **正常（输出 11）** ✅ |

**结论**：只要当前管线生成 **flat 形式**的顶层函数，二代就坏。与 `and_rax_1` 无关
（那一条已独立排除，§9.6 的"保留不修"结论不变）；与 `pubs`、与 `fvs`/`flat` 的写法也无关。
而 `y.exe`（Sep-7 管线）产出的 flat 代码是**好的**（所以 `yc_l1` 正常）——
**坏的只是当前管线的 flat codegen**。

**搜索范围已收窄**：`lir.yac` 的几处改动已逐一排除，余下差异分布在
`emit_x86_64.yac` / `emit.yac` / `jit.yac` / `runtime.yac` / `backend.yac` ——
也就是"flat 调用怎么发、`gfnst` 怎么写、`closure` 分支怎么走"。**下一轮从这里下手。**

> ⚠️ **这是当前唯一挡路的阻塞，而且比"二代验证"更严重。**
>
> `Makefile` 重建 `$(YC_A)` 用的是**两遍**流程（pass 1 用现有 `yc_a`，pass 2 用 pass 1
> 产出的**二代**），所以**只要动了任何编译器源码，`make` 就重建不出 `yc_a`**。
> 实测：`make test-compiler` → `pass 1` 正常，`pass 2` **Error 139**（SIGSEGV）→
> `make test*` / `make bootstrap` / golden 套件**全部阻塞**。验收只能手工做（§10）。
>
> 在它修好之前，任何依赖"二代自举"的验证都不可信 —— 包括 §9.6 那条"保留不修"
> （它当时是**推不出来**才保留的）。

## 10. 落地顺序

| 步 | 内容 | 为什么 |
|---|---|---|
| **1** | ~~决策 §4.10~~（内联原语 vs `call yac_*`）✅ **本轮完成：27 条全删** | 决定指令集大小，后面全依赖 |
| **2** | ~~修 `--dump-lir`（§9.4）~~ ✅ **本轮完成** | 后面每一步的验收都要它 |
| **3** | 三处静默兜底改报错（§9.1）✅ · `--verify-lir` **v1**（§8，规则 1/2/5/6/9）✅ | 让不符合规范的东西**立刻暴露** |
| **4** | `$` 不对称 → **决定保留不修**（§9.6） · 清死 handler（§9.5）：无争议的 4 条（`sal` / `sar` / `$lea` / `gcall`）已删；**27 条内联原语按 §4.10 全删（待执行）** | 规范立起来后清死代码才有依据 |
| **5** | `fvs` 落进 `proc` 头（§2 / §5.5 S2）+ 删 `maybe_tcall`（§5.5 S1） | 小改动，解锁"读 fv 表"的验证 |
| **6** | 指令集收敛（§5 全部） | 纯重构 |
| **7** | `topfn_has` 出 LIR（S3）+ `fn_entry`（§7.6） | 消除概念泄漏与重复 |
| **8** | 性能（§11） | 独立，可并行 |

> **本轮的验证方式**：用 9-07 的 `y.exe` 重建 `build/test_tmp/yc_l1.exe`（约 20s），
> 跑 `fact` / 闭包 / `l4_42` 三个基准 + `tests/compiler/cases/` 全部编译 +
> `tests/compiler/lir/` 的 9 个 golden（**一代**，全部通过）。
>
> **第 0 步（自举回路）已定位、但推迟。** 根因是当前管线的 **flat codegen**（§9.7），
> 修它属于"闭包表示"那条线，**排在 5 / 6 之后**。所以第 0 步不是"先做"，而是
> **"二代验证被它锁住"**。
>
> ⚠️ **而且锁得比原先估计的彻底。** `Makefile` 的 `$(YC_A)` 重建本身就是**两遍**流程
> （pass 1 用现有 `yc_a`，pass 2 用 pass 1 产出的**二代** —— 注释说明是为了让 emit 的
> 更新反映到 runtime 的重新发射上）。所以**只要改动任何编译器源码，`make` 就无法重建
> `yc_a`** → `make test*` / `make bootstrap` **全部阻塞**。golden 套件也要
> `TEST_HARNESS`，而它依赖 `$(YC_A)`，所以**同样跑不了**。
>
> **§9.7 的验收只能手工做**：`./y.exe` → `yc_l1`，再用 `yc_l1` 编译整个 bundle
> （编译器自己的源码会用到的每个 opcode 都会走一遍）+ 跑用例。这是"pass 1"的手工等价物。

**关键约束**：2 → 3 → 4 必须在 5 / 6 之前。5、6 都是"改 IR 形状"的事，而现在的
`--dump-lir` 报的是假象、三处静默兜底会掩盖错误。**先有可信的观测和严格的报错，再改 IR。**

## 11. 性能结构（独立于以上全部）

| # | 内容 | 现状 |
|---|---|---|
| C1 | **槽复用**（活跃性分析 + 着色） | `nslots = nth(br,3) + 2`，`ctr` 每临时 +1，**从不复用** |
| C2 | **算术内联**（int 快路径 + 溢出检查） | `bin("+",d,a,b) = ["fcall", d, "yac_num_add", [a,b]]` —— 每次 `+` 都是一次过程调用 |
| C3 | 常量折叠 | `2*3` 仍发 `mov_imm;mov_imm;mul` |

## 12. 与 Chez 的层对照

| Chez（`s/cpnanopass.ss`） | yac |
|---|---|
| `np-convert-assignments`（把被赋值的变量装箱） | **无**（yac 无赋值；ANF 的别名天然不可变） |
| L1–L5：`lambda`，无自由变量信息 | ANF：`letfun`（无 fv 信息） |
| `np-convert-closures` L5→L6，引入显式 `(closures …)` | **无对应层** —— 压进 `lir_letfun_*` |
| `np-optimize-direct-call` / `np-identify-scc` / `np-lift` | **无**（缺调用图 → 缺 well-known） |
| `np-expand/optimize-closures` L6→L7（决定闭包表示） | 一行 `flat = topfn_has(name) and ncap == 0` |
| `np-impose-calling-conventions` L12.5→L13 | 隐含在 LIR 的 `caps` 字段约定里 |
| `uncover-live` / `build-interference` / `color` / `assign-registers` | **无**（槽不复用，§11 C1） |
| `np-place-overflow-and-trap` | **无**（算术全走 runtime 调用，§11 C2） |

**yac 的 LIR 大致相当于 Chez 的 L7–L9 之间**（闭包已展开、表示已决定、尚未做
寄存器分配与指令选择）。
