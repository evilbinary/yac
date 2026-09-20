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
名字表只给**入口地址**，不给环境。`import yc.compiler` + `compile("1+2")`
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
| **5** | **L2 跨镜像**（§2.4）：cell 共享（jsess patch）+ `fn_entry` 策略点 | link 13 PASS、repl 26/26、`import yc.compiler` + `compile("1+2")` e2e、`blob_len > 0` |
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
| `bind_caps` / `outer_caps` / `cap_slots` | 前导 caps ABI 机制（顶层路径；嵌套闭包仍需要 `closure` 的 caps 槽位）⇒ **核实：仍在用** ✗（`lir.yac:1492` 建 env、`:1615` 构造 `closure` insn ✓）—— 本行只剩"顶层前导 caps"那半是旧的 ✓ |
| `host_env_extra`（backend） | blob 侧 G+136 槽填充 |
| `rt_host_call_ins`（`yac_host_call` 桥） | 调用点的 caps 解包 |
| `rt_gtab_get` / `xset` 表 | 名字→闭包注册表（flat 不需要传环境） |
| `via_hid` / `gtcall` emit 路径 | blob 调用的特例 → 收进 `fn_entry` |
| `yjit` clos_list / `host_tab_fill` 残留 | 首会话槽拷贝 ⇒ **核实：`clos_list` 早已不存在** ✓，但 **`host_tab_fill` 是活的** ✗（7 处 ✓）—— 它就是 §8.10 里把宿主叶地址写进**会话** G+136 槽的那一位 ✓，**不能删** ✗（本行只剩前半段是旧的 ✓）|
| `gcall` | rev1 的调用指令 |
| `pass_lir` 的 `TOPVALS` 打印、`gvst_push` 的 `GVST?` 打印 | 调试残留 |
| 第 6 步收敛掉的 opcode | `xcall` / `apply` / `ticall` / `tailapply` / `iccall` / `$icall` / `gvld` / `gvst` / `gfnst` —— **本条已结清（2026-09-17 ✓）**：`tailapply` / `ticall` / `xcall` **已删** ✓（§8.12 ✓）、**`apply` 已收进唯一动态调用形态** ✓（§8.16 三步 ✓）、`gvld` / `gvst` / `gfnst` 本来就没有 ✓；`iccall` / `$icall` 经核实**是活的** ✗（C 互操作 ✓ / 内核手写 LIR ✓）⇒ **不在删除范围** ✓ |

**逐项核实（2026-09-17 ✓，方法同 §8.12 普查：全用例库 111 个 dump + 源码扫描 ✓）**：

| 状态 | 对象 |
|---|---|
| **早已不存在** ✓（清单过期 ✓）| `host_env_extra` ✓、`rt_host_call_ins` ✓（只剩 `yac_host_call` 一条注释 ✓）、`rt_gtab_get` ✓、`xset` ✓、`via_hid` ✓、`gtcall` ✓、`clos_list` ✓、`pass_lir` 的 TOPVALS 打印 ✓、`gvst_push` 的 GVST? 打印 ✓、`gcall` ✓（已无 opcode，只剩注释 ✓）|
| **仍在用** ✗（**不能删** ✗）| `bind_caps` / `outer_caps` / `cap_slots` ✓（闭包 caps ✓）、`host_tab_fill` ✓（§8.10 ✓）|
| **已删** ✓（本轮及之前 ✓）| `tailapply` / `ticall` ✓（§8.12 ✓）、`xcall` ✓（名字已摘 ✓）|
| **待做** ✗（本清单仅剩这一项 ✓）| **`apply` 的形态收敛** ✓ —— `apply` 仍是独立 opcode ✓（编译器自身 387 处 ✓），§6 下面那段"能力不删，只改形态"说的就是它 ✓ |

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

### 8.1 剩余失败（0 条，2026-09-17 全绿）

> 本节原为 2026-09-15 的"四类失败（全量 679 / 7）"。四条已全部修完 ✓：
> `pkg profile`（§8.7 ✓）、`pkg compiler` 与 `import after use`（§8.9 ✓）、
> 最后一条 `repl let fn after expr line`（§8.8 ✓，其副作用"import 后的宿主叶调用读 0 段错误"
> 由 §8.10 的"两个基址"收尾 ✓）⇒ **全量 `make test` = 746 / 0** ✓（2026-09-18：新增 `gcroot_pub` ✓、`call_toplevel_value` ✓、`guest _eval shadows wrapper` ✓、三条内建元数用例 ✓、两条 AOT 负例 ✓ 以及三条重定义用例 ✓ —— 后五条要 harness 支持"期望 rc / 期望失败" ✓，见 §8.18 ✓）。
> 下表保留下来作"曾经红过什么"的台账。

| 用例 | 现象 | 根因 | 证据 |
|---|---|---|---|
| ~~`pkg profile`~~ | rc **139** ⇒ **已修（2026-09-16，§8.7）** | **平过程被当闭包值调用**：profiling 打开后进普通函数 ⇒ 调用点按**对象 ABI** 放参（真参从 `rsi` 起、`rdi` 不设），被调方是**平**过程（形状 = `yac_str_cat`：`len(a)`+`len(b)`），从 `rdi` 读 ⇒ `len(0)` ⇒ 段错误 | gdb 崩点 `mov 0x18(%rax),%rax`（`rax=0`）；调用点 `mov %rax,%rsi; xor %rax,%rax; mov %rax,%rdx…`；被调方 `mov %rdi,-0x10(%rbp)` + 两个 `len`。x86_64 的 ABI 表：`ycall` `emit_x86_64.yac:702-733`（对象 ABI），`tcall_other` `:172-186`，自尾调用 `:758` / 跨过程尾调用 `:760`（写死 `traw=true`）。闭包值的产生点：`front/lir.yac:846`、`:1501` |
| ~~`repl let fn after expr line`~~ | 症状随布局变（`error: not a function` / 静默无输出 / SIGSEGV 139 / SIGILL 132）⇒ **已修（2026-09-17，§8.8）**。真触发条件是"**函数定义不在会话第 1 行**"，与"前面有没有表达式行"无关 | 12.14 的 **jslot / 闭包入口发布**那半：表达式行之后才定义的函数落在会话 blob 里，其闭包入口没人从宿主注册表填 ⇒ 调用落到错入口 | `tests/run.yac:479` 的注释即为这条的常驻哨兵 |
| ~~`pkg compiler`~~ | rc **0** ⇒ **已修（2026-09-16，§8.9：修的是跑法，不是代码）** | **不是崩溃**：程序调用 `compiler` 包的**宿主叶**（`@compile_native` / `@host_arch` / `@mk_target` / `@host_format`），它们只在 **yc 自己的镜像**里有实现；被当独立可执行跑时槽是桩 ⇒ 打印 `host fn unavailable` ⇒ 返回 0 | 桩的生成点 `emit_x86_64.yac:2065-2080`（`unstub_procs`）。**结论是跑法/设计缺口**：该用例应走进程内 host 路径，而不是独立二进制 |
| ~~`import after use`~~ | `expected: 1` / `actual:`（空） ⇒ **已修（2026-09-16，§8.9）** | **`import` 不提升**（修前**两个前端都错**）：按**源码顺序**解析名字，`import rt.os` 写在用 `host_os` 的 `let` **之后** ⇒ 未绑定 | 用例 `tests/interp/import_late.yac`（`let f(_) = host_os(0)` / `import rt.os` / `if str_len(f(0)) > 0 then 1 else 0`）。`./yac --pkg src-self …` ⇒ `error: 1:12: unbound variable 'host_os'`（rc 1）；`./yc --pkg src-self …` ⇒ **`error: 1:1: unbound variable 'host_os'`** ✗ ⇒ **不是"C 解释器专有"，`yc` 同样不会提升**。把 `import rt.os` 挪到第 1 行 ⇒ **两者都打 `1`** ✓（`./yac` rc 0 / `yc` 编译并运行 rc 0 ✓）。**修法（已落地）**：两个前端各自做**顶层 `import` 的稳定提升** —— `yc` 在 `rewrite_imports`（`back/backend.yac`，`report_unbound_ex` 的预检与 `link_from_ast` 都经它 ⇒ 一处改动同时覆盖检查与链接）；C 解释器在 `src/parser.c` 把 import 子项插到"**前排 import 游标**"而不是"import 出现处"（imports 本来在前时 `memmove` 长度为 0 ⇒ 与原来的 append 逐字节等价）。验收见 §8.9 |

> **规范结论（值得写进实现约束）**：**闭包值的调用目标必须是对象 ABI（标记帧）**。
> `$proc` 这类**平**过程被当成值（`closure`）去 `apply` / `icall` 时，必须在中间套一层
> 对象 ABI 的 **thunk**（丢弃对象、把真参从 arg1 搬到 arg0），或者干脆不允许这种取值。
> 缺这一层时不会编译报错，只会静默错位 —— `pkg profile` 就是它的一次实爆。

### 8.2 潜伏缺口（现在不失败，踩到才炸）

| # | 位置 | 内容 |
|---|---|---|
| ~~1~~ | ~~`emit_arm64.yac:1475`、`emit_riscv64.yac:1480`~~ ⇒ **不是缺口：已退休的 opcode**（§6 清理清单第 288 行 ✓，详见 §8.12） | ~~`tailapply` / `ticall` 带**栈参数**（>7）时发 `brk` / `unimp` ⇒ 未实现~~ ⇒ **这两个 opcode 第 6 步就收敛掉了** ✓：唯一构造点 `lir.yac:211/215` **永不触发** ✓（`tco_find` 只认自调用 ✓），大样本 LIR 里 **0 处** ✓（§8.12 的普查表 ✓）。⇒ 三后端的 case 是**死代码** ✓，本项从"待实现"改判为"待删除"（§6） |
| ~~2~~ | ~~`emit_x86_64.yac:754` vs `emit_arm64.yac:685` / `emit_riscv64.yac:671`~~ **已修（2026-09-17，§8.11）** | 跨过程尾调用的目标 ABI：x86_64 **写死 `traw=true`**（当作平目标）✗，arm64 / riscv64 按**名字前缀**判（`is_yac_proc_name`）✗ —— 两者都是近似：真判据是目标首条 `local`/`$local` 的**第 4 栏**（`selfabi`）✓。三个后端已统一到 `emit.yac` 的 `tcall_raw_of` ✓ |
| 3 | 提交 `943d438` | 提交信息写 "correct riscv64 ycall param register offset"，实际内容是**回退 + 改注释**（`10 + j` 是对的，注释错了）⇒ 信息与内容不符 |
| 4 | 仓库根的 `./yc`（无扩展名） | 早先记的是"`./yc` 会抢先命中陈旧的无扩展名文件"。**2026-09-16 更正**：实测本机 `yc` 与 `yc.exe`（以及 `yac` / `yac.exe`）是**同一 inode**（`ls -i` 同号 ⇒ 硬链接/同一文件），`cp -f yc.exe yc` 会报 "are the same file" ⇒ 该现象在本机不成立 ✗（可能是别的机器/文件系统上的经历）。仍要记住的是**构建方向**：`make` 只写 `yc.exe` / `yac.exe` ✓，`./yc` / `./yac` 能跑就不能证明它们是新二进制以外的什么 |
| 5 | `build/` | 遗留 `patch_rv.py` 与 `emit_*.bak*` / `run.yac.bak` / `Makefile.bak` 等备份；无害，但别当源码读 |
| 6 | `qemu-*` 组 | `--shared ccall add` 在**转发 shim**下按设计 SKIP（每组 3 个）：shim 只上传可执行文件，`.so` 会被远端当 Windows 路径找。该用例只在**真 qemu** 环境回归 |
| ~~7~~ | ~~前端~~ ⇒ **已修（2026-09-17，§8.14）** | **顶层 `let` 绑定的值不能当函数调用** ✗：`let f(x) = print(x)` + `let a = f` + `a("x")` ⇒ `error: LIR: call to undefined procedure 'a'`（`calli` 只认 Σ 里的**过程**与 Γ 里的局部槽 ✓，而顶层值 **两者都不在** ✓ —— 见 `lir_var` 的 `kind == 2` ✓）。是响亮报错而非静默 ✓，但与"值是一等函数"不一致 ✓。**修**：`calli` 增一分支 ✓ —— `kind == 2` 时把 cell 读进临时槽（`gval` ✓）再发**动态调用** `icall` ✓（对象 ABI ✓ = §8.1 规范结论 ✓）；新用例 `call_toplevel_value` ✓ 三架构 PASS ✓ |

