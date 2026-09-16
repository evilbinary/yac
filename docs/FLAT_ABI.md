# FLAT_ABI.md — Flat ABI 与目标 IR

> **本文档只描述待实现的目标。** 已落地的部分（顶层函数 flat：`gvar` + 静态
> stub cell + `fcall` rel32）不再展开，仅在建立上下文时用一句话带过。
>
> **本文只管「flat / 静态化」这一件事。** IR 的权威定义在 `docs/LIR.md`
> —— 指令集、迁移表、后端契约都在那里，本文只留指针。

## 目标

一句话：**让"扁平"成为编译器分析的常态结果，而不是顶层函数的特例。**

三件事：

| # | 内容 | 解决什么 |
|---|---|---|
| **1** | **值也静态化** | 顶层 `let x` 有静态单元；引用它就是一次 load，不再走 caps 链。这是"顶层函数真捕获数 = 0"成立的前提 |
| **2** | **闭包表示成阶梯** | 从"零分配静态"到"堆闭包"共五档；判据与"是否顶层"**无关** |
| **3** | **IR 收敛** | call 家族 10 条 → 3 条；名字/值访问统一（方案见 `LIR.md` §3 / §5，摘要见 §4） |

### 没有 1 会怎样

顶层函数只要读任意一个顶层 `let` 值，`ncap > 0` → 不是 flat → 变回闭包 →
调用方又得传前导 caps。于是这类函数在跨镜像场景下**依然拿不到环境**：
名字表只给**入口地址**，不给环境。`import compiler` + `compile("1+2")`
就会卡在这里。

**顶层函数理论上 100% 可 flat** —— 它的自由变量只可能是顶层名，而顶层名的
地址是编译期常量。做不到的唯一原因就是"顶层值还没有静态的家"。

## 准则

1. **一个判据：`ncap` = 函数体中自由引用的「真局部变量」数。** flat ⇔ `ncap == 0`。
   顶层函数名、顶层值名都有静态地址，**不计入**。
2. **一个单元：** 每个顶层名字一个静态 cell，函数和值**共用同一布局**。
3. **lir 不认识槽位 / 镜像边界。** lir 只发**名字**；槽位分配、跨镜像解析、
   打补丁全是 emit 的事（策略点 `fn_entry`）。
4. **call 只有一个**（+ 尾位置 + C 调用）。目标解析方式、ABI 家族是**字段**，
   不是 opcode。（完整展开见 `LIR.md` §3 的 KISS / OCP。**`caps` 这个字段本身
   将取消** —— 见准则 7。）
5. **判据必须是结构事实。** 判定"是不是顶层名"只能用"该名字是不是某个顶层
   item 的**直接绑定**"——**不能靠名字形态猜**（`t0` / `_` 这类编译器临时
   会污染结果）。
6. **分层交付。** 每步独立验收，不要求一次性自举通过。
7. **过程对象是它环境的唯一入口。** 调用点只传对象，不再传捕获值：`ncap` 在
   callee 的 `proc` 头，运行期捕获数在对象的 `nenv`（`[+24]`）。这是取消 `caps`
   的目的地（`LIR.md` §4.4）—— 把"前导槽位"与"对象"两条取捕获的路**合成一条**。
   **硬约束：无对象可传的 C ABI 导出（`--shared` / `--shared-int`）必须 `ncap == 0`。**
   落地（`LIR.md` §4.4.7）：调用点按被调方种类发 `ycall`（yac 过程，对象 ABI，参数从
   `rsi` 起）或 `fcall`（运行时 `$proc`，SysV，参数从 `rdi` 起）；种类由 Σ 表项第 4 字段
   显式携带，emit 端不推断。yac 过程的序言以 `["local", nslots, nparams, 1]` 的尾字段
   标记对象 ABI，并把 self 存入保留寄存器 `r13`。

---

## 1. 扁平化判据

现在的 `ncap = len(fvs)` 把三种本质不同的东西混在一起了：

| 自由变量 | 本质 | 访问方式 | 计入 `ncap`？ |
|---|---|---|---|
| 顶层 `letfun` `g` | 编译期常量地址 | `gvar g` → 装入口 | **否** |
| 顶层 `let` 值 `v` | 静态 cell | `gval v` → 读 `[cell+0]` | **否** |
| 函数体内的局部变量 `y` | 真·动态环境 | **唯一**需要 env 前缀的 | **是** |

所以：

```
ncap := 函数体中自由引用的「真局部变量」个数
flat ⇔ ncap == 0
```

分类与重定向是**纯语法工作**（fvs 与名字分类都已知），不需要任何运行时机制。

### 判据从哪来：ANF 的结构

**"顶层名集合"直接来自 ANF 的结构，不需要任何分析。** `anf_all` 产出的是
"每个顶层 item 一个 ANF body"（`anf.yac:184-189`），所以 `body = [binds, tail]`
里的 `binds` 就是该 item 的**直接**顶层绑定。`topfn_scan` 已经在用这个事实
（只扫 `nth(nth(items, i), 0)`，从不下降进嵌套 body）。扩成 `scan_toplevel`
即可同时登记函数与值。

两条纪律：

1. **只接受结构事实，不猜名字形态。** 顶层 `let x = 5` 与编译器临时 `t0` 在 ANF
   里形状**完全相同**（都是 `["let", name, atom]`）。区分它们既不可能，也没必要
   ——顶层临时同样是顶层绑定、同样有静态地址。
2. **过近似靠惰性分配消掉。** cell 在发 `gvar` / `gval` 的那一刻才 `gref_alloc`；
   没人从嵌套作用域引用 → 不分配、不发布。而 ANF 的**别名**（`["let", "y",
   ["var", "t0"]]`，见 `anf.yac`）保证了被捕获的永远是源级名字，临时名不会进 fvs。