| ~~8~~ | ~~`rt/runtime.yac` 的 profiler 重定向~~ ⇒ **已修（2026-09-17，§8.15）** | **钩子的 ABI 归属只做到"兼容"** ✗：`rt_funs_rename_prof` 只在**运行期列表**（rt base + 被链接的包）里找钩子 ✓，而它跑在**链接期** ⇒ 程序**自带**的 `prof_enter_go` 还没出现 ✗ ⇒ 那条调用仍是平 ABI ✓，靠"名字**同放 rdi 与 rsi**"兜 ✓（§8.7 修法 2 ✓）。⇒ 一旦钩子**多参**、或哪天只留一个寄存器，就会再错位 ✗。**修**：新增 `emit.yac` 的 `prof_hook_fix` ✓ —— 在**整个 proc 列表已知**时（发射前 ✓）按**目标帧 ABI** 决定 `fcall`/`ycall` ✓（与 `tcall_raw_of` 同一条规则 ✓），并撤掉那个双寄存器 hack ✓（实参表 `[1, 1]` → `[1]` ✓）|
| ~~9~~ | ~~`build/patch_funoffs.py`~~ / `fun_off` ⇒ **生产路径已修（2026-09-17，§8.17）**；**工具那半改成按下标配对** ✓ 但**仍不能验** ✗ | 按**过程名**匹配 `(名, 偏移)` ✗ ⇒ **不只是工具小疵** ✗：`backend.yac` 的 `fun_off(fo,"_eval")` 在 **JIT 入口**的生产路径上 ✓，REPL 里一行 `let _eval(x) = x + 1` 就能让**客体的 `_eval` 顶掉包装器** ⇒ **SIGSEGV** ✗（rc=139 ✓）。根因：`funOffsRev` 是**逆发射序** ✓，名字扫描从**表头**（= 最后发射的那个）开始 ✗。修法：**按顺序**取 —— 入口恒为 `funs[0]` ✓，即该表**最后一个**记录 ✓（`entry_off(fo)` ✓）。**工具那半没修** ✗：把它改成按下标配对**仍然崩** ✗，实测表明崩因与配对**无关** ✓（见 §8.17 末）|
| 10 | `jit.yac` / REPL 的调试面 | ~~`--dump-lir` 配 `--repl` **什么都不打**、`--dump-asm` 配 `--repl` 只 dump **第一行**~~ ⇒ **2026-09-17 已修**（§8.8 ✓）：两个开关都**逐行生效** ✓（`parse_args` 补记 spec 标志 ✓ + REPL 路径逐行 re-arm ✓ + 逐行 LIR dump ✓），asm dump 另加 `=== gref cells` / `=== tag22 cells` 两张表 ✓。仍要注意：jit 路径的 `log` 被 **hush**（发射期打印看不见 ✓）⇒ 发射期内只能用 `print` 或盒子 ✓。**批处理侧另有一处** ✓：`--dump-lir` 对**带包导入**的程序原本**不出 dump** ✗（`dump_lir` 没设 `link_need_box`/`link_local_box` ⇒ `rt_for_link` 链不到包 ⇒ 只吐 `error: LIR: call to undefined procedure 'host_arch'` ✗）⇒ **2026-09-17 已修**（§8.12 ✓）：先 `link_from_ast(ast)` ✓，现在编自身 bundle 能出 **2.6 MB** LIR ✓ |

| 11 | 本机 PATH | **没有 C 编译器**：`gcc` / `cc` / `clang` / `tcc` 全不在 PATH，`where.exe gcc` 也找不到；但 `/mingw64/bin/gcc.exe`（15.2.0）与 `/mingw32/bin/gcc.exe`（16.1.0）**存在**。⇒ 在**裸**的当前 shell 里 `gcc` 起不来（连 `-E` 都 rc=1：驱动 spawn 不了 `cc1` ✗），而 `make` 的隐式 `CC` 默认值就是 `cc` ⇒ `make test-*` 一旦需要重建 `$(BIN)`（`src/*.c` 比 `build/*.o` 新就会）**整组报错** ✗。可用的建法：走 MSYS2 MINGW64 环境再显式给编译器 —— `MSYSTEM=MINGW64 CHERE_INVOKING=1 MSYS2_PATH_TYPE=inherit /e/soft/msys2/usr/bin/bash.exe --login -i -c 'cd /e/workspace/yac && make CC=gcc yac.exe'`（实测可编、可链接 ✓）。**gdb 也在那里** ✓：`/mingw64/bin/gdb.exe` ✓（同样不在 PATH ✓，取陷阱现场要用绝对路径 ✓ —— §8.2 #15 就是这么拿到 RIP 的 ✓） |
| 12 | `make` 的 `$(YC_A)` 规则 | 两趟自举（`yc_a.exe` → `.new` → `.new2` → `mv`）**不能并行跑**：同时开两个 `make test-*`（各自都要重建 `$(YC_A)`）会撞在一起，第二趟产物缺失 ⇒ `mv: cannot stat 'build/yc_tmp/yc_a.exe.new2'` ✗（实测一次）。而且配方里 `echo pass 2` 前是 `;` ⇒ 第二趟失败后 `mv` 仍会跑 ⇒ 报错位置具有误导性。**串行跑 `make`** ✓。**2026-09-17 又踩一次**（症状不同 ✓）：一条 `make test` 因超时被切断 ✓ 但**仍在后台跑** ✓，此时又起一条 ⇒ 日志开头出现 **NUL 字节** + 3 条假失败 `FAIL: compiler capture_2args / ncap12_disp8 / capture_shadow_t`（`actual: compile rc=1`）✗ —— 用例本身没问题 ✓，等残留进程结束后单独重跑 ⇒ **0 FAIL** ✓。⇒ 跑测试前先确认没有正在跑的 `make` ✓。**2026-09-17 第三次**（新知识 ✓）：**被取消/超时的 `make test` 会把进程留在后台** ✗ —— `ps -W | grep -E 'make\.exe|run_tests'` 一次就能看到好几条（本次见到 13:26 起的一条 ✗）✓；清理：Windows PID 用 `taskkill //F //PID <pid>` ✓、MSYS PID 用 `kill -9 <pid>` ✓，清完再跑 ✓，一次就 **0 FAIL** ✓ |
| ~~13~~ | ~~`emit_arm64.yac:1336` / `emit_riscv64.yac:1338`~~ ⇒ **已修（2026-09-17，§8.13）** | **GC 根发布只在 x86_64 做了** ✗：`gset` 用**非立即数**源时，x86_64 会存完后调 `yac_gval_pub(name, value)` ✓（`emit_x86_64.yac:1373` ✓），而 arm64 / riscv64 只做存储 ✓（注释自己写着 "GC publish **TODO**" ✗）⇒ 运行期赋值的顶层值 cell 不是 GC 根 ✗。**修**：两架构各补上同样的发布调用 ✓；新用例 `tests/compiler/cases/gcroot_pub.yac` ✓ 在三架构都 PASS ✓ |
| ~~14~~ | ~~`emit_arm64.yac` 的 `$ld64` / `$st64`~~ ⇒ **已修（2026-09-17，§8.13）**，**修 #13 时才发现** ✗ | arm64 的 `$ld64`/`$st64` 用 `ldur`/`stur` **直接编码偏移** ✗，而它们只有 **±256** 的 9 位空间 ✓；G 区要到 **440/448**（注册表根 ✓）⇒ 越界 ⇒ 读写错地址 ⇒ `yac_gval_pub` / `yac_gval_list` 在 arm64 **必崩** ✓（`$ld8` 一直懂得先 `add` ✓，64 位版漏了 ✗）。**修**：大偏移先折进地址寄存器再 `ldur`/`stur` ✓（与 `$ld8` 同形 ✓）|

| ~~15~~ | ~~REPL **同名函数重定义**~~ ⇒ **已修（2026-09-18，§8.19）** | `let f(x) = 1` → `let f(x) = 2` → `f(0)` ⇒ **SIGILL（rc=132）** ✗（2026-09-17 实测 ✓）。**触发面已收窄到一点** ✓（判别矩阵 ✓）：**值**重定义（`let a=1` → `let a=2` → `a`）正常 ✓ rc=0 ✓；**换名字的第二个函数 blob**（`let f` → `let g` → `g(0)`）正常 ✓ rc=0 ✓；**只有同名函数重定义** ✗ 崩 ✓。已知证据 ✓：两次提交的 `yac_jslot_set` 与调用的 `yac_jslot_get` 用的是**同一槽 id（0）** ✓ ⇒ 不是槽号 ✗；而 `--dump-asm` 的 gref 表显示 blob 1 的 `cell f entry=JIT_VADDR+52776` ✓（正是它的 `f @52776` ✓）**正确** ✓，blob 2 的同一单元 **`entry=0`** ✗ ⇒ 重定义那份**从未烘焙进单元** ✓；同一 dump 里 blob 2 的逐过程偏移还是**垃圾** ✗（`f @l (6432 bytes)` ✗、`yac_host_unimpl @5` ✗，blob 1 是 `@36081` ✓）⇒ 该 blob 的**偏移登记表本身坏了** ✗。陷阱现场（`/mingw64/bin/gdb.exe` ✓）RIP = `JIT_VADDR+60031` ✗，JIT 区内**非代码**字节 ✓、`bt` 无帧 ✓。**根因已确认** ✓（2026-09-18）：发射循环用 `emit_jsess_skip(name)`（`emit.yac` ✓）**只按名字**查会话出口表 ✓ ⇒ 第二行 `let f(x) = 2` 时 `f` 已在表里 ⇒ **整个 `f` 被跳过** ✗ ⇒ 该 blob 的 `funOffsRev` 里没有 `f` ⇒ `fill_gref` 的 `find_gref` 得 -1 ⇒ 走上文的回退**绑到 `yac_host_unimpl` 桩** ✗ ⇒ 下一行一调 ⇒ **SIGILL** ✓（RIP = `JIT_VADDR+60031` ✓；dump 里的垃圾偏移 `f @l (6432 bytes)` ✓ 正是"被跳过"时 `offs` 存的 `skipat - T` ✗）。**旁证** ✓：重定义后**只求值不调用** ⇒ rc=0 ✓ 打印 `<fun>` ✓ ⇒ 定义阶段没问题 ✓，坏的只是**烘进单元的入口** ✗。**试过并撤回的修法** ✗：把跳过限制为"本镜像自带之外的（运行时/unstub）过程"（按下标 ✓）⇒ **反而更糟** ✗：连原本正常的"换名第二条"也崩 ✓ ⇒ 重发 guest 过程会破坏该 blob ✗，不是放开跳过就完事 ✗。**下一步方向** ✓（未做 ✗）：只对**本次提交自己定义的名字**失效出口表项 ✓（jit 层知道本行绑定了谁 ✓）⇒ 新 blob 才会重发那份定义 ✓；另外 `fill_gref` 把 **guest 名字**回退成宿主桩这件事本身也该改成响亮报错 ✗。**未修** ✓ |

| ~~17~~ | ~~`front/lir.yac` 的 `lir_rt_*` 内建表~~ ⇒ **已修（2026-09-17，§8.18）** | **内建原语的参数个数不匹配 ⇒ 段错误** ✗：`exit()`（用户报的 ✓）、`str_cat()`、`str_ref(1)`、`str_len()`、`read_file()`、`write_file("x")`、`system()` —— 实测 7 个里 **6 个 SIGSEGV** ✗（`gc_collect(1)` 只因忽略参数才没事 ✓）。根因：表里直接 `nth(ss, k)` ✗，越界得 `[]` ✓，而发射器把 `[]` 当**槽号** ✗ ⇒ 读垃圾地址 ✓。修法：改为带守卫的 `rt_arg(g, ss, k)` ✓（§8.18 ✓）|
| 16 | REPL 里的 **LIR 致命错** | `log_fatal` 会**结束整个会话**（rc=2 ✓），而前端错 / 语法错只报错**继续** ✓（rc=0 ✓）。**既有行为** ✓，与本次修法无关 ✓；`exit()` 只是撞上它 ✓（`exit(3)` 是真退出 ✓ rc=3 ✓；要离开 REPL 用 `:q` 或 `exit(0)` ✓）。**未修** ✗（要让它可恢复得给 `log_fatal` 加 REPL 的长跳 ✗，属结构改动 ✗）|

| ~~18~~ **已修（2026-09-18 ✓，见本节末）** | **调用非函数值**（`let f = 1` → `f()`）| **SIGSEGV / SIGILL，而不是干净报错** ✗（用户报的 ✓，2026-09-18）。**已复现** ✓：REPL 两行版 **139** ✗、AOT 同名写法 **139** ✗；单行 `let f = 1 in f()` ⇒ **132** ✗；而 `let f = 200000 in f()` / `let f = "s" in f()` ⇒ **132** ✓（守卫**按设计**拦住了 ✓）、`let f(x)=x in f(7)` ⇒ 7 ✓。**已排除** ✓：不是"值存坏了" ✗ —— `let f = 1` 之后 `f` / `print(f)` / `let g = f` 都得到 **1** ✓ ⇒ jslot 里的值**是对的** ✓。**现场** ✓：两行版 RIP = `JIT_VADDR+58874` ✓ 而第二 blob 的 T≈58860 ⇒ 即 **blob 2 内偏移 ≈ 14** ✗（应在 `bad` 标签 ✓）；`rdi = 2`（= 被拒的 tagged 1 ✓）⇒ 守卫**确实拒绝了** ✓，只是从**拒绝路径**跳错了地方 ✗；单行版 RIP = `JIT_VADDR+120` ✓（在 `_eval` 内 ✓）⇒ **132** ✗。AOT 侧 RIP = `0x400368`（139 ✗）/ `0x400371`（132 ✗）✓（基址/`TEXT_OFF` 未知 ⇒ 暂未对到过程名 ✗）。⇒ **字节级证据** ✓（2026-09-18 追加）。**已证伪我自己的第一条假设** ✗（如实记下 ✓）：本以为"守卫失败出口的补丁目标没解析对" ✗，于是插了探针（`emit_patch_rel` 记录名字→id ✓、`emit_resolve_patch` 的 tag 1 记录 id→`offs[id]` ✓，都写文件 ✓，用完已撤 ✓）⇒ 实测 `yac_not_fn` **id=52 ✓、target=38377 ✓、nfun=120 ✓**，而**同一份程序**的 dump 里第 **52** 个过程正是 `yac_not_fn @38377` ✓ ⇒ **id 与目标都对** ✓ ⇒ 崩**不在**这条 `call` 上 ✗。已确认的其余事实 ✓：`--dump-asm` 的 hex 可直译守卫序列 ✓（`mov rbx,0x20000 / cmp rax,rbx / jb bad / mov r11d,[rax+0x10] / … / bad: mov rsi,rax; call` ✓）；三处现场 RIP ✓（REPL 两行 `JIT_VADDR+58874` ✓、REPL 单行 `JIT_VADDR+120` ✓、AOT `0x400368` / `0x400371` ✓）都**落在非过程起点** ✓，且按 `RIP = code_base + offs[id] + 1`（`apply_patch` 写的是 PC 相对位移 ✓）反推得到的 `offs[id]`（14 / 872 ✗）与探针实测（38377 ✓）**不一致** ✗ ⇒ 说明**我对 `code_base` 的换算仍不可靠** ✗（JIT 区里的那些字节读出来是**指针** ✓ = 数据区 ✓），所以"跳到哪/为什么"**还不能定论** ✗。⇒ **当前最可信的说法** ✓：崩溃发生在**守卫自身**这条路上 ✗（它第一道小值检查在运行中**没有生效** ✗），而 `yac_not_fn` 那条 call 是**无辜的** ✓。**未修** ✗。**统一线索** ✓（2026-09-18 追加）：gdb 在 RIP 处反汇编得到 `mov 0x10(%rax),%rbx`（闭包取 fnptr ✓）且 **`rax = 0`** ✗ ⇒ 是**编译器自己 apply 到一个 nil 闭包** ✓；而"往 `emit_program_x86_64` 注入一个**局部函数**"（调试工具 `build/patch_funoffs.py` ✓）会**独立复现同一现场** ✗（含局部函数的程序一律 rc=139 ✓，只写常量的注入却正常 ✓）⇒ **#9 工具那半与 #18 是同一族** ✓：emit 路径里的**局部闭包会取到 nil** ✗。⇒ 顺序应是**先修 #18 ✓**，工具才有意义 ✓。**已修（2026-09-18 ✓）**：根因在 `emit_x86_64.yac` 的 `emit_call_guard` —— 它把 `acall = bytes_len(b)` 取在 **`mov_rax_to_argreg(b, 1)` 之前** ✗，而那个 `mov rsi,rax` 占 3 字节 ⇒ 链接期按 `acall + 1` 写 rel32 ⇒ **rel32 落进 `mov rsi,rax` 里 ✗、`call` 自己的字段留成 0** ✗ ⇒ 拒绝路径执行垃圾 ✗（实测拒绝块字节 `48 d0 94 00 00 00 00 00` ✗，应为 `48 89 c6 e8 …` ✓）⇒ rel32 值随布局变 ⇒ 同一缺陷表现为 **SIGSEGV**（多数形状 ✗）或**碰巧 `ud2` ⇒ SIGILL 132**（`print(f())` ✗）⇒ 这就是"形状相关"的真相 ✓。修法：`acall` 挪到**紧贴 `call_user`** ✓（与同级 4 处惯例一致 ✓，`emit_x86_i_clos:1627` ✓）⇒ 五种形状全部 **rc=0 + `error: not a function: …`** ✓。**两条后续** ✗：(a) **arm64 / riscv64 根本没有这个守卫** ✗（`yac_not_fn` 只在 `emit_x86_64` 出现 ✓）⇒ 那两个后端上"调用非函数值"仍是未定义行为 ✗；(b) 报错里的**值**：REPL 打 `1` ✓、AOT 打 `0` ✗（守卫传的是已被 `and_rax_1` 去掉 tag 的 `rax` ✗）⇒ 消息有余量可改 ✓（不影响语义 ✓）|

> ~~**工作树里一处未决**~~ ⇒ **已定** ✓（2026-09-17）：`src-self/back/jit.yac:76` 那条 **hushed 追踪**
> （`log("jit", …)` ✓ = 每个 gref 名字 + 有没有 gfn 项 ✓）**已随提交进入 HEAD** ✓ ⇒ 当作"**留作 `--verbose` 开关**" ✓，
> 不再算残留 ✓。它只在 `--verbose` 下打印 ✓（非 verbose 零输出 ✓），是排查 blob 里"cell 没填上"的第一手工具 ✓。

### 8.3 当时的基线（改动验收对照）

| 组 | 结果 |
|---|---|
| host `compiler` | 176 / 1 |
| host `interp` | 35 / 1 |
| host `pkg` | 19 / 2 |
| `qemu-arm64` | **81 / 0**（3 SKIP） |
| `qemu-riscv64` | **81 / 0**（3 SKIP） |
| 全量 `make test` | **679 / 7** |

> **2026-09-18 现状**（全量实跑 ✓）：host `compiler` **196 / 0** ✓、host `pkg` **22 / 0** ✓、
> host `interp` **36 / 0** ✓、`qemu-arm64` **85 / 0** ✓、`qemu-riscv64` **85 / 0** ✓（各 3 SKIP ✓）、
> 全量 `make test` **746 / 0** ✓ —— **全绿** ✓（新增 `call a non-function value` ✓：非函数值调用必须报错并继续 ✓）（2026-09-18 ✓：`tailapply`/`ticall` 退役 opcode 清理 ✓、`gset` 的 GC 根发布补到 arm64/riscv64 ✓、arm64 G 区大偏移修复 ✓、顶层 `let` 的值可当函数调用 ✓、REPL 入口改按**发射顺序**定位 ✓（§8.17 ✓）、内建原语的元数检查 ✓（§8.18 ✓）、**REPL 同名重定义不再挂掉**且给出覆盖告警 ✓（§8.19 ✓）、harness 支持"期望 rc"的负例 ✓（repl 组 ✓ + compiler 组 ✓））。
> 相对上表的增量来自新增用例：`fun_eq` / `print_dotted` / `prof_hook_name` / `import_late`（compiler）
> + `pkg prof_hook`（pkg）；`pkg profile`、`pkg compiler`、`import after use`、`repl let fn after expr line`
> 也从此表里的失败逐条转绿 ✓（分别见 §8.7 / §8.9 / §8.9 / §8.8）。

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
| 1 | `rt_funs_rename_prof` 的重定向从 `fcall` 改为 **`ycall`**（对象 ABI） | 钩子来自**运行期 base + 被链接的包**（`pkg/yc/profile.yac`、`src-self/back/profile.yac`）⇒ `runtime_add` 能看见并重定向 |
| 2 | 运行期那条钩子调用把名字**同时放进 rdi 与 rsi**（`[1, 1]`） | 钩子**写在程序自己**里 ⇒ 不在 `runtime_add` 的列表里（它只拿到 `rt_base` + 链接的包）⇒ 调用保持平 ABI，靠 rdi 也能被对象钩子…… 反之同理 |

**测试**：

| 用例 | 断言 |
|---|---|
| `tests/compiler/cases/prof_hook_name.yac` | 输出恰为 **`f`**（最小复现固化；修前是 `0`） |
| `tests/pkg/prof_hook.yac` | rc **42**：dump 存在且含 `bump`/`work`（修前 `nfuncs=0`） |

**结果**：host `compiler` **182 / 1**、host `pkg` **21 / 1** ⇒ `pkg profile` 不再失败 ✓。剩余失败 **3 条**：`repl let fn after expr line`（12.14 jslot）、`pkg compiler`（跑法/设计缺口）、`import after use`（C 解释器那条路）。

### 8.8 已修：REPL 跨行调用 —— `dest > 0` 的 globals 基址没带 append 偏移（2026-09-16 立项，2026-09-17 修）

**根因**（一句话 ✓）：`emit_x86_64` 给解析状态的 `GLOBALS_BASE` 在 blob 上取的是**会话第一张镜像**的
data 基址（`nth(js, 1)` ✗），而 `fill_gref` / `bake` 把 stub cell 写在**本镜像**的 data 区 ✓，
`emit_apply_unres(dest)` 又只给部分栏加 append 偏移 ✗ ⇒ **第 2 行起**的 `gvar` cell 地址（tag 22）
比实际 cell 低 `59044 − 56376 = 2668` ✗ ⇒ 会话槽里存的是**别的 cell 的地址** ✗ ⇒ 下一行 `icall`
读 `[错地址+16]` ⇒ 跳进垃圾 ⇒ 症状随布局变：`error: not a function` / 静默无输出 / SIGSEGV 139 / SIGILL 132 ✓。

**决定性读数** ✓（`--dump-asm --repl` 新加的两张表 ✓，定义行的镜像）：

| 表 | 读数 | 判读 |
|---|---|---|
| `=== gref cells` | `cell f off=1208 val=0 entry=8589993232` | ✓ 入口 = `JIT_VADDR + dest(58292) + off(348)` ✓ **是对的** ✓ |
| `=== tag22 cells` | `globbase=8589990968 goff=456 cell=8589991424` | ✗ `globbase − JIT_VADDR = 56376` = **第 1 张镜像**的 `data_start` ✗（应为 `dest + data_start = 59044` ✓）|

**修法**（两处 ✓）：

1. `src-self/back/emit/emit_x86_64.yac`：`GLOBALS_BASE = LOAD_VADDR + TEXT_OFF + data_start`（去掉
   `if T != 0 then nth(js, 1)` 那个"会话基址"特例 ✓ ⇒ 与 `fill_gref` / `bake` 的落点**同源** ✓）。
2. `src-self/back/lower.yac` 的 `emit_apply_unres(off)`：按架构把 **globals 栏**也加上 `off` ✓ ——
   x86_64 加第 2/3/4 栏 ✓，arm64 / riscv64 加第 2/3/5 栏 ✓（它们第 4 栏是 **labels 列表**，不能碰 ✓）。
   `off == 0`（AOT、以及会话第 1 行 ✓）行为不变 ✓。

**顺带修好的调试工具** ✓：`--dump-lir` / `--dump-asm` 在 `--repl` 下现在**逐行生效** ——
原先前者**完全不生效** ✗、后者只 dump 会话**第一张**镜像 ✗（§8.2 #10 ✓）；asm dump 另加
`=== gref cells`（名字 / cell 偏移 / `+0` 值 / **`+16` 入口** ✓）与 `=== tag22 cells`
（解析后的 cell 绝对地址 ✓）两张表 ✓ —— 这两个数就是这次定位的钥匙 ✓。

**验收** ✓：全量 `make test` **716 / 0** ✓（原 712/2 ✓）；`compiler` **185 / 0** ✓、`pkg` 22/0 ✓、
`interp` 36/0 ✓、`qemu-arm64` / `qemu-riscv64` **85 / 0** ×2 ✓；REPL 花样：定义在第 2、第 3 行 ✓、
两个跨行定义互相调用 ✓、跨行 `a == f` = **1** ✓、`foldl(f, 0, [1,2,3])` = **3** ✓。

---