### 逐函数转换算法

```
对每个 letfun / lambda f:
  fvs = free_vars(body)
  对每个 fv 分类:
    顶层函数 g → 引用点重写为 ["gvar", d, g]；调用点 call(name=g)
    顶层值   v → 引用点重写为 ["gval", d, v]
    真局部变量 y → 计入 ncap，保留 env 前缀
  若"真局部变量"类为空 → f flat:
    proc 头 ncap = 0，ntot = nparams，rdi 就是第一个真参
  否则 → 保留闭包（只应发生在捕获了局部变量的内层 lambda 上）
```

---

## 2. 名字单元（值静态化）

### 2.1 布局与三种访问

**cell 的完整布局与访问指令见 `docs/LIR.md` §4.5**（32B，放在 globals 数据区，
**不在 GC 堆**）。函数与值**共用同一布局**，只是访问不同的偏移：

| 用途 | 读 / 写 cell 的哪里 |
|---|---|
| 函数当值 | **单元地址本身**（tagged）—— 直接当闭包对象用 |
| 顶层值 | `cell + 0` |
| 发布 | `cell + 0`（值）或 `cell + 16`（函数入口） |

**关键点：`nenv = 0` 让这个单元本身就是一个合法的零捕获闭包对象。** 于是：

- 函数：`call` / 间接调用读 `[+16]` / `[+24]`，**通用闭包路径零改动**。
- 值：读 `[+0]`。
- 「函数值」和「值的容器」是同一种东西 → 值传递 / 返回 / 存容器**全不用改**。

### 2.2 值的三类初始化

值有了静态单元，必须回答"谁在什么时候写进去"：

| 情况 | 处理 |
|---|---|
| **纯常量 / 字面量** | 直接 bake 进 data 段，零运行时动作（应覆盖大多数顶层 `let`） |
| **`let x = f(1)`（依赖调用）** | `_start` 里、任何函数被调用之前，**按源序** `gset(name, 0, src)` |
| **前向引用**（`let a = b`，`b` 在后） | 必须**显式定义**：要么分段（stage），要么编译期报错。**不能靠运气** |

### 2.3 硬性要求

1. **顶层发布顺序是契约**，不是实现细节。
2. **cell 必须登记为 GC root**，否则值会被回收。
3. **帧指令必须排在所有发布之前** —— 它才是读 argc/argv、建帧、写 GC `stack_hi`
   的地方（`LIR.md` §2 已把这条写成语法约束：指令序列必须以帧指令开头）。
4. **槽位分配必须幂等。** `gref_alloc(name)` 对同一名字恒返回同一下标；
   索引表单一来源，读写两侧不可能不配对。

### 2.4 跨镜像（L2）

cell 表是**共享**的，不是拷贝：

- 宿主：每个顶层名一个 cell，`_start` 里发布完毕。
- blob：`gvar` / `gval` 里的**名字**由 `fn_entry(name)` 解析；跨镜像时解析为
  一个待填补丁槽，`jsess` 把它 patch 到**宿主的同名 cell**。
- 因为是同一块内存（不是拷贝），宿主后续改变量 blob 也看得见 —— 正是 REPL 语义。
- **因为 flat 函数没有前导 caps，`compile("1+2")` 的 `rdi` 就是字符串** →
  原始问题解决。

---

## 3. 闭包表示阶梯

### 3.1 两个正交判据

1. **`free*` 是否为空** → 决定是不是"零分配静态"
2. **callee 是否 well-known**（**全部**调用点都静态可知）→ 决定有捕获时能否退化

> **静态化的判据是 `free* == ∅`，与"是否顶层"无关。** 顶层只是这个判据的
> 一个特例——顶层函数的自由变量全是全局名，碰巧为空而已。

### 3.2 五个表示

| `free*` | well-known | 表示 | 分配 |
|---|---|---|---|
| **空** | — | 静态 cell：值就是 `cell \| 1` | **零** |
| 1 个 | 是 | **就是那个自由变量本身**，不包对象 | **零** |
| 可借用 | 是 | 复用另一个必然存在的闭包记录 | **零** |
| 2 个 | 是 | 2 字对象，直接调用约定 | pair |
| n 个 | 是 | 定长 vector，直接调用约定 | vector |
| 任意 | 否 | 堆闭包对象（现状） | heap |

参照实现：Chez Scheme `np-expand/optimize-closures` 的表示选择
（`s/cpnanopass.ss:2412-2442`，判定字段名 `closure-type` ∈
`constant | singleton | borrowed | pair | vector | closure`）。

### 3.3 yac 的落地路径

| 档位 | 前置条件 | 落地步骤 |
|---|---|---|
| 空 `free*` → 零分配 | 顶层值静态化（§2） | 第 2 步 |
| 判据下沉到任意 lambda | 同上 | 第 3 步 |
| `singleton` / `borrowed` / `pair` / `vector` | **需要调用图**：算出每个 lambda 的调用点集合，全部静态可知才算 well-known | 第 7 步 |

**yac 缺的两块：**

1. **调用图 / well-known。** 现在只有 `Σ` / `topfn` 这种"名字表"，没有"调用图"。
2. **lift。** 现状**两条路并存**：`call` / `tcall` 的 `["static", n]` 走**前导槽位**
   （调用方把 n 个值搬进 callee 槽 `1..ncap`），`["dyn"]`（间接调用）走**对象**
   （个数读 `[obj+24]`，第 i 个读 `[obj+32+i*8]`）。本文件所说的 "boxed env"
   指的是**后者**。而 "lift" 的方向与**前者相反** —— 把自由变量改成
   **调用时多传参数**（unboxed args）以缩小闭包槽位。
   **顺序**：先把两条路**合成一条**（取消 `caps`，见 `LIR.md` §4.4），再对热点做 lift
   —— 因为 lift 依赖 `well-known`（调用图，第 7 步），而"合成一条"不依赖。