### 8.8.1 立项时的过程记录（2026-09-16，保留备查）

**复现矩阵**（`./yc --repl`，每行一条）：

| 行序 | 结果 |
|---|---|
| 定义 → 调用（`let f(x) = x + 1` / `print(f(2))`） | **3** ✓ |
| **表达式 → 定义 → 调用**（`1+1` / `let f(x) = x + 1` / `print(f(2))`） | **`error: not a function0`** ✗ |
| 表达式 → 表达式 → 定义 → 调用 | 同样 ✗ |
| 定义 → 表达式 → 调用 | **3** ✓ |
| 定义 → 定义 → 调用 | **3** ✓ |

⇒ 触发条件只有一个：**定义行之前出现过表达式行** ✓（不是行号、也不是前面有几行 ✓）。

**仪表读数**（`jit.yac` 临时打印，已撤除 ✓）：两种情形里**发布完全一样** ✓ ——
`bind=[f]`、`is_fun=y`、`kind=gvar`、`jslot ix=0` ✓；唯一差别是**定义所在的 blob 落点**：

| 情形 | `dest` | `live_len` |
|---|---|---|
| A（定义在第 1 行 ✓） | `0` | `0` |
| B（定义在第 2 行 ✗） | `58196` | `58196` |

**当前线索** ✓：定义行用 `["gvar", slot, name]` 造 stub 闭包、由 `yac_jslot_set` 存进会话槽 ✓；下一行通过 `yac_jslot_get` 取回它、`icall` 从 `+16` 解出**入口** ✓。A 里 blob 在 `dest=0` ✓ 一切正常；B 里 blob 被**附加**到 `dest=58196` ✓ ⇒ 怀疑这条附加路径上"cell 的绝对地址 / 入口字段"没有完整带上 `dest` ✗（与 `emit_x86_64.yac:2308` 注释同一族："a blob's entry is absolute, so it must include T" ✓；`emit_apply_unres` 对 x86_64 已同时调代码段与数据段基址 ✓，所以还要看 tag 22/23 与 `gref_base()` 的合成 ✓）。

**下一(已做的)实验 — dump 观察** ✓：

| 手段 | 结果 |
|---|---|
| `--dump-lir` 配 `--repl` | **什么都不打** ✗ —— 该开关只在批处理路径生效（`run_cli` 的 else 分支 ✓），jit 路径直接把该行的 prog 交给 `emit_program_x86_64` ✓ |
| `--dump-asm` 配 `--repl` | **只 dump 第一行**的镜像 ✗（实测 `_eval @0 (118 bytes)` = `1+1` ✓），后续行没有段头 ✗ |
| A 的定义行 `_eval`（256 B ✓） | 7 个 `movabs`（除 1 个 `0x2` = 带标签的 1 ✓ 全是**补丁占位** ✓）+ 3 个 `call`（`yac_jslot_set` + 两次 `print` ✓ = `start_eval_ins` 的形状 ✓）⇒ **形状完全正确** ✓ |

**决定性发现（宿主侧探针，已撤除 ✓）** —— 定义行跑完后从宿主读会话槽：

| 情形 | `idx` | `dest` | `fun_off(fo,"f")` | **会话槽里的值** |
|---|---|---|---|---|
| A（定义在第 1 行 ✓） | 0 | 0 | 52706 | （该探针后续打印未出现 ✗，但程序正常 ✓） |
| B（表达式行在前 ✗） | 0 | **58196** | **348** | **0** ✗ |

⇒ **B 里定义行写进会话槽的是 `0`** ✗（不是闭包 ✓）⇒ 下一行 `yac_jslot_get` 取回 0 ⇒ `f(2)` 应用 0 ⇒
`error: not a function` ✓✓ —— 与 `start_eval_ins` 注释里记的旧故障**完全一致** ✓（注释说 `gval` 会读到 0 ✗、
改用 `gvar` 修好 ✓；**B 里 `gvar` 同样读到 0** ✗）。
另外两边 `fun_off(fo,"f")` 相差极大（52706 ✗ vs 348 ✗）⇒ 下一步要确认本行 blob 的偏移表把 `f` 匹配成了哪个过程 ✓
（怀疑附加 blob 里"名字 → 偏移 / cell"的对应错了 ✓）。

⇒ 两个 dump 都**看不到问题** ✗：`--dump-asm` 是**打补丁前**的字节 ✓，而本 bug 出在**解析后的地址**上（附加 blob 的 `dest` ✓）。所以下一步不再靠 dump ✓，而是：定义行跑完后在**宿主侧**读会话槽的值、打印其 `+16`（入口字段 ✓），A/B 对比 ✓（或给 `emit_resolve_loop` 的 tag 22/23 加一次性打印 ✓）。

**2026-09-16 第二轮读数（全部为临时探针，已撤除 ✓；三组基线复核 183/1、22/0、36/0 ✓）**

复现矩阵修正 ✓：**触发条件是"函数定义不在会话第 1 行"** ✗，而不是"定义行之前有表达式行" ✗ ——
`let f(x)=x+1` / `1+1` / `print(f(2))` 里表达式行在定义**之后**也照样 `3` ✓，只有定义落在**第 2 行及以后**才坏 ✗
（定义行的 blob 被**附加**到 `dest > 0` ✓）。

| # | 探针（宿主侧生成，只打标量／单个值 ✓） | 读数 |
|---|---|---|
| 1 | `jit.yac` 每行读 `emit_jit_blob`/`emit_jsess` | 每行都是 `blob=1` ✓；`T`（会话长度）在**第 1 行 = 0** ✓、定义在第 2 行 = `58196` ✓、**第 3 行仍是 `58196` ✗ 而它的 `dest = 59740`** ✗ |
| 2 | 定义行的 `_eval` 里**写槽后立刻读回**（`yac_jslot_set` → `yac_jslot_get`） | `DBG-SETGET ix=<fun>` ✓ ⇒ 写读往返**正常** ✓（A、B 都一样 ✓）⇒ "写进槽的是 0" ✗ 这条旧结论**不成立** ✗ |
| 3 | `emit_x86_64.yac:fill_gref` 记录 `[name, off, entry, T]` | B 的定义行：`nm=f off=567 v=8589993355 T=58196` ✓（`v = JIT_VADDR + 58196 + 567` ✓ ⇒ **入口值算对了** ✓）；调用行同样 `off=567 T=58196` ✓ |
| 4 | 调用点（`jslot_rw_expr` 的取值处）打印 `yac_jslot_get` 的值 | 打印为 `<fun>` ✓（`runtime.yac` 的 `pv_oth` 兜底：**任何认不出的对象都印 `<fun>`** ✗ ⇒ 它证明不了这是个合法闭包 ✗） |
| 5 | 被调用的**返回值** | `4294995852` ✗ —— = `JIT_VADDR/2 + 28556`，即一个**地址型**结果 ✗，不是 `2+1` ✓ ⇒ 说明**执行到的不是 `f` 的 `x+1` 代码** ✗ |

**结论（收窄）**：会话槽、槽索引、入口值三者都"看起来对" ✓，但**实际跳过去的代码不是 `f`** ✗
（第 5 行读数：返回地址而不是 3 ✓；把守卫的"不是函数"分支改指到另一个桩后，同一会话直接 **SIGILL** ✓ ⇒
目标确实是错代码 ✓）。⇒ 问题在**附加 blob 的"名字 → 入口/单元"对应**这一层，而不是"槽里是 0" ✗。
两个已定位的不对称 ✓：

1. **AOT 走 id 表、blob 走名字扫描** ✗：AOT 的入口由 tag 23 = `codebase + offs[fid]` 烘焙 ✓，
   `fid = emit_map_get(st[4], nm)`（`emit_resolve_patch` `emit.yac:1060-1065` ✓）；
   blob 的入口却由 `fill_gref`（`emit_x86_64.yac:2315+` ✓）**按名字扫 `funOffsRev`** ✗ 现算
   （`T + LOAD_VADDR + TEXT_OFF + off` ✓）。名字在表里可能不止一条（stub 记录 + 真记录 ✓，§8.6 记过 ✓），
   **试过跳过空 insns 的记录，行为不变** ✗ ⇒ 该修法（至少单独）不成立 ✗。
2. **`emit_jsess_box` 在发射期间被写回旧值** ✗：第 3 行的 `fill_gref` 读到 `T = 58196`（= 第 2 行的长度 ✓）
   而同一行的 `dest = 59740` ✓ ⇒ 两者应相等（注释 `emit_x86_64.yac:2308-2314` 即以"`js == live`"为前提 ✓）
   ⇒ 发射路径里**有人把该盒子设成了上一行的值** ✗（待钉：在 `emit_program_x86_64` 入口再打一次 `T`，
   与 `fill_gref` 的 `T` 对比，就能看出是"进来就旧"还是"中途被改" ✓）。

**下一步（二选一，都不大）**：(a) 把 blob 的入口改成与 AOT **同源**（`emit_map_get` + `offs` ✓），
彻底不用名字扫描 ✓；(b) 先钉 (2) 的盒子被谁改（一次只读探针 ✓），把 `T` 恢复成"本行的 append offset" ✓
—— 顺带说明为什么 **A 恰好对**：定义在第 1 行时 `T = dest = 0` ✓，两个缺陷都被"零"掩盖了 ✓。

### 8.9 已修：`import` 提升（两个前端）+ `pkg compiler` 的跑法（2026-09-16）

**A. `import` 是声明，必须在使用之前生效（两个前端都缺）。**

用例 `tests/interp/import_late.yac`：

```yac
let f(_) = host_os(0)
import rt.os
if str_len(f(0)) > 0 then 1 else 0
```

| 路径 | 修前 | 修后 |
|---|---|---|
| C 解释器 `./yac` | `error: 1:12: unbound variable 'host_os'`（rc 1）✗ | **`1`** ✓（rc 0）|
| `yc` | `error: 1:1: unbound variable 'host_os'`（编译失败）✗ | 编译 ✓ + 运行值 **1** ✓ |
| 对照：把 `import rt.os` 挪到第 1 行 | 两者都 `1` ✓ | 同 ✓ |

**改法两步**：

1. `src-self/back/backend.yac`：新增 `hoist_imports`，在 `rewrite_imports` 开头调用它。选这里是因为
   `rewrite_imports` 是 **`report_unbound_ex` 的未绑定预检**与 **`link_from_ast` 的链接**共同入口
   ⇒ 一处改动同时覆盖"检查"和"链接"（两者原本都按源码顺序走，import 之前的名字不在作用域里）。
   稳定划分：imports 保持相对顺序在前、其余保持相对顺序在后；imports 是声明、无运行期效果 ⇒ 语义不变。
2. `src/parser.c`：`import_splice` 由"追加到 import 的**文本位置**"改成"插入到**前排 import 游标**
   `nhoist`"（`parse_items` 每层一个游标）。imports 本来在前时 `nhoist == nitems` ⇒ `memmove` 长度 0
   ⇒ 与原来的 append **逐字节等价**，既有文件零风险。同一语义也补了 compiler 用例
   `tests/compiler/cases/import_late.yac`（`["import_late", "rc", "1"]`）把 `yc` 侧固定下来。

**B. `pkg compiler` 该走进程内 host 路径，而不是独立二进制。**

`pkg/yc/compiler.yac` 头部写明：每个导出符号都是 **`@host` 宿主叶**，编译器树**故意不链进 guest**。
⇒ 独立跑必然打印 `host fn unavailable: host_format / host_arch / mk_target / compile_native` 并返回 0
（实测 ✓）——这是**设计**，不是 bug。真正能用这些叶子的只有 **yc 进程**，且只有 **named（REPL）路径**
会把宿主的槽表填进 blob（`back/jit.yac:97 host_tab_fill`、`:59 gref_fill`）；`--cps` 走
`named=false` 路径（`yc.yac:222 jit_eval`）✗，AOT 更没有宿主 ✗。实测三态：

| 跑法 | 输出 |
|---|---|
| 独立二进制（`yc <case> -o bin` 然后跑 bin） | `host fn unavailable: …` ×4 ⇒ rc **0** ✗ |
| `yc --cps <case>` | 同样四个 `host fn unavailable` ✗ |
| **`yc --repl <case>`（把用例作为位置参数预载 ⇒ named 路径）** | **`42`** ✓ |

**改法**（只动测试）：`tests/run.yac` 的 pkg 组新增 **`"host"` 类型**（新助手 `run_stdin` +
`pkg_one` 的 host 分支），`["compiler", "rc", "42"]` → `["compiler", "host", "42"]` ✓；
`pkg/yc/compiler.yac` 与用例文件**未改** ✓。

**验收**：

| 项 | 修前 | 修后 |
|---|---|---|
| host `compiler` | 182 / 1 | **183 / 1** ✓（+`import_late`）|
| host `interp` | 35 / 1 | **36 / 0** ✓ |
| host `pkg` | 21 / 1 | **22 / 0** ✓ |
| `qemu-arm64` / `qemu-riscv64` | 83 / 0 | **85 / 0** ×2 ✓ |
| 全量 `make test` | 679 / 7 | **712 / 2** ✓（唯一 FAIL 仍是 §8.8 那条）|

> **两条顺带记下的本机环境事实**（都在 §8.2）：#11 无 C 编译器 ⇒ 要 `make CC=gcc` 且走 MSYS2 MINGW64
> shell（裸 shell 里 `gcc` 连 `-E` 都起不来 ✗）；#12 `$(YC_A)` 的两趟自举**不能并行** ✗。

### 8.10 已修：REPL blob 的"两个基址"—— cell 用本镜像、G/宿主槽用会话（2026-09-17）

**症状**（用户报的）：`./yc --repl app/demo/t2.yac` → `import yc.compiler` → `compile("1+2")` ⇒ **SIGSEGV**；
gdb 显示 **RIP = 0** ✓、返回地址在 JIT 会话镜像里 ✓ ⇒ 会话执行了一次 `call 0` ✓。

**触发形态**（四组对照 ✓；A 需要"先有一行完整跑过"✓）：

| # | 会话 | 结果 |
|---|---|---|
| A | `1+1` → `import yc.compiler` → `compile("1+2")` | **139** ✗ |
| B | `1+1` → `import text.fmt` → `printf("x")` | 0 ✓ |
| C | `1+1` → `import yc.compiler` → `print(1)` → `compile("1+2")` | 0 ✓ |
| D | `1+1` → `let f(x) = x + 1` → `f(2)` | 0 ✓ |

**根因**：§8.8 的修复把 blob 的 globals 基址从"会话第一张镜像的 data"改成"本镜像 data + append 偏移" ✓
—— 这对**会话 let 的 cell**（tag 22/23/24）是必须的 ✓，但 **G / 宿主槽表**（tag 3..8、21）**不是本镜像的**：
`emit_glob_data` 对 blob 直接跳过 ✗，而宿主叶地址由 `back/jit.yac:97 host_tab_fill` 写进
**运行中会话**的 `data_start`（`yjit_layout_get` ✓）、不是写进 blob 自己的镜像 ✗ ⇒ 按"本镜像"去读那批槽
读到的是**全零内存** ✗ ⇒ `call 0` ⇒ SIGSEGV ✓（C 组只因中间多一行把会话推迟了一拍才没踩到 ✓）。

**修法**：把解析状态里的"一个 globals 基址"拆成**两个** ✓：

| 栏 | 是谁 | 谁用 |
|---|---|---|
| 3 `GLOBALS_BASE` | **本镜像** data（+append 偏移 ✓） | tag 22 / 23 / 24（gvar stub cell、AOT 入口、立即数入 cell ✓） |
| 6 `GSESS_BASE`（新增 ✓） | REPL blob = **会话**的 data；AOT = 本镜像 | tag 3..8（G+0..+128 ✓）、tag 21（宿主叶 G+136、extern G+456 ✓） |

- `emit_x86_64` 构造 `rst` 时第 7 栏放 `GSESS_BASE = if T != 0 then nth(js,1) else 本镜像` ✓
  （正是拆开之前的 `GLOBALS_BASE` 表达式 ✓）；arm64 / riscv64 两栏相同 ✓（它们总把宿主槽烘进本镜像 ✓）。
- `emit_apply_unres(off)` 加 append 偏移时**不动第 7 栏** ✓（会话的 G 不随追加移动 ✓）。
- `emit_resolve_patch` 里 `gsess = if len(st) > 6 then nth(st,6) else globbase` ✓（老状态兜底 ✓）。

**回归用例** ✓：`tests/run.yac` 的 repl 组新增
`["nested compile after import", "1+1\nimport yc.compiler\ncompile(\"1+2\")\n:q\n", "<bytes>"]` ✓
（compiler 组也会跑到 ⇒ 计数 +2 ✓）。

**验收** ✓：A 组 rc **139 → 0** ✓（输出 `<bytes>` ✓）、§8.8 的 `1+1 / let f / print(f(2))` 仍为 **3** ✓、
全量 `make test` **716 / 0** ✓（`compiler` 185/0 ✓、`pkg` 22/0 ✓、`interp` 36/0 ✓、
`qemu-arm64` / `qemu-riscv64` 85/0 ×2 ✓）。

### 8.11 已修：尾调用的目标 ABI 三方对齐（`traw`）—— 2026-09-17

**病灶**：跨过程尾调用（`tcall`）必须按**目标帧的 ABI** 放参数，而三个后端用了**三套判据** ✗：

| 后端 | 原判据 |
|---|---|
| x86_64 | 自调用用调用方自己的 `is_raw`（= `fun_is_raw` ✓），**其他一律写死 `true`**（当平目标）✗ |
| arm64 / riscv64 | **名字前缀 `yac_`**（`is_yac_proc_name`）✗ |

**真判据**（三个后端的 prologue 都这么读 ✓，注释写明 "never inferred here" ✓）：
首条 `local` / `$local` 指令的**第 4 栏** —— `selfabi = len(insn) > 3` ✓。

| 帧 | 参数寄存器 | 谁 |
|---|---|---|
| **标记**（对象 ABI ✓） | 从 **1 号**起，0 号是对象 | front 给每个可闭包调用的 proc 写第 4 栏 ✓ |
| **未标记**（平 ABI ✓） | 从 **0 号**起 | `_start`、运行期 `$proc`、宿主叶、unimpl 桩 ✓ |

`fun_is_raw`（§8.2 里常与它混着提 ✓）**不是**这个 ✗ —— 它描述的是**函数体的算子集**（`$` 系列 ✓）；`yac_` 前缀只是**约定** ✓。

**修法** ✓：`back/emit/emit.yac` 新增 `fun_selfabi`（读第 4 栏 ✓）、`tcall_tab_*`（把本次发射的 `funs` 编成 `[名字, 是否未标记]` 表 ✓）与 `tcall_raw_of(name)` ✓（表里查不到的（外部 / 跨镜像）⇒ **退回 `yac_` 约定** ✓）；三个后端各自在 proc 循环前 `tcall_tab_set(funs)` ✓，`traw` 一律取 `tcall_raw_of(fn)` ✓（x86_64 的自调用分支也改用它 ✓ —— 与旧的 `is_raw` 等价 ✓，现有 proc 上 `fun_is_raw` 与"未标记"同解 ✓）。arm64 / riscv64 的本地 `is_yac_proc_name` 删除 ✓（已移到 `emit.yac` 作为兜底 ✓）。

**验证** ✓：

| 项 | 结果 |
|---|---|
| 正证据（临时 `--verbose` 统计，取证后已撤 ✓） | `print(1)`：procs **120** / plain **105**（= 内核 `$proc` + `_start` + 10 个 unimpl 桩 ✓）；compiler bundle：procs **3469** / plain **1780** ⇒ marked 全是 guest ✓ ⇒ 判据确实读到第 4 栏 ✓ |
| 两趟自举 | 通过 ✓，且 **stage2 ≡ stage3 逐字节相同** ✓（md5 `3778b23d…` ✓）|
| 全量 `make test` | **716 / 0** ✓（未变 ✓ —— 现存 5 处手写 `tcall` 的目标全是未标记 `$proc` ⇒ 新旧判据同解 ✓）|
| `qemu-arm64` / `qemu-riscv64` | **85 / 0** ×2 ✓ |

**可达性**（为什么全绿也要修 ✓）：`tcall` 只能由**手写 LIR** 产生 ✓（普通源码写不出 `$proc` 记录 ✗；`runtime.yac` 里 5 处目标全是 `yac_*` ✓）⇒ 今天**无活症状** ✓。但一旦出现"**未标记但名字不是 `yac_`**"的目标（例如以后加一个别的命名的内核 proc ✓）或"**被标记的目标**" ✓，同一份源码在 x86_64 与 arm64 之间就会**差一整个参数寄存器** ✗ —— 这类不对称没有编译期报错 ✓，只有跨架构跑起来才看得出来 ✓。

> 顺带核对过的另一处同族判据 ✓：**普通调用** `ycall`（`emit_x86_64.yac:702` ✓）**一律按对象 ABI 发射** ✓（"flat callees ignore it" ✓）✓ ⇒ 与 `tcall` 不同 ✓，它不看目标 ✓，因此不受本次改动影响 ✓（平目标忽略 0 号寄存器 ✓）。

### 8.12 改判：`tailapply` / `ticall` 是**已退休 opcode**，不是待实现缺口（2026-09-17）

**起因** ✓：§8.2 第 1 条原记为"`tailapply`/`ticall` 带栈参数（>7）时 arm64 / riscv64 发 `brk` / `unimp` ⇒ 未实现" ✗，
看起来像个待补的能力缺口 ✓。实际不是 ✗ —— 这两个 opcode **已经在第 6 步收敛掉** ✓，
台账 §6「清理清单」（`FLAT_ABI.md` 第 288 行 ✓）里就列着它们 ✓：

> `| 第 6 步收敛掉的 opcode | xcall / apply / ticall / tailapply / iccall / $icall / gvld / gvst / gfnst |`

紧跟的一段还写明"**能力不删，只改形态**" ✓：动态调用（嵌套闭包 / call-cc 式）的**能力**保留 ✓，
但不再是独立 opcode ⇒ 收进调用指令的"运行期动态 caps"布局 ✓（`LIR.md` §4.4 ✓）。

**普查方法** ✓：`--dump-lir` 打出程序的 LIR ✓，统计**指令位置**上（`[op, ` ✓）各拼写的出现次数 ✓。两个样本 ✓：

| 样本 | 规模 | 对照（在用的 opcode ✓） |
|---|---|---|
| `tests/run.yac` ✓ | 439 `ycall` / 2076 `fcall` / 11 `tcall` ✓ | 不是空样本 ✓ |
| **编译器自身**（`build/yc_tmp/yc_bundle.yac` ✓）| **2.6 MB** LIR ✓ | 8277 `ycall` / 25409 `fcall` / 527 `tcall` / 33 `icall` / 2 `ccall` ✓ |
| **全用例库** ✓（111 个 `tests/compiler/cases/*.yac` + `tests/pkg/*.yac` ✓，逐个 dump ✓ 全成功 ✓）| 299 `ycall` / 14 `tcall` / 12 `icall` / 2 `ccall` ✓ | `apply` 10 ✓、`iccall` **5** ✗（见盲区 2 ✓）|

> **踩过并已修的坑** ✓：`--dump-lir` 对**带包导入**的程序原本**根本不出 dump** ✗ —— `backend.yac:966 dump_lir` 只做 `rt_for_link` ✓、
> 从没设过 `link_need_box` / `link_local_box` ✗（而 `rt_for_link` 只链这两个盒子里记着的包 ✓）⇒ 拿它编 `yc_bundle.yac` 只得到
> 52 字节的 `error: LIR: call to undefined procedure 'host_arch'` ✗。**空 dump 上 `grep` 也会得 0** ✗ ——
> 本小节第一版那句"编译器自身 0 处"就是这么来的 ✗（已按下面两个样本的真实测量改正 ✓）。
> **已修** ✓：`dump_lir` 现在先调 `link_from_ast(ast)` ✓（与 `compile_env_of` 同序 ✓），2.6 MB ✓ / 开头就是 `ycall rt.os/__init` ✓。
> 纯诊断路径 ✓、不进编译管线 ⇒ **不需要跑回归** ✓。

| 拼写 | 源码构造点 | emitter case | 出现（自身 / harness）| 判定 |
|---|---|---|---|---|
| `xcall` | **0** ✓ | 0 ✓ | 0 / 0 ✓ | **纯死** ✓（只在 `vops(0)` 名单里 ✗）|
| `gvld` | 0 ✓ | 0 ✓ | 0 / 0 ✓ | **纯死** ✓ |
| `gvst` | 0 ✓ | 0 ✓ | 0 / 0 ✓ | **纯死** ✓ |
| `gfnst` | 0 ✓ | 0 ✓ | 0 / 0 ✓ | **纯死** ✓ |
| `$icall` | 0 ✓ | 4 ✓ | 0 / 0 ✓ | **活着** ✗ —— 见下面的盲区 ✓ |
| `tailapply` | 1（`lir.yac:211` ✓）| 3 ✓ | 0 / 0 ✓ | 死 ✓（构造不可达 ✓，见下 ✓）|
| `ticall` | 1（`lir.yac:215` ✓）| 3 ✓ | 0 / 0 ✓ | 死 ✓（同上 ✓）|
| `iccall` | 1（`lir.yac:1417` ✓）| 3 ✓ | **5 / 0** ✗ | **活着** ✗ —— 见下面的"第二个盲区" ✓ |
| **`apply`** | **2**（`lir.yac:1195` / `:1203` ✓）| 3 ✓ | **387 / 64** ✗ | **仍在用** ✗ ⇒ **不能删** ✗ |