### 3.4 收益示例

`map(xs, fun(x) -> x + g)` 现在必须 `closure` + `icall`（每次求值分配一个
闭包对象）。`g` 静态化 + `map` 判为 well-known 后，这个 `fun` 命中
`free* = {g}`（1 个）→ **值就是 `g` 本身，零分配**。

---

## 4. IR 简化的两个前提

IR 收敛的完整方案（KISS / OCP 准则、六类维度泄漏、逐条迁移表、`fn_entry`）
全在 `docs/LIR.md` —— 索引见文末附录。**本文不写任何指令形态。**

对 flat 而言，IR 收敛有**两个前提**必须先行：

1. **`caps` 必须带标签，不能用魔法值。** 现在 `apply_ncap` 用 `-1` 表示"动态"，
   这正是"该做成显式字段"的信号。目标是把 caps 布局做成调用指令的一个**字段**
   （编译期已知 n 个前导捕获 / 运行期动态 / `raw` 直调），而不是多条独立 opcode。
2. **尾位置必须先变成结构。** 现在 LIR 用 `maybe_tcall`（`lir.yac:832-839`）事后
   把 `fcall` 改写成 `tcall` —— 这正是"尾位置不是结构"的直接后果，也是尾位置
   变成 3 个 opcode 的根因。ANF 改成 `body = [bind*, tail]` 后（`ANF.md` §4.1），
   尾位置由语法给出，`maybe_tcall` 与 `ticall` / `tailapply` 一并消失（第 4 步）。

> **顺序：先做 `fn_entry`，再删 opcode。** 三份重复代码合并之后，`xcall` 会自然
> 退化成 `call` 的一个分支，而不是被强行删除（已列入 `LIR.md` §10 的顺序表）。

---

## 5. 落地顺序与验收

| 步 | 内容 | 验收 |
|---|---|---|
| **0** | 修好自举链（当前工作树 `yc.exe` 一进 `--ast` 即 SIGSEGV） | `yc` 自编译两遍 + `--ast` 不崩 |
| **1** | 单镜像 flat 基线收尾（顶层函数 flat） | 两遍自举、`make yc-iso`、`tests/compiler` 各阶段 |
| **2** | **顶层值静态化**（§2）：`scan_toplevel` 判据、`gval` / `gset`、发布时序、GC root | 顶层 letfun **全部** `ncap = 0`；LIR dump 里 `gval` 条数 == 顶层 `let` 条数 |
| **3** | **判据下沉**（§1）：`flat ⇔ 真捕获数 == 0`，作用域扩到所有 lambda | 函数体内无捕获的 `fun` 也生成静态 cell；各阶段测试 |
| **4** | **ANF 修正**（`ANF.md` §4.1）：尾位置结构化 `body = [bind*, tail]`；`letcallcc` 多值形状；`anf_expr` 兜底改为编译期报错 | 各阶段 golden 不变；删掉 `tail(x)` 谓词与 `maybe_tcall`；不再出现 `ticall` / `tailapply` 生成 |
| **5** | **L2 跨镜像**（§2.4）：cell 共享（jsess patch）+ `fn_entry` 策略点 | link 13 PASS、repl 26/26、`import compiler` + `compile("1+2")` e2e、`blob_len > 0` |
| **6** | **IR 收敛**（`LIR.md` §5）：`call` / `tcall` / `ccall` + `caps` 字段 | 每阶段 golden 不变（纯重构：IR 形状变、语义不变） |
| **7** | **调用图 + well-known + lift**（§3.3） | 分配计数下降；`map` / `foldl` + 无捕获回调不再产出 `closure` 指令 |

第 2、3 步做完，"非顶层也能静态化"就成立了。第 7 步才是真正消灭分配的地方。

**暂不列入的：`letrec`（互递归函数组）。** ANF 已预留
`["letrec", [fnbind*], body]`（见 `docs/ANF.md` §5.3），但**现在不实现**。
它的前置正好是上面几项：顶层名字预扫描（第 2 步）、`calli` 对非 self 目标发
`tcall`（**跨函数 TCO**，后端 ≤6 参数的兄弟调用已就绪）、绑定检查放开前向引用。
缺了跨函数 TCO，互递归只是"能编译但长链爆栈"。

顶层互递归**不需要** `letrec` —— `scan_toplevel` 放行即可（两个 flat 函数、
入口是编译期常量，组协议是空操作）；真正需要它的只有**嵌套**互递归组。

### 可观测指标

否则"静态化做对了"没有可验证的判据：

- **静态化名字数**（应等于顶层 `letfun` + 顶层 `let` 数）
- **每个 lambda 的真捕获数**（`--dump-lir` 里 `proc` 头的 `ncap`）
- **分配计数**（第 7 步后应下降）

### 验证载体

- `tests/compiler/<stage>/`：`lex` / `parse` / `anf` / `cps` / `opt` /
  `uncps` / `lir` 逐阶段 golden（`<name>.yac` + `<name>.expected`）；
  `lower` 编译 + 运行比退出码；`pack` 比镜像魔术字节。
  `tests/compiler/run.yac --bless` 重新记录 golden。
- `yc` 增加 `--dump-lex` / `--dump-lir`（照 `--dump-anf` 接线），
  这是"每阶段严格测试"的最小前提。

---

## 6. 清理清单

在 L1 + L2 全绿后删除。

| 对象 | 原用途 |
|---|---|
| `bind_caps` / `outer_caps` / `cap_slots` | 前导 caps ABI 机制（顶层路径；嵌套闭包仍需要 `closure` 的 caps 槽位） |
| `host_env_extra`（backend） | blob 侧 G+136 槽填充 |
| `rt_host_call_ins`（`yac_host_call` 桥） | 调用点的 caps 解包 |
| `rt_gtab_get` / `xset` 表 | 名字→闭包注册表（flat 不需要传环境） |
| `via_hid` / `gtcall` emit 路径 | blob 调用的特例 → 收进 `fn_entry` |
| `yjit` clos_list / `host_tab_fill` 残留 | 首会话槽拷贝 |
| `gcall` | rev1 的调用指令 |
| `pass_lir` 的 `TOPVALS` 打印、`gvst_push` 的 `GVST?` 打印 | 调试残留 |
| 第 6 步收敛掉的 opcode | `xcall` / `apply` / `ticall` / `tailapply` / `iccall` / `$icall` / `gvld` / `gvst` / `gfnst` |

**能力不删，只改形态**：`apply` / 间接调用的**能力**保留（嵌套闭包 +
call/cc 式动态调用），但不再是独立 opcode —— 变成调用指令的「运行期动态 caps」
那一种布局（`LIR.md` §4.4）。

---

## 7. 与 DESIGN.md 的关系

`docs/DESIGN.md` 是语言与 IR 的权威设计。本文三件事与它的关系：

### 7.1 不违背「ncap=0 也要有闭包对象」

`DESIGN.md` §2 的 `letfun` 规则里有一条注释：

> `; ncap=0 也分配闭包：函数当值（map/filter）时槽里必须是闭包。`
> `; 按名调用走 fcall，不读这个槽。不做「只按名」分析。`

**本文不推翻这条要求。** 要求是"函数当值时必须是个闭包对象"，而 §2.1 的 cell
布局 `[value][mark][entry][nenv=0]` **就是这样一个对象** —— 只是放在静态区而不是
堆上。所以 `map(f)` 传静态 cell 地址、`icall` 读 `[+16]` / `[+24]` 全部零改动。

**冲突只在"这个零捕获闭包住哪"，不在"它存不存在"。**

### 7.2 DESIGN.md §2 需要同步改三处

| 规则 | 现状 | 改成 |
|---|---|---|
| `Γ ⊢ ["let", x, a]` | 一律 `Γ[x ↦ s]`（`_start` 的 frame slot） | **顶层**时 `Γ[x ↦ cell(x)]` |
| `Γ ⊢ ["letfun", …]` 的 `I_out` | 一律 `[["closure", …]]` | 真捕获数 = 0 时不分配，`Γ[f ↦ cell(f)]` |
| `⊢ program`（`_start`） | 直接跑各顶层 body | 顶层 `let` 要发 `gset`（发布）+ 处理前向引用 |

> **实现侧的两个前提**：`emit_insn_go` 的兜底是 `else st`（**静默跳过未知指令**），
> `lir_atom` / `lir_expr_i` 也有静默兜底 —— 这正是漂移长期没被发现的原因。
> 改法与执行顺序见 `LIR.md` §9.1 / §10。

### 7.3 DESIGN.md §8.2 需要补一条

§8.2 的根集合列的是 `m->code` / `m->env` / 实参数组 / 原语回调里的临时值
——**没有"globals 区"**，因为原设计里 yac 值不驻留在静态区。

本文引入 cell 后：

> **cell 区必须登记为 GC root。** 且 §8.2 明确"不做保守扫描、只用显式值栈"，
> 所以需要一条**显式的登记路径**，不能指望被扫到。

---

## 8. 实测缺口与欠账（台账，2026-09-15）

本节只记**实测到的事实**（行号以当时工作树为准），不是设计意图。目的：这些
点现在**不报错**，但迟早会被踩到 —— 记在这里，免得下次从零再查一遍。

### 8.1 四类失败（全量 679 / 7）

| 用例 | 现象 | 根因 | 证据 |
|---|---|---|---|
| `pkg profile` | rc **139**（期望 42） | **平过程被当闭包值调用**：profiling 打开后进普通函数 ⇒ 调用点按**对象 ABI** 放参（真参从 `rsi` 起、`rdi` 不设），被调方是**平**过程（形状 = `yac_str_cat`：`len(a)`+`len(b)`），从 `rdi` 读 ⇒ `len(0)` ⇒ 段错误 | gdb 崩点 `mov 0x18(%rax),%rax`（`rax=0`）；调用点 `mov %rax,%rsi; xor %rax,%rax; mov %rax,%rdx…`；被调方 `mov %rdi,-0x10(%rbp)` + 两个 `len`。x86_64 的 ABI 表：`ycall` `emit_x86_64.yac:702-733`（对象 ABI），`tcall_other` `:172-186`，自尾调用 `:758` / 跨过程尾调用 `:760`（写死 `traw=true`）。闭包值的产生点：`front/lir.yac:846`、`:1501` |
| `repl let fn after expr line` | `f(2)` 给 **2**（期望 3） | 12.14 的 **jslot / 闭包入口发布**那半：表达式行之后才定义的函数落在会话 blob 里，其闭包入口没人从宿主注册表填 ⇒ 调用落到错入口 | `tests/run.yac:479` 的注释即为这条的常驻哨兵 |
| `pkg compiler` | rc **0**（期望 42） | **不是崩溃**：程序调用 `compiler` 包的**宿主叶**（`@compile_native` / `@host_arch` / `@mk_target` / `@host_format`），它们只在 **yc 自己的镜像**里有实现；被当独立可执行跑时槽是桩 ⇒ 打印 `host fn unavailable` ⇒ 返回 0 | 桩的生成点 `emit_x86_64.yac:2065-2080`（`unstub_procs`）。**结论是跑法/设计缺口**：该用例应走进程内 host 路径，而不是独立二进制 |
| `import after use` | 失败 | 走的是 **C 解释器 `./yac`** 那条路（`interp` 组 35/1），与 `yc` 后端无关 | `make test-interp` |