**`tailapply` / `ticall` 为什么不可达** ✓：唯一构造点是 TCO 重写的**非自调用**分支 ✓，
而 `tco_find`（`lir.yac:199` ✓）对 `apply` / `icall` 只在目标是**自调用**（`["self"]` ✓）时才返回命中下标 ✓
⇒ `tco_one` 只会走 `tcall` 分支 ✓。

**`iccall` 的判定** ✓：`ccall_is` ✓ = "被调方是一个**值**，且该值标记为 `ccall`" ✓ ⇒ "把装进来的 C 函数取成值再调"那一形（dylib / cimport 路 ✓）。
套件里的 `--shared ccall add` 是**直接** ccall ✓（不走值 ✓），编译器自身 2.6 MB 里也是 0 ✓ ⇒ **实践上死** ✓；
但构造点 `lir.yac:1417` 只被 `ccall_is` 把守 ✓，所以严格来说是"**没人走到**"而不是"**走不到**" ✗ ——
想删就先补一个"取成值再调"的用例确认 ✓（有 `vop_known` 兜底 ✓，删错也会在校验期响亮报错 ✓）。

**顺带发现的死代码不一致** ✗（都在不可达路径上 ✓）：x86_64 的 `tailapply`/`ticall` 用 `nreg = min(ntot, 6)` ✓
⇒ **>6 个值的多余参数被静默丢掉** ✗（返回错值 ✓）；arm64 / riscv64 同场景发 `brk` / `unimp` ✓（响亮 trap ✓）。
今天都不可达 ✓，但**一旦**有人手写 `$proc` 发出它 ✓，三架构行为就分叉 ✗。

**⚠ 普查的两个盲区**（都踩过 ✓，结论因此两次判错 ✗）：

1. **内核手写 LIR 不在 dump 里** ✗：`dump_lir` 只打**本单元**的 proc ✓（`dump_ps` 从 `st0 = len(rt0) + 1` 起 ✓），
   **runtime / 内核 proc 全被切掉** ✗ ⇒ 只由手写内核 LIR 使用的 opcode 在 dump 里必然显示 0 ✗ ——
   例如 `$icall`：`src-self/rt/runtime.yac` 里有 **3 处** ✓（`["$icall", 14, 4]` / `[16, 5]` / `[12, 9]` ✓，
   在 apply / 间接调用那几条内核 proc 里 ✓）而 dump 是 0 ✓ ⇒ **它是活的** ✗。⇒ 结论必须**同时**扫 `rt/runtime.yac` ✓。
2. **单进程样本覆盖不到全部语言面** ✗：拿"两个最大的程序"（harness + 编译器自身 ✓）当样本时，
   只有**各自编译成独立程序**才出现的 insn 会漏掉 ✗ —— `iccall` 就是这样被判成"实践上死"的 ✗：
   它只在 **C 互操作**里出现 ✓（`ccall(<值>, args…)` ✓，第一个参数是**值**不是字符串字面量 ✓），
   而那两个大样本都不碰 C 互操作 ✗。**全用例库普查** ✓（111 个 `tests/compiler/cases/*.yac` + `tests/pkg/*.yac` ✓，
   逐个 `--dump-lir` ✓、全成功 ✓）：`iccall` **5 处** ✗（`ccall_cload` 2 ✓ / `ccall_many` 1 ✓ … ✓），
   用例 `ccall_cload`（rc 20 ✓）/ `ccall_many`（rc 64 ✓）本来就在 compiler 组里跑 ✓ ⇒ **它是活的** ✗。
   （同一轮也顺带复核了 `tailapply` / `ticall`：**全用例库 0 处** ✓ ⇒ §6 那次删除站得住 ✓。）

⇒ 可靠的普查配方 ✓：**全用例库 dump**（111 个 ✓）+ **`rt/runtime.yac` 手写 LIR 源码扫描** ✓ —— 两边都为零才叫死 ✓。

**结论与已执行的动作**（2026-09-17 ✓）：

| 动作 | 对象 |
|---|---|
| **删掉** ✓（构造点不可达 ✓；三后端 handler + 名单 + 前端构造 + `vops(0)` 一起摘 ✓）| `tailapply` / `ticall` ✓：`lir.yac` 的 `tco_one` 非自调用分支改成 `else ins` ✓（留成普通调用 ✓，值由外层 `ret` 带走 ✓）、`prof_is_tco` 只剩 `tcall` ✓、三后端各删一个 handler 块与名单项 ✓、`emit.yac` 的 `apply_ncap`/`apply_args` 去掉 `ticall` 分支 ✓、只服务于它的 `emit_call_guard_tail`（x86_64 ✓）一并删除 ✓ |
| **删掉** ✓（只摘名字 ✓）| `xcall` 从 `vops(0)` 摘除 ✓（它的 x86_64 handler 早被删过 ✓，注释留在 `emit_x86_64.yac:1995` ✓）|
| **无需动** ✓（本来就没有 ✓）| `gvld` / `gvst` / `gfnst` —— 全仓只剩**历史注释** ✓（`emit.yac:1198` / `jit.yac:27` / `runtime.yac:2209` 等 ✓）|
| **保留** ✗ | `$icall`（内核手写 LIR 3 处在用 ✓）、`apply`（动态调用能力的实际拼写 ✓）、`iccall`（**活的** ✗：`ccall(<值>, args…)` ✓，用例 `ccall_cload` / `ccall_many` 就在 compiler 组里跑 ✓）|

**验收** ✓：两趟自举通过 ✓ + **stage2 ≡ stage3 逐字节相同** ✓（删的都是不可达代码 ⇒ 输出不该变 ✓，实测如此 ✓）；
全量 `make test` **716 / 0** ✓（0 条 FAIL 行 ✓）；`qemu-arm64` / `qemu-riscv64` **85 / 0** ×2 ✓（两架构的 handler 块也删了 ✓）。

**校验兜底** ✓（`backend.yac:1025 vop_known` ✓ → `vinsns` 里 `vbad(name, "unknown op '…'")` ✓）：
名字从 `vops(0)` 摘掉后 ✓，任何残留的 `tailapply` / `ticall` / `xcall` 都会在**校验期**响亮报错 ✓；
反过来 ✗ 只删 emitter 的 case 而留着名单 ✓，就会落进 dispatcher 末尾的 `else st` ⇒ **静默不发射** ✗（那才真危险 ✗）。

> **`apply` 千万不能删** ✗ —— 它才是"动态调用能力"当前的实际拼写 ✓（样本 387 / 64 处 ✗）：
> §6 说的"**能力不删，只改形态**"这件事**还没做完** ✗（`apply` 仍是独立 opcode ✓）。
> 顺带清掉一条**陈旧注释** ✓：x86_64 的 tailapply 块曾写着"REPL 行的尾调用走这里" ✗ ——
> 实测（`printf '1+1\nlet a = 5\na()\n:q\n' | ./yc --repl --dump-lir` ✓）今天该行降成 **`icall`** ✓。

### 8.13 已修：arm64/riscv64 的 GC 根发布 + arm64 的 G 区大偏移（2026-09-17）

**怎么发现的** ✓：清点 §6 时顺手核对"顶层值注册表现在谁发布" ✓ —— `emit.yac:601` 写的是
`yac_gval_pub` / `yac_gfn_pub` ✓，而 arm64 / riscv64 的 `gset` 注释写着 "GC publish … **TODO**" ✗。

**#13：两个架构少了那次发布** ✗

`gset` 的**非立即数**分支（运行期赋值 ✓）在 x86_64 上是"存储 + 调 `yac_gval_pub(name, value)`" ✓
（`emit_x86_64.yac:1373` ✓），arm64 / riscv64 只做了存储 ✓ ⇒ G+440 的 `[name, value]` 表里没有这一条 ✓
⇒ `yac_gc_collect` 不标它 ✓ ⇒ **只被那个 cell 引用的活值可能被回收** ✗（FLAT_ABI.md 2.3 的 GC 根 ✓）。
修法 ✓：两架构各补上同样的调用 ✓ —— `yac_gval_pub` 是**未标记** `$proc`（平 ABI ✓）⇒ 名字进 0 号参数寄存器 ✓、
值进 1 号 ✓；strlit 与调用都按各自 `fcall` 的惯用法打 patch ✓（`a64_bl` / `rv_jal` + `emit_patch_rel` ✓）。

**#14：arm64 的 `$ld64`/`$st64` 够不到 G+440** ✗（修 #13 时才暴露 ✓）

arm64 把偏移**直接**塞给 `ldur`/`stur` ✗ —— 它们只有 **9 位（±256 字节）** ✓，而 G 区布局要走到
**G+440 / G+448**（顶层值 / 函数注册表根 ✓，`emit.yac:601` ✓）⇒ 越界 ⇒ 编出**错误的地址** ✓
⇒ `yac_gval_pub`（写 G+440 ✓）与 `yac_gval_list`（读 G+440 ✓）在 arm64 **必崩** ✓。
同文件的 `$ld8` 一直有 `add` 兜底 ✓，64 位版漏了 ✗。修法 ✓：`|off| > 255` 时先 `add`/`sub` 把偏移折进基址寄存器 ✓，
再把 `ldur`/`stur` 的偏移写 0 ✓（与 `$ld8` 同形 ✓）。

**证据（修前 → 修后）** ✓ —— 探针 = 顶层运行期赋值 + 只数注册表条数 ✓（不碰 `intern` 名字 ✓）：

```yac
let v = str_cat("keep", "_me")
print(len(yac_gval_list()))
```

| 架构 | 修前 | 修后 |
|---|---|---|
| x86_64（对照 ✓）| **1** ✓ | **1** ✓ |
| riscv64 | **0** ✗（表是空的 ✓）| **1** ✓ |
| arm64 | **SIGSEGV** ✗（G+440 地址算错 ⇒ 读到垃圾 ✓）| **1** ✓ |

> 修 #14 之前，连**源码里直接写** `yac_gval_pub("probe", 7)` 在 arm64 也崩 ✓ —— 与 emitter 改动无关 ✓，
> 是一条一直躺在那里、只在 arm64 上炸的独立 bug ✓。

**验收** ✓：两趟自举通过 ✓ + **stage2 ≡ stage3 逐字节相同** ✓；新用例 `tests/compiler/cases/gcroot_pub.yac` ✓
（`["gcroot_pub", "out", "1"]` ✓）在 **compiler / arm64 / riscv64 三处 PASS** ✓；全量 `make test` **721 / 0** ✓（0 条 FAIL ✓）。

### 8.14 已修：顶层 `let` 的值可以当函数调用（2026-09-17）

**症状** ✓（响亮报错 ✓）：

```yac
let f(x) = print(x)
let a = f
a("x")                    /* error: LIR: call to undefined procedure 'a' */
let s = str_cat
print(s("ab", "cd"))      /* error: LIR: call to undefined procedure 's' */
```

**根因** ✓：前端的调用解析（`lir.yac` 的 `calli`）只走两条路 —— Γ（局部槽 ✓）与 Σ（**过程** ✓）——
而顶层 `let` 绑定的**值**两条都不在 ✗。引用侧（`lir_var` ✓）明确写着
"top-level values are not procs and are **absent from Σ**" ✓，它用 `sigma_kind_h(...) == 2` 认出来 ✓
（`kind == 2` ⇒ `["gval", slot, name]` ✓）。`calli` 里没有这一支 ⇒ 落到最后的 `else` ⇒
`log_fatal("LIR: call to undefined procedure '…'")` ✓。

**修法** ✓（一处 ✓）：`calli` 加一个参数 `inl`（本项的指令表 ✓，供它推入 `gval` ✓）与一个分支 ✓ ——
`kind == 2` 时：`insn_push(inl, ["gval", dst + 1, g])` ✓ 再返回 `["icall", dst, dst + 1, ss]` ✓。

- **为什么是 `icall`** ✓：值调用必须走**对象 ABI** ✓（§8.1 的"规范结论" ✓）—— 闭包对象进 0 号参数寄存器 ✓、
  参数从 1 号起 ✓。平过程（`$proc` / `str_cat` 这类 ✓）到这一步**已经**被 `lir_clos_prim` 包成 0 捕获的
  thunk 闭包 ✓ ⇒ 直接 `icall` 就对 ✓。