> **规范结论（值得写进实现约束）**：**闭包值的调用目标必须是对象 ABI（标记帧）**。
> `$proc` 这类**平**过程被当成值（`closure`）去 `apply` / `icall` 时，必须在中间套一层
> 对象 ABI 的 **thunk**（丢弃对象、把真参从 arg1 搬到 arg0），或者干脆不允许这种取值。
> 缺这一层时不会编译报错，只会静默错位 —— `pkg profile` 就是它的一次实爆。

### 8.2 潜伏缺口（现在不失败，踩到才炸）

| # | 位置 | 内容 |
|---|---|---|
| 1 | `emit_arm64.yac:1475`、`emit_riscv64.yac:1480` | `tailapply` / `ticall` 带**栈参数**（>7）时发 `brk` / `unimp` ⇒ 未实现；真跳到就 trap，偏"响亮失败"但仍是缺口 |
| 2 | `emit_x86_64.yac:754` vs `emit_arm64.yac:685` / `emit_riscv64.yac:671` | 跨过程尾调用的目标 ABI：x86_64 **写死 `traw=true`**（当作平目标），arm64 / riscv64 按名字判（`is_yac_proc_name`）。假设不对称 —— 对象 ABI 的跨过程目标在 x86_64 上会被按平调用（与 §8.1 第 1 条同族） |
| 3 | 提交 `943d438` | 提交信息写 "correct riscv64 ycall param register offset"，实际内容是**回退 + 改注释**（`10 + j` 是对的，注释错了）⇒ 信息与内容不符 |
| 4 | 仓库根的 `./yc`（无扩展名） | 本机 `EXEEXT = .exe` ⇒ `make` 只产出 `yc.exe`；MSYS 的 `./yc` 会**抢先命中**那个无扩展名文件 ⇒ 陈旧二进制反复误导调试（已发生两次）。构建后须 `cp -f yc.exe yc`，或直接 `rm yc` |
| 5 | `build/` | 遗留 `patch_rv.py` 与 `emit_*.bak*` / `run.yac.bak` / `Makefile.bak` 等备份；无害，但别当源码读 |
| 6 | `qemu-*` 组 | `--shared ccall add` 在**转发 shim**下按设计 SKIP（每组 3 个）：shim 只上传可执行文件，`.so` 会被远端当 Windows 路径找。该用例只在**真 qemu** 环境回归 |
| 7 | 前端 | **顶层 `let` 绑定的值不能当函数调用**：`let f(x) = print(x)` + `let a = f` + `a("x")` ⇒ `error: LIR: call to undefined procedure 'a'`（`lir.yac` 的 `calli` 只认 Σ 里的过程与局部槽；顶层值名不在其中）。是**响亮报错**而非静默 ✓，但与"值是一等函数"不一致（Chez 里 `(define a f)` 后 `(a "x")` 是合法的）|

### 8.3 当时的基线（改动验收对照）

| 组 | 结果 |
|---|---|
| host `compiler` | 176 / 1 |
| host `interp` | 35 / 1 |
| host `pkg` | 19 / 2 |
| `qemu-arm64` | **81 / 0**（3 SKIP） |
| `qemu-riscv64` | **81 / 0**（3 SKIP） |
| 全量 `make test` | **679 / 7** |

### 8.4 已修复：平过程当值（2026-09-16）

**修的是什么。** `let f = <原语>` / `map(<原语>, xs)` 这类"原语当值"以前会得到一个
**平入口**的闭包 ✗ —— 闭包调用按对象 ABI 传参（对象进 arg0、真参从 arg1，`LIR.md`
4.4.4），而 `$proc` 的真参从 arg0 起 ⇒ 整体错一格 ⇒ `pkg profile` 读 `len(0)` 段错误、
`let f = str_cat` 静默无输出。且 `front/lir.yac` 的 `is_prim_name(nm)` 兜底把**任何**
原语都包成 `print` ✗（实测：`let f = len` / `cons` / `str_cat` 全部得到
`[closure, 1, print, []]`）—— 连 `repl` 的三条 `<fun>` 用例都只是**假通过**（值错、显示对）。

**怎么修的**（只动前端，后端零改动）：取值点不再把裸入口放进闭包 ✗，而是给该原语生成一个
**规范化的 0 捕获 wrapper proc**（每个目标名一个，名字 `<target>$v`）✓：

```
[proc, yac_str_cat$v, 2, 0, [[local, 3, 2, 1],            ; 标记帧 = 对象 ABI
                             [label, $tco],
                             [fcall, 1, yac_str_cat, [1, 2]], [ret, 1]], yac_str_cat$v]
[closure, dst, yac_str_cat$v, []]                         ; 取值点
```

`fcall` 是唯一把平约定**写明白**的调用形态（`LIR.md` 4.4），所以两侧都不需要靠名字形状猜
ABI。真名**不抄第二份表** ✓：直接问镜像的 proc 列表（试用 `nm` / `yac_`+`nm`，看哪个是
`$proc`）。wrapper 名确定 ⇒ "是否已生成"就是查一次单元自己的 proc 列表（Σ）⇒ **无全局
注册表、无 reset 陷阱**。三个取值点都收敛了：`lir_var` 的 Σ-`$proc` 分支、`is_prim_name`
分支，以及 `lir_qvar`（限定名 `pkg/name`）的同一条。

**效果**（`src-self` 两阶段自举 + 全组无回归）：

| 组 | 修前 | 修后 |
|---|---|---|
| host `compiler` | 176 / 1 | **176 / 1** ✓ |
| host `interp` / `pkg` / `boot` | 35/1 · 19/2 · 1/0 | 同 ✓ |
| `qemu-arm64` / `qemu-riscv64` | 81 / 0 | **81 / 0** ✓ |
| `let f = str_cat` + `f("ab","cd")` | 无输出 ✗ | **`abcd`** ✓ |
| `repl cons` / `exit as value` | 假通过（值是 `print`） | 真通过 ✓ |

**新台账（同一轮实测）：**

| # | 事实 |
|---|---|
| 1 | `filter` `foldr` `head` `popen` 在 `is_prim_name` 里，但**既无 `yac_*` 过程、也无 `lir_rt` 条目** ⇒ 连**按名调用**都是 `undefined procedure` ⇒ 当值只能**报错**（不静默） |
| 2 | `band` 是内联原语但没有 wrapper（见下）⇒ 当值报错 |
| 3 | **这个前端不允许前向引用** —— `lir_rt` 定义在 `lir_var` 之后，所以从取值点**不能**调用它（实测：`unbound variable 'lir_rt'`）⇒ 内联原语的 op 形状在 `lir_prim_inline_op` 里**抄了 4 条**（`exit` / `str_len` / `str_ref` / `bytes_len`），出处已注释；`band` 因为要复用 `bin()` 而暂未支持 |
| 4 | **`pkg profile` 仍是 139** ⇒ §8.1 第一条的"平过程当值"根因**已被证伪**（该机制修好后它照崩）⇒ 需**重新定位**；原先的旁证（被调方为"平 2 参过程"、调用点用对象 ABI）指向别的入口 |
| 5 | `a == cons` 仍是 **false**（实测 `let a = cons in a == cons` ⇒ 不等）—— 那是 §8.5 的下一步，不是本条修复的内容 |

### 8.5 已落地：名字单元静态化（Chez 式 `eq?`）

Chez 里 `(define a display)` 后 `(eq? a display)` 是 `#t` ✓，因为**读名字 = load**（一个
稳定的值对象），而这里 `let a = cons` 以前每次出现都**构造**一个新闭包 ✗（实测：`let a = f`
对**用户函数**是 `#t` ✓，对**原语**是 `#f` ✗）。

**修法（本轮已落地）**：§8.4 造 wrapper 时，把它的**取值**从 `["closure", dst, wn, []]`
换成 `["gvar", dst, wn]` —— `gvar` 本来就是这个语义 ✓（`emit_x86_64.yac:1279` 原文：
"a static stub closure [0, 0, entry, 0] in the image's globals area, **one per referenced
name** (32 bytes) … Value = the stub's address (tagged)"）⇒ **一名一个 cell ⇒ 同一地址** ✓、
**零分配** ✓、`icall` 的 `nenv=0` 展开照常 ✓、后端**一行未改** ✓。

| 实测 | 修前 | 修后 |
|---|---|---|
| `let a = cons in a == cons` | `#f` ✗ | **`#t`** ✓ |
| `let a = cons in let b = cons in a == b` | `#f` ✗ | **`#t`** ✓ |
| `let f = str_cat` + `f("ab","cd")` | 无输出 ✗ | `abcd` ✓ |
| 取值点分配 | 每次 `yac_alloc` | **无**（静态 cell） |

**范围被刻意收窄（实测教训）**：**只有我们自己造的 wrapper** 走静态 cell ✓；**用户
letfun 保持原来的分配式 `closure`** ✓。因为把这条**推广到所有 0 捕获闭包**会立刻炸 ✓：
`gvar` 的 entry 是**按名字烘焙**的，它的解析器不认识**捕获过程的 letfun 名** ⇒
实测 `error: EMIT: patch to unknown proc 'interp_step'` ✗（`tests/run.yac` 里
`interp_step` 是 `ncap = 3` 的顶层 letfun，被 `foldl(interp_step, …)` 当值使用）。所以
"用户函数的跨出现点相等"仍是**未做**的一步，且必须先把 `gvar` 的解析面扩到本镜像的
捕获过程（或给这类值另一条稳定的 cell 路）✓。

**仍未做**：

| # | 内容 |
|---|---|
| 1 | ~~用户 letfun 的取值仍是每次分配 ⇒ 跨出现点 `==` 仍是 `#f`~~ ⇒ **2026-09-16 已修**（`ncap == 0` 走静态 cell + `gvar` 计入剪枝引用，见 §8.6） |
| 2 | REPL 显示名字（`<fun cons>`）：cell 的 `+32` 在 `nenv = 0` 时无人读 ✓，可以放名字，但要同步改 3 条 repl 用例的 `<fun>` 期望 |
| 3 | `pkg profile` **仍是 139** ⇒ §8.1 第一条的根因**已被证伪**，待重新定位（见 §8.4 第 4 条） |

#### 8.5.1 顶层 letfun 相等：A 方案实测（**未落地**，2026-09-16）

现象：**顶层** `let f(x) = …` + `let a = f` ⇒ `a == f` 是 `#f` ✗（Chez 里 `(define a f)` /
`(eq? a f)` 是 `#t`）；**同一写法放进 `in`-链**（局部绑定）却是 `#t` ✓ —— 因为局部的 letfun
在定义点建一次闭包并绑进 Γ 槽，而顶层名字**没有那个"位置"**。

`FLAT_ABI.md` §7.2 的修法是"顶层 ⇒ `Γ[x ↦ cell(x)]` + 发 `gset` 发布"。实测下来它**不是
一处前端小改**，路上有三个坑，全部有据可查：

| # | 坑 | 证据 |
|---|---|---|
| 1 | `gvar`（**代码入口** stub）**不能**用于顶层 letfun 的通用情形 | 前端注释声称"顶层 letfun 必然 ncap == 0"（`lir.yac:1593-1596`），但 `tests/run.yac` 的 `interp_step` 被判为顶层 letfun（`sigma_kind == 1`）**且 ncap = 3** ✗ ⇒ `error: EMIT: patch to unknown proc 'interp_step'` |
| 2 | `gset` / `gval` 的名字表**只认"顶层值名"**，不认 letfun 名 | 在 letfun 定义点发 `gset` 后，三种名字形态都失败：`gname` ✗、`resolve_call(name)` ✗（这正是既有 kind==2 发布处注释指定的形态）、`sigma_code(Σ, name)` ✗ ⇒ 一律 `error: EMIT: patch to unknown proc 'slen'`（`front/lexer.yac:46` 的顶层 letfun，被当值使用） |
| 3 | Σ 的 proc 列表里 **stub 先于真记录**金 | `lir_proc_find` 按名字取到的第一条是 `lir_letfun_begin` 建的 stub（空 insns，ncap 不可信），所以"这条 letfun 是否 0 捕获"在**引用点**判不准 |