- **为什么 `dst + 1` 能当临时槽** ✓：调用的结果落在 `dst` ✓，而下一个绑定要等这一串发完才占用 `dst + 1` ✓。

**证据（修前 → 修后）** ✓：

| 形态 | 修前 | 修后 |
|---|---|---|
| `let f(x) = print(x)` + `let a = f` + `a("x")` | `error: LIR: call to undefined procedure 'a'` ✗ | 打出 **x** ✓ |
| `let s = str_cat` + `print(s("ab","cd"))` | `error: … 's'` ✗ | 打出 **abcd** ✓ |
| 局部高阶 `let h(g) = g(7)`（对照 ✓）| 8 ✓ | 8 ✓（无回归 ✓）|

**验收** ✓：两趟自举通过 ✓ + **stage2 ≡ stage3 逐字节相同** ✓（编译器自身不走这条新路 ✓，所以输出不变 ✓）；
新用例 `tests/compiler/cases/call_toplevel_value.yac` ✓（`["call_toplevel_value", "rc", "2"]` ✓）在
**compiler / arm64 / riscv64 四处 PASS** ✓；全量 `make test` **726 / 0** ✓（0 条 FAIL ✓）。

> **写用例时自己踩的坑** ✗（记一笔 ✓）：`len` 是**列表**的 ✓，字符串要用 **`str_len`** ✓ ——
> 第一版用例写了 `len("42")` ✓，编译运行都不报错 ✗ 但值不是 2 ⇒ 表现为"用例失败"✗ 而编译器无辜 ✓。
> ⇒ **新用例的期望值也要先手工跑一遍** ✓（本次就是这么发现的 ✓）。

### 8.15 已修：profiler 钩子的 ABI 不再"两头兼容"（2026-09-17）

**病灶** ✓（§8.7 留下的补丁 ✓）：内核用**裸名字**调钩子（`rt_prof_enter_ins` ✓ 里那一条 `fcall prof_enter_go` ✓）。
`runtime_add` / `rt_funs_rename_prof` 会把这条指令改成对象 ABI 的 `ycall` ✓ —— 但它在**链接期**跑 ✓，
那时**本单元自己的 proc 还不存在** ✗ ⇒ 程序**自带**的钩子（`let prof_enter_go(name) = …` ✓ = 普通 letfun = **对象 ABI** ✓）
不在它看得见的列表里 ✗ ⇒ 调用保持平 ABI ✗，而名字只在 0 号参数寄存器里 ⇒ 对象 ABI 的钩子从 1 号读 ⇒ 读到 nil ✗
（§8.7 的 139 段错误就是这么来的 ✓）。当时的兜法是把名字**同放两个寄存器**（实参表 `[1, 1]` ✓）✗ ——
能用，但"一旦钩子多参、或哪天只留一个寄存器"就会再错位 ✗。

**修法** ✓：把"按**目标帧的 ABI**决定调用种类"这条规则（§8.11 为 `tcall` 立的那条 ✓）也用到钩子上 ✓：

| 位置 | 改动 |
|---|---|
| `back/emit/emit.yac` | 新增 `prof_marked_hook` / `prof_fix_insns` / `prof_fix_funs` / `prof_hook_fix` ✓：扫**最终** proc 列表 ✓，若存在**本单元定义**的、**marked**（对象 ABI）`prof_enter_go` / `prof_leave_go` ✓ ⇒ 把内核那两条 `fcall` 改成 `ycall` ✓；否则原样返回（**零开销** ✓）|
| 三个后端 | 在构造 `funs` 时套一层 `prof_hook_fix(...)` ✓（在 `tcall_tab_set` 之前 ✓，三处各一行 ✓）|
| `rt/runtime.yac` | 实参表 `[1, 1]` → **`[1]`** ✓（**撤掉双寄存器 hack** ✓）；两处注释改成描述新规则 ✓ |

平 ABI 的那一半（运行期自带的 no-op 桩 `prof_enter_go` ✓ 是 `$proc` ✓）不受影响 ✓ ⇒ 仍是 `fcall` ✓ + 一个实参 ✓ ✓。

**正证据** ✓（可推理 ✓）：撤掉 hack 之后，0 号寄存器以外**没有**第二份名字 ✓；若 `ycall` 改写没生效 ✓，
marked 的钩子会从 1 号参数寄存器读到别的东西 ⇒ 输出**必然不是** `f` ✓。实测：

| 用例 | 结果 |
|---|---|
| `tests/compiler/cases/prof_hook_name.yac` ✓（程序自带钩子 ✓）| 输出恰为 **`f`** ✓（改写生效 ✓）|
| `tests/pkg/prof_hook.yac` ✓（包提供钩子 ✓）| rc **42** ✓ |
| `tests/pkg/profile.yac` ✓ | rc **42** ✓ |
| 全量 `make test` | **726 / 0** ✓（0 条 FAIL ✓）|

**验收** ✓：两趟自举通过 ✓ + **stage2 ≡ stage3 逐字节相同** ✓；新 pass 只在"本单元有 marked 钩子"时才动手 ✓
⇒ 对现有程序是**空操作** ✓（726/0 与改动前一致 ✓）。

> 附带说明 ✓：`prof_hook_name.yac` 的注释里那段"同放 rdi 与 rsi"的描述已按新机制改写 ✓ ——
> 否则它会变成下一份"过期真相" ✓。

### 8.16 已做：`apply` 收进唯一动态调用形态（三步 ✓）—— 2026-09-17

**先把"值不值"厘清** ✓：`LIR.md` §4.4.6 已经把"`apply` / `icall` / `tailapply` / `$icall` 全删 ✓、
调用只有一种"写成**目标设计** ✓，§4.4.7 记的落地现状是"`apply`/`icall` 的 emit 保留为对象 ABI 序列 ✓、
**前端的发码点已收敛** ✓" ⇒ 要紧的不是删一个拼写好看 ✓，而是**少一个"调用点必须与目标帧协商"的编译期字段** ✓
（`ncap` ✓）—— 本会话抓到的 §8.1 / §8.7 / §8.11 / §8.15 全是这一类 ✗。查证结果比预期好：

| 事实 | 结论 |
|---|---|
| 三个后端**没有任何一处**读 `ncap` ✓ | `apply` 与 `icall` 在 **emit 层早已是同一条指令** ✓ |
| x86_64 `emit_x86_i_clos` 里那套"旧平 ABI 重排 + 动态 nenv 跳表" ✗ | **死代码** ✓（该函数只在 `k == "closure"` 时被调用 ✓）|
| arm64 / riscv 的注释 ✓ | 写明 caps 由**被调方**经 self 寄存器读 ✓、"repack 是**已死的平 ABI 方案**" ✓ |

⇒ 三步里**没有一步需要动 ABI** ✓：步 1 只换拼写 ✓、步 2 只删拼写 ✓ —— 机器码逐字节不变 ✓
（步 1 后与步 2 后各做一次固定点自举，**两次都 IDENTICAL** ✓，这正是"ncap 早已没人读"的实证 ✓）。

**步 0：删死代码 + 撤 `apply_ncap`** ✓

- `emit_x86_64.yac`：删掉 `emit_x86_i_clos` 内整段 `apply`/`icall` ✓（含动态 nenv 跳表 ✗）。
- `emit.yac`：删 `apply_ncap` ✓（撤导出 ✓ + 撤三后端 import ✓）；`icall_args` 保留并简化为 `nth(insn, 3)` ✓。
- 验收：全量 **726 / 0** ✓（未变 ⇒ 那段确实不可达 ✓）。

**步 1：前端改为只发 `icall`** ✓

`lir.yac` 两处发码点 ✓：`["apply", dst, ["self"], ncap, ss]` → `["icall", dst, ["self"], ss]` ✓；
`["apply", dst, clo, ncap, ss]` → `["icall", dst, clo, ss]` ✓（caps 由对象携带 ✓，调用点不再重排 ✓）。
**TCO 不受影响** ✓：`tco_find` 早已同时认 `apply`/`icall` ✓、`tco_one` 的 `icall` 分支产出 `tcall` ✓ ——
正证据：`tests/compiler/cases/tco_cap.yac` ✓（`let x = 42 in let loop(n) = … loop(n-1) in loop(100000)` ✓）
的 LIR 仍是 `[tcall, 7, loop, [6]]` ✓ ⇒ 带捕获的自递归仍走循环 ✓（否则 10 万层必爆栈 ✓）。

**步 2：删拼写** ✓

| 位置 | 改动 |
|---|---|
| `lir.yac` | `tco_find` 只认 `icall` ✓；`tco_one` 删掉与之重复的 `apply` 分支 ✓ |
| 三后端 | 分发条件 `k == "apply" or k == "icall"` → `k == "icall"` ✓（arm64 / riscv 的 `i_clos` 守卫同步 ✓）|
| `backend.yac` 的 `vops(0)` | 摘掉 `"apply"` ✓（有 `vop_known` 兜底 ⇒ 残留会**校验期响亮报错** ✓）|
| `emit.yac` | `apply_args` → **`icall_args`** ✓（名字也改准 ✓，不留过期真相 ✓）|

**验收** ✓：

| 项 | 结果 |
|---|---|
| 自身 LIR 普查（2.6 MB ✓）| `apply` **387 → 0** ✓；`icall` 33 → **404** ✓ ⇒ `[apply` 已不可能再出现 ✓ |
| TCO 正证据 | `tco_cap.yac` 仍为 `[tcall, 7, loop, [6]]` ✓ |
| 两趟自举 + 固定点 | 通过 ✓，**stage2 ≡ stage3 逐字节相同** ✓（步 1 后、步 2 后各验一次 ✓）|
| 全量 `make test` | **726 / 0** ✓（0 条 FAIL ✓，含 arm64 / riscv64 用例组 ✓）|

> **§6 的 opcode 条目就此结清** ✓：`tailapply` / `ticall` / `xcall` / **`apply`** 已删 ✓；
> `gvld` / `gvst` / `gfnst` 本来就没有 ✓；`iccall` / `$icall` 经核实**是活的** ✓（C 互操作 / 内核手写 LIR ✓），
> **不在删除范围** ✓ —— 至此 §6 只剩那 8 项"逐项核实早已不存在 / 仍在用"的记录 ✓（见本节的状态表 ✓）。

### 8.17 已修：REPL 入口按**发射顺序**定位（原 §8.2 #9）—— 2026-09-17

**症状** ✓：REPL 里 `let _eval(x) = x + 1` 再 `print(_eval(41))` ⇒ **SIGSEGV（rc=139）** ✗（`--dump-lir` 显示崩前那行正常提交 ✓）。

**根因** ✓：blob 的**入口包装器**就叫 `_eval` ✓，而一个 blob 里有**三份** `_eval`：包装器 ✓、
一条**空体前向桩** ✓（`[proc, _eval, 1, 0, [], _eval]` ✓）、以及**客体自己定义的那份** ✓。
JIT 用 `fun_off(fo, "_eval")` 找入口 ✗ —— `fo` 是 `funOffsRev` ✓（发射器按发射顺序 `cons` 出来的**逆序**表 ✓），
扫描从**表头**开始 = **最后发射**的那个 ✗ ⇒ 客体那份把包装器**盖掉** ✓ ⇒ JIT 把客体过程当入口跑 ⇒ 崩 ✓。

**修法**（KISS ✓：**一个查表问题就用查表解决** ✓，不发新状态 ✓）：入口**本来就在手里** ✓ ——
四个 blob 构造器都是 `["prog", cons(entry, …), "_start"]` ✓ ⇒ 入口恒为 `funs[0]` ✓，
也就是 `funOffsRev` 的**最后一个**记录 ✓。`backend.yac` 把 `fun_off(fo, name)` 换成 `entry_off(fo)`
（走到表尾 ✓，4 行 ✓），`jit.yac` 五处与 `pack_yjit_prog` 改用它 ✓；
`build/patch_funoffs.py` 同理改成 `funs[i]` ↔ `offs[i]` 配对 ✓（`offs` 对**每个** proc 都 push ✓，与 `funs` 逐项对齐 ✓）。

> **试过并否掉的方案** ✗：让三后端在 `i == 0` 时调 `yjit_entry_set(...)` 发布入口偏移 ✓。
> 它把**一个查表问题**变成**发射路径 + 全局箱子 + 三处 import** ✗ —— 耦合与状态都加错了地方 ✗，
> 而答案本来就躺在那张表里 ✓。已撤 ✓。

**验收** ✓：