⇒ 要做 A，必须**同时**：把发射侧的 *name → cell* 表扩到 letfun 名（新能力，含前向引用语义），
并且在引用点拿到**可信的 ncap**（跳过 stub）。只改前端会在自举第二阶段就断（实测 ✓）。

**窄版（B）实测也撞同一堵墙** ✗：只对"**真记录**（insns 非空）且 ncap == 0"的顶层 letfun 用
`gvar` 静态 cell —— `src-self` 自举**通过** ✓（连 `slen` 都能解析 ✓），但**独立程序**里失败 ✗：
`let f(x) = print(x)`（顶层 letfun，0 捕获）+ `let a = f` ⇒ `error: EMIT: patch to unknown proc
'f'` ⇒ 程序自己的顶层 letfun 名**不在发射侧的 name/id 表**里。

⇒ **两项修复都在发射侧**（前端改不动）：(1) 把 *name → cell/id* 表建到"单元的顶层名字（含
letfun）"；(2) 规定这类名字的发布语义（`gset` 写一次 / `gvar` 静态 cell）与前向引用。这三条
（8.5.1 的坑 1/2/3）是同一件事的三面。

> **2026-09-16 更正（见 §8.6）**：上面把 `'f'` 归因为"名字不在发射侧的 name/id 表里"⇒ 要"把表建到
> 单元顶层名字"——**不成立** ✗。真正原因是**剪枝**：`back/lower.yac` 的 `drop_unreachable` 只沿
> `insn_callee`（`fcall` / `ycall` / `tcall` / `closure`）标记过程，不认 `gvar` / `gval` / `gset`
> ⇒ 只被 `gvar` 引用的顶层 letfun 在装配 prog 时被丢掉（实测 110 → 109、长度 = 1 的名字 2 → 0）。
> 所以只修两处就够：**引用收集加 `gvar` / `gval` / `gset`**（`defceba`）+ **前端对 `ncap == 0`
> 的顶层 letfun 用静态 cell**；name/id 表**不需要**扩（`emit_id_scan` 本来就按名字扫全表 ✓）。
> 坑 1 仍要防 ✓：实测 `interp_step` 依旧走 `[closure, 157, interp_step, [59, 61, 63]]` ✓
> —— 捕获型必须留在分配式 `closure` 上。

### 8.6 实测：`gvar` 引用的过程会在装配时被丢掉（2026-09-16）

**现象**：把顶层 letfun 的取值改成静态 cell（`gvar`，§8.5 的推广）后，
`let f(x) = … in let a = f in a == f` 编译报 `error: EMIT: patch to unknown proc 'f'`。

**读数链**（同一台编译器逐层仪表化，全部只打标量）：

| 观测点 | 过程数 | 长度 = 1 的名字数 |
|---|---|---|
| 前端 prog（`--dump-lir t5.yac`） | 3（`_start` + `f` stub + `f` 真体） | 2 |
| 后端 `tco_prog` 现场（`backend.yac:841/870`） | 110 | 2 |
| 发射器收到的 `prog[1]`（`emit_x86_64.yac:2078`） | **109** | **0** |
| 发射器 `funs`（= `prog[1]` + 10 个宿主桩） | 120 | 0 |

⇒ 发射期 id 表**每进程只建一次**（实测 `emit_ids_new` 调用数 = 1），内容是 `prog[1]` + 宿主桩
（`emit_ids_bind` `emit.yac:334`，值 = 索引 + 1）。`emit_map_get` 在 fmap miss 后会**按名字扫全表**
（`emit_id_scan` `emit.yac:220`）—— 所以"表里没有"意味着**这个名字确实不在被发射的 prog 里**，
而不是解析器不认识 letfun（先前一句"表里没有 letfun 名"说得太绝对，此处更正）。

**结论**：`f` 的 proc 记录是在**装配被发射的 prog 时**被丢的
（109 = 110 − 2×`f` + 1×`pkg/__init`；长度 = 1 的名字数 2 → 0 同时印证）。该装配只跟**调用 / 闭包**引用，
**不把 `["gvar", dst, name]` 当成对 `name` 这个过程的引用**。

**修法（2026-09-16 已落地）**：剪枝的判据是 `back/lower.yac:24` 的 `insn_callee`
（只认 `fcall` / `ycall` / `tcall` / `closure`），`drop_unreachable`（`:47`）据此收集存活过程。
新增 `insn_name_ref`：`gvar` / `gval` 取 `insn[2]`、`gset` 取 `insn[1]`，在 `scan` 里与
`insn_callee` 一起 `mark`（`gname_of` 同时接受已 intern 的 id 和字符串；`mark` 对非过程名是空操作）。
这一步与 §8.5 的"用户 letfun 跨出现点 `==`"是同一次修复。

**验收（实测）**：

| 用例 | 结果 |
|---|---|
| `let f(x) = … in let a = f in a == f` | **1** ✓（原 `0` ✗）；`if a == f then print("equal")` 打到 `equal` ✓ |
| `let a = f in let b = f in a == b` | **1** ✓ |
| `foldl(add2, 0, [1,2,3,4])`（0 捕获 letfun 传给运行期 HOF） | **10** ✓ |
| `make test-compiler` | **176 / 1** ✓（同基线，唯一失败仍是 `repl let fn after expr line`） |
| `qemu-arm64` / `qemu-riscv64` | **81 / 0** ×2 ✓（各 3 SKIP） |
| `make test` 全量 | **679 / 7** ✓（仍是那 4 个用例，无新增） |

**仍未做（同一族的前端限制）**：**把函数当值的变量再调用**（`let a = f in a(4)`）报
`error: LIR: call to undefined procedure 'a'` —— callee 只走 Γ / Σ 解析，不认"值是过程"的绑定。
（`let f = str_cat` 后 `f("ab","cd")` 同样报这条。）

**另记两条环境事实**：

| # | 事实 |
|---|---|
| 1 | **顶层绑定按源码顺序解析**：新帮手若插在首次使用**之后**，编译报 `unbound variable 'xxx'`（实测 `9717:1`）⇒ 新函数 / 新 box 必须定义在使用点之前 |
| 2 | **发射期打印会拖垮自举**：在 emit 路径里逐条 `print` 会让 stage2（编译器编自己）**段错误**（两次实测）⇒ 仪表只用标量计数，并保持极低输出量 |

### 8.7 已修：profiler 钩子的 ABI 错位（`pkg profile` 139）

**现象**：`pkg profile` rc **139**；最小复现（5 行、不需要 `profile` 包）里钩子打印出的名字是 **0**：

```yac
let prof_enter_go(name) =
    print(name)                        /* 期望 "f"，实际 0 */
let f(x) = x + 1
let _ = list_push(yac_prof_cell(), 1)  /* 让 prof 会话非空 ⇒ 钩子生效 */
let _ = f(1)
```

**根因**：运行期（`rt_prof_enter_ins` / `rt_prof_leave_ins`）用**平** ABI 调钩子（`fcall` ⇒ 第一个参数在 **rdi**），而**用户写的** `prof_enter_go` 是普通 letfun＝**对象 ABI**（第一个参数从 **rsi** 读，LIR.md 4.4.4）⇒ 名字进 rdi、钩子读 rsi ⇒ 拿到 nil ⇒ profiler 把它当字符串哈希（`yac_str_hash` 读 `[0+24]`）⇒ 段错误。旁证：ELF 里名字**已正确烘焙**（`imm=0x414679`，tagged 池对象）；`yac_prof_enter` 的序言是 `mov %rdi,-0x10(%rbp)`（平 ✓），`prof_enter_go` 是 `mov %rsi,-0x10(%rbp)`（对象 ✗）。

**两处修复**（`src-self/rt/runtime.yac`）：

| # | 改动 | 覆盖的情形 |
|---|---|---|
| 1 | `rt_funs_rename_prof` 的重定向从 `fcall` 改为 **`ycall`**（对象 ABI） | 钩子来自**运行期 base + 被链接的包**（`pkg/profile.yac`、`src-self/back/profile.yac`）⇒ `runtime_add` 能看见并重定向 |
| 2 | 运行期那条钩子调用把名字**同时放进 rdi 与 rsi**（`[1, 1]`） | 钩子**写在程序自己**里 ⇒ 不在 `runtime_add` 的列表里（它只拿到 `rt_base` + 链接的包）⇒ 调用保持平 ABI，靠 rdi 也能被对象钩子…… 反之同理 |

**测试**：

| 用例 | 断言 |
|---|---|
| `tests/compiler/cases/prof_hook_name.yac` | 输出恰为 **`f`**（最小复现固化；修前是 `0`） |
| `tests/pkg/prof_hook.yac` | rc **42**：dump 存在且含 `bump`/`work`（修前 `nfuncs=0`） |

**结果**：host `compiler` **182 / 1**、host `pkg` **21 / 1** ⇒ `pkg profile` 不再失败 ✓。剩余失败 **3 条**：`repl let fn after expr line`（12.14 jslot）、`pkg compiler`（跑法/设计缺口）、`import after use`（C 解释器那条路）。

---

# 附录：到 `LIR.md` 的索引

**本文不定义任何指令** —— 语法、形态、语义、现状与迁移**全部**以 `docs/LIR.md`
为准。需要什么，去那里看：

| 需要什么 | 位置 |
|---|---|
| 语法（`prog` / `proc` / `insn` / `operand`；`proc` 头的 `[fvs]`） | `LIR.md` §2 |
| 定位与 8 条不变量 | `LIR.md` §1 |
| KISS / OCP 准则 + 六类被泄漏进 opcode 的维度（带 `emit` 行号证据） | `LIR.md` §3 |
| **完整目标指令集**（含形态与语义） | `LIR.md` §4 |
| **名字单元 cell 布局**（32B，`nenv = 0` 即零捕获闭包） | `LIR.md` §4.5 |
| **现状 → 目标迁移表**（call 10→3、名字单元 4→3、对象内存 8→2、原生内存 4 条保留；含死代码与幽灵） | `LIR.md` §5 |
| 闭包在 LIR 的 5 处落点 | `LIR.md` §6 |
| 后端契约、patch 语言、TCO 三条路径、`fn_entry` 策略点 | `LIR.md` §7 |
| 应实现的 `--verify-lir` 校验规则 | `LIR.md` §8 |
| 缺陷清单（静默兜底 / `topfn_has` / `--dump-lir` / `$` 契约） | `LIR.md` §9 |
| 落地顺序 | `LIR.md` §10 |

> **为什么这里没有指令表。** 本附录曾抄过一份完整指令表，**已经漂移**：
> `$sp` / `$carg` 的签名与 `emit` 不符、`obj_sti` / `obj_st_int` 的源写反、
> 已合并的 `$local` / `mov_imm` / `alloc_s` 仍列成独立指令。
> 教训：**一份"以别处为准"的抄件，迟早会变成第二份真相。**