| 项 | 结果 |
|---|---|
| 复现（`let _eval` → `print(_eval(41))`）| rc **139 → 0** ✓、输出 **42** ✓ |
| 对照（`let f(x) = x + 1` → `print(f(2))`）| 仍 **3** ✓ |
| 回归用例 | `repl guest _eval shadows wrapper` ✓ **PASS** ✓ |
| 两趟自举 + 固定点 | 通过 ✓，**stage2 ≡ stage3 逐字节相同** ✓ |
| 全量 `make test` | **728 / 0** ✓（0 条 FAIL ✓；`compiler` **188 / 0** ✓）|

**`build/patch_funoffs.py` 那半：没修成，但查清了真因** ✓（工具**已恢复原状** ✓，只留下注释与一条 `--revert` 修复 ✓）：

| 变体（注入到 `emit_program_x86_64` 里）| 结果 |
|---|---|
| A：只 `write_file(..., "常量")` ✓ | 插桩编译器**正常** ✓、文件写出 ✓ |
| B：A + 一个**局部递归函数** ✓（= 原工具的形状 ✓）| **SIGSEGV（rc=139）** ✗、文件不写 ✗ |

⇒ 崩因是**往那个过程里注入"局部函数"** ✗（会改它的帧 ABI ✓），**与按名字 / 按下标配对无关** ✓ ——
工具 docstring 里"printing from emit breaks the self-build"是**误判** ✗。所以：那条修法（改成 `funs[i]` ↔ `offs[i]`）
**看似对、实测崩** ✗ ⇒ 已撤回 ✓。**可信的 dump 用 `--dump-asm`** ✓：`asm_dump_procs` 是**顶层过程** ✓
（不新建闭包 ✓）、且本来就**按下标**把 `funs` 与 `offs` 配对 ✓（`asm_dump_procs` 里 `nth(offs, i)` ✓）。

> **另记一条过程教训** ✗：查这条时我自己 `rm` 了 `build/funoffs.txt` ✓，又用**可能已被换掉的 `yc`** 下了"LIR 里没有 write_file"的结论 ✗ —— 判据本身不干净 ✓。后用**干净两趟构建**（与 728 / 0 那次**逐字节相同** ✓）重做，才有上面的 A/B 结论 ✓。

> **顺带发现（未修，记为 §8.2 #15）** ✗：REPL 里**同名重定义**（`let f(x) = 1` → `let f(x) = 2` → `f(0)`）
> 会 **SIGILL（rc=132）** ✗。与本节不是同一条路 ✓（jslot 槽号实测一致 ✓，坏的是**存进/取出的值** ✗），机理待查 ✓。

### 8.18 已修：内建原语的元数不匹配 ⇒ 段错误（`exit()` 等 6 个）—— 2026-09-17

**症状**（用户报的 ✓）：REPL 里 `exit()` ⇒ **SIGSEGV（139）** ✗。查下去发现**不是 REPL 特有** ✓：
AOT 里同样崩 ✓，而且**同一族还有 6 个** ✗：

| 写法 | 改前 | 改后 |
|---|---|---|
| `exit()` / `str_cat()` / `str_ref(1)` / `str_len()` / `read_file()` / `write_file("x")` / `system()` | **SIGSEGV 139** ✗ | `error: LIR: builtin '…' called with too few arguments (N)` ✓ **rc 2** ✓ |
| `exit(0)` / `exit(7)` / `str_cat("a","b")` / `gc_collect(1)` | 正常 ✓ | 正常 ✓（未受影响 ✓）|

**根因** ✓：`exit` / `str_len` / `str_ref` / `bytes_len` 是**没有过程的内在原语** ✓，`read_file` / `write_file` /
`system` 等则直接落到内核过程 ✓ —— 两条路都在 `front/lir.yac` 的 `lir_rt_*` 表里**直接** `nth(ss, k)` ✗。
`nth` 越界返回 `[]` ✓ ⇒ insn 变成 `["exit", []]` ✗ ⇒ 发射器 `emit_load_rax_slot(nth(insn,1))` 把 `[]`
当**槽号**读 ✗ ⇒ 读垃圾地址 ⇒ 崩 ✓。LIR 铁证 ✓：`exit(7)` 是 `[exit, 4]` ✓（槽号 ✓），`exit()` 是 `[exit, []]` ✗。

**顺带确认的惯例** ✓：**guest 过程**的元数不匹配是**容忍**的 ✓（`let f(x)=x` 下 `f()` ⇒ nil ✓、`f(1,2)` ⇒ 忽略多的 ✓）
—— 只有"原语"这条路在崩 ✗，所以它不是设计 ✓，是漏了检查 ✓。

**修法**（KISS ✓，一处描述不变 ✓）：`front/lir.yac` 新增守卫

```
let rt_arg(g, ss, k) = if k < len(ss) then nth(ss, k) else log_fatal(...)
```

并把五张表（`lir_rt_list` / `str` / `bytes` / `num` / `rt` ✓）里的 `nth(ss, k)` **47 处**机械换成 `rt_arg(g, ss, k)` ✓
（唯一的第 48 处是 `lir_leticcall` 里 `["iccall", dst, nth(ss, 0), …]` ✗ —— 那儿的 `ss[0]` 是**被调方槽** ✓，
另一套约定 ✓、必然存在 ✓ ⇒ 已还原 ✓）。**元数仍然只由"降级里读了几次"这一个地方描述** ✓（不新增元数表 ✓），
缺参则按前端既有方式报给用户 ✓（`log_fatal` ⇒ `error: LIR: …` ✓、rc 2 ✓，与 `let x = band` 那条同形 ✓）。

**验收** ✓：

| 项 | 结果 |
|---|---|
| 上表 6 个写法 | **139 → rc 2 + 明确文案** ✓（不再有段错误 ✓）|
| 正确写法（`exit(7)` ✓ / `str_cat("a","b")` ✓ / `gc_collect(1)` ✓）| 行为不变 ✓（`exit(7)` 仍是 rc 7 ✓）|
| 用户报的 REPL 场景 | `exit()` 从**段错误** ⇒ **干净报错** ✓；`exit(3)` 仍以 3 退出 ✓（离开 REPL 用 `:q` / `exit(0)` ✓）|
| 全量 `make test` | **738 / 0** ✓（0 条 FAIL ✓ ⇒ 无误伤合法调用 ✓；含 repl 三条 + compiler 两条负例 ✓）|

> **测试基建也随之补上了** ✓：原来 `repl_check` 要求 **rc = 0**（注释就写着 "rc must be 0 (not SIGSEGV)" ✓），
> "**应当失败**"的负例表达不出来 ✗ —— 这正是 `exit()` 能悄悄崩掉的原因之一 ✓。
> 现在 repl 用例可以带**可选第 4 栏 = 期望退出码** ✓（`tests/run.yac` 的 `repl_check(st, yc, name, src, needle, wantrc)` ✓，
> 不带则仍是"必须 0 且含 needle" ✓），并补了三条 ✓：
>
> | 用例 | 期望 | 修复前 |
> |---|---|---|
> | `repl exit() too few args` ✓ | rc **2** + 文案含 `builtin 'exit' called with too few arguments` ✓ | **139** ✗（⇒ 会判 FAIL ✓ 已实测 ✓）|
> | `repl str_cat() too few args` ✓ | rc **2** + 文案 ✓ | **139** ✗ ✓ |
> | `repl exit(3) status` ✓ | rc **3** ✓（这不是错误 ✓：`exit(n)` 真以 n 退出 ✓）| 3 ✓ |
>
> **反向验证** ✓：拿**修复前**那次构建（与 728/0 同哈希 ✓）跑同三条输入 ⇒ `exit()` / `str_cat()` 都 **139** ✗、
> `exit(3)` ⇒ 3 ✓ ⇒ 前两条**确实会红** ✓（能失败的测试才算测试 ✓）。
>
> **编译器组**（AOT 侧）也补上了同类负例 ✓：新加 `compiler_err_cases(0)` + `compiler_err_one` ✓ ——
> 它在 `compiler_all` 里单独跑 ✓（**故意不塞进被 cps / eval / iso 共用的 `compiler_cases(0)`** ✓，
> 那些组没有"期望失败"的概念 ✓），源文件内联 ✓（不新增用例文件 ✓）。语义：**必须以 rc 2 失败** ✓、
> 文案含预期片段 ✓、且**不留产物** ✓。两条 ✓：`err_exit_arity` ✓ / `err_str_ref_arity` ✓。
> 反向验证 ✓：修复前 `let _ = exit()` **编译成功（rc 0 ✓）**、产出 95 KB 产物 ✓、而它**运行时段错误（139）** ✗
> ⇒ 新用例判 `rc=2` 得 0 ⇒ **会红** ✓ ✓。

### 8.19 已修：REPL 同名重定义 —— 不再挂掉，并给出覆盖告警 —— 2026-09-18

**症状** ✓（用户报的 ✓）：REPL 里 `let f(x) = 1` → `let f(x) = 2` → `f(0)` ⇒ **SIGILL（rc=132）** ✗。

**触发面** ✓（判别矩阵 ✓）：**值**重定义正常 ✓；**换名**的第二个函数 blob 正常 ✓；只有**同名函数重定义** ✗ 崩 ✓；
重定义后**只求值不调用**是好的 ✓（打印 `<fun>` ✓）⇒ 定义阶段没问题 ✓，坏的是**烘进 gref 单元的入口** ✗。

**根因** ✓（证据链 ✓）：

1. 发射循环用 `emit_jsess_skip(name)`（`emit.yac` ✓）**只按名字**查会话出口表 ✓ ⇒ 第二行 `let f(x) = 2` 时 `f` 已在表里 ⇒ **整个 `f` 被跳过** ✗；
2. 于是该 blob 的 `funOffsRev` 里没有 `f` ⇒ `fill_gref` 的 `find_gref` 得 -1 ⇒ 按回退**绑到 `yac_host_unimpl` 桩** ✗；
3. 下一行一调 ⇒ **SIGILL** ✓（gdb：RIP = `JIT_VADDR+60031` ✓，落在该 blob 代码区之外 ✓、`bt` 无帧 ✓；
   dump 里 `f @l (6432 bytes)` 的垃圾偏移 ✓ 正是"被跳过"时 `offs` 存的 `skipat - T` ✗）。

**修法**（两处 ✓）：

| 处 | 改动 |
|---|---|
| `emit_x86_64.yac` 的 proc 循环 | 跳过判据改成**记录种类** ✓：只有 `nth(f, 0) == "$proc"`（内核 / 运行期 ✓）才允许 `emit_jsess_skip` ✓；guest 过程（`["proc", …]` ✓）一律照发 ✓ —— 换名的新函数本来就不在出口表里 ✓，所以这条对它**零影响** ✓，只改"重定义"这一种情形 ✓ |
| `back/jit.yac` 的 `compile_jit_go` | 在 `let bn = repl_last_let(ast1)` 之后 ✓，若 `bn != ""` 且 `jit_let_idx(bn) >= 0` ⇒ 打印 `warning: redefining '<名>' -- the new definition replaces the previous one` ✓（`jit_let_idx` 是**纯查询** ✓，登记的正是本会话绑定过的顶层名字 ✓ —— **值**也在内 ✓）|

**为什么不是"按 `i < nguest` 分"** ✗（试过并撤回 ✗）：blob（T != 0）时 `funs` **不再追加 unstub 表** ✓ ⇒ `i < nguest` 恒真 ✗ ⇒ 等于把**整个运行期**重发进 blob ✓ ⇒ 布局全乱，连原本正常的"换名第二条"也崩 ✗。判据必须落在**记录种类**上 ✓。

**验收** ✓：

| 项 | 结果 |
|---|---|
| `let f(x)=1` → `let f(x)=2` → `f(0)` | **132 → rc 0，输出 2** ✓ + 告警 **1** 条 ✓ |
| 三次重定义 | rc 0、输出 **3** ✓、告警 **2** 条 ✓ |
| 值重定义 `let a=1` → `let a=2` → `a` | rc 0、**2** ✓ + 告警 ✓ |
| 单定义 / 换名 | **无告警** ✓、行为不变 ✓ |
| 回归用例（repl 组 ✓）| `redefine fn warns` ✓ / `redefine fn takes effect` ✓ / `redefine value warns` ✓ —— rc=0 管"不挂" ✓、needle 管告警 ✓ |
| 全量 `make test` | **746 / 0** ✓（`compiler` **196 / 0** ✓）|

> 另 ✓：`emit_jsess_skip` 只有 x86_64 调用 ✓（arm64 / riscv64 不追加会话 ✓）⇒ 本次无需同步另两后端 ✓。

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
