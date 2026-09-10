# FLAT_ABI.md — Flat ABI 与目标 IR（rev3）

> **本文档只描述待实现的目标。** 已落地的部分（顶层函数 flat：`gvar` + 静态
> stub cell + `fcall` rel32）不再展开，仅在建立上下文时用一句话带过。
> 目标 IR 的完整清单见 **附录 A**，与新指令的对照见 **附录 B**。

## 目标

一句话：**让"扁平"成为编译器分析的常态结果，而不是顶层函数的特例。**

三件事：

| # | 内容 | 解决什么 |
|---|---|---|
| **1** | **值也静态化** | 顶层 `let x` 有静态单元；引用它就是一次 load，不再走 caps 链。这是"顶层函数真捕获数 = 0"成立的前提 |
| **2** | **闭包表示成阶梯** | 从"零分配静态"到"堆闭包"共五档；判据与"是否顶层"**无关** |
| **3** | **IR 收敛** | call 家族 10 条 → 3 条；名字/值访问统一（附录 A） |

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
4. **call 只有一个**（+ 尾位置 + C 调用）。目标解析方式、caps 布局、ABI 家族
   是**字段**，不是 opcode。
5. **判据必须是结构事实。** 判定"是不是顶层名"只能用"该名字是不是某个顶层
   item 的**直接绑定**"——**不能靠名字形态猜**（`t0` / `_` 这类编译器临时
   会污染结果）。
6. **分层交付。** 每步独立验收，不要求一次性自举通过。

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

### 2.1 布局

顶层每个名字一个 32B 单元，放在 globals 数据区（**不在 GC 堆**）。
函数与值**共用同一布局**，只是使用不同的偏移：

```
cell + 0   : value        ← 顶层 let x 的值        (gval 读这里)
cell + 8   : mark
cell + 16  : entry        ← 顶层 letfun 的入口     (gvar 的值经这里)
cell + 24  : nenv = 0     ← 常量 0
cell + 32… : env…          (本形态下为空)
```

**关键点：`nenv = 0` 让这个单元本身就是一个合法的零捕获闭包对象。** 于是：

- 函数：`gvar` 装 `cell | 1`（tagged）。`call` / 间接调用读 `[+16]` / `[+24]`，
  **通用闭包路径零改动**。
- 值：`gval` 读 `[+0]`。
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
3. **`["local", …]` 必须排在所有 `gset` 之前** —— `local` 才是读 argc/argv、
   建帧、写 GC `stack_hi` 的地方。
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
| `singleton` / `borrowed` / `pair` / `vector` | **需要调用图**：算出每个 lambda 的调用点集合，全部静态可知才算 well-known | 第 6 步 |

**yac 缺的两块：**

1. **调用图 / well-known。** 现在只有 `Σ` / `topfn` 这种"名字表"，没有"调用图"。
2. **lift。** yac 的 caps 是 **boxed env**（调用时从闭包对象槽位取）；
   把自由变量改成**调用时多传参数**（unboxed args）能缩小闭包槽位 ——
   这是进一步降低分配的关键一步。

### 3.4 收益示例

`map(xs, fun(x) -> x + g)` 现在必须 `closure` + `icall`（每次求值分配一个
闭包对象）。`g` 静态化 + `map` 判为 well-known 后，这个 `fun` 命中
`free* = {g}`（1 个）→ **值就是 `g` 本身，零分配**。

---

## 4. call 收敛与 IR 简化

### 4.1 真实的正交维度

现在有 10 条活跃的 call：`fcall` / `xcall` / `icall` / `apply` / `tcall` / `ticall` /
`tailapply` / `ccall` / `iccall` / `$icall`。另有一条**已无生产者**的 `gcall`
（rev1 遗留，见第 6 节清理清单）。

真正的正交维度只有 **3 个**：

| 维度 | 取值 |
|---|---|
| callee 静态性 | 静态名字 / 运行期值 |
| caps 布局 | 无（flat）/ 编译期已知 n / 运行期取 |
| 是否尾位置 | 是 / 否 |

外加**一个不同的 ABI 家族**（C 调用）。3 个维度 + 1 个家族被展开成了 10 个
opcode。被泄漏进 opcode 的还有第四类信息——**linker 的问题**：

- `fcall` / `xcall` 的区别是"名字怎么解析"（本镜像 label / 全局槽 / 跨镜像补丁槽）。
  这是 `lower` / `link` 阶段的事，不该出现在 IR 里。

### 4.2 目标形态：3 条 opcode

```
["call",  dst, target, args, caps]     ; 结果 → dst
["tcall", target, args, caps]          ; 尾位置；无 dst，控制转移（形状不同，独立成条）
["ccall", dst, name, args]             ; 调用 C
```

两个字段取代 opcode：

```
target ::= ["name", nm]    静态名字 —— emit 经 fn_entry(nm) 解析为
                           rel32 / 全局槽 / cell / 补丁槽
         | ["slot", s]     运行期值（栈槽）

caps   ::= ["static", n]   编译期已知 n 个前导 caps；n = 0 即 flat（无前导）
         | ["dyn"]         运行期从 [obj+24] 取
```

这样：

- `xcall` 消失 —— "名字怎么解析"收进 `fn_entry` 策略点（§4.3）
- `icall` / `apply` 合并 —— 区别变成 `caps` 字段（`["dyn"]` vs `["static", n]`）
- `ticall` / `tailapply` 消失 —— 只留 `tcall`
- `iccall` 并入 `ccall`（`ccall` 保留原名，`iccall` 成为它的动态变体）
- `gcall` 消失 —— 就是 `call(target=["name", …])`
- `$icall` 保留 —— 它是 `rt/runtime.yac` 手写 LIR 的内部原语，属 raw 家族

> 注意 `caps` 用带标签的形式而不是 `-1` 这样的魔法整数 —— 现有的
> `apply_ncap` 用 `-1` 表示"动态"，正是"该做成显式字段"的信号。

**前置条件：尾位置必须先变成结构。** 现在 LIR 层用 `maybe_tcall`（`lir.yac:832-839`）
事后把 `fcall` 改写成 `tcall` / `ticall` / `tailapply` —— 这正是"尾位置不是结构"
的直接后果，也是 §4.1 里"tail 变成 3 个 opcode"的根因。

ANF 改成 `body = [binds, tail]`、`tail = ["atom", a] | ["call", f, as]` 之后
（见 `docs/DESIGN.md` §2.1），**尾位置由语法给出**：`maybe_tcall` 可以删除，
`ticall` / `tailapply` 随之消失，`letif` 两支的死代码也一并消失。

### 4.3 `fn_entry` 策略点（OCP 落点）

emit 侧只留**一个**解析名字的地方：

```
fn_entry(name) -> 绝对地址 | 桩槽
  AOT  = 布局期 bake
  yjit = 布局期填
  blob = jsess patch
```

新增镜像形态 = 加一个 `fn_entry` 分支 + 一个填表者，**不动 lir**。

这也是去重的抓手：现在"装一个 callee 地址再间接调用"这段逻辑在 emit 里
**写了三遍**（`gvar` handler、`xcall` handler、`fcall` 的 `via_slot` 分支），
都是 `movabs 0 占位 → 记 patch → tag/store → 间接 call`。合并成
`emit_callee_ref(name) -> 寄存器` 一处即可。

**顺序建议：先做 `fn_entry`，再删 opcode。** 三份重复代码合并之后，
`xcall` 会自然退化成 `call` 的一个分支，而不是被强行删除。

### 4.4 内存访问族（候选合并，差异已核实）

现在有 8 条内存指令：`mref` / `mset` / `mref8` / `mset8` / `ld64` / `st64` /
`obj_sti` / `obj_st_int`。

emit 实现显示它们的**差异只有三个维度**：

| 维度 | 取值 | 证据 |
|---|---|---|
| 偏移形式 | 静态 disp32 / 动态索引（槽号） | `mref` 用 `mov rax,[rax+disp32]`；`ld64` / `mref8` 先从槽取索引再 `sar 1` |
| 访问宽度 | 64 / 8 | `mref8` 用 `movzx`，`ld64` 用 `mov rax,[...]` |
| 结果是否重新打 tag | 是 / 否 | `mref` 直接存（已 tagged）；`mref8` 有 `shl rax,1`；`ld64` 无 |

所以可以合并成 2 条：

```
["mref", dst, obj, off, w, retag]    ; off = imm | 槽号；w ∈ {64, 8}
["mset", obj, off, src, w]
```

`obj_sti` / `obj_st_int` 是 `mset` 的"源是槽 / 立即数"变体，用同一个字段区分即可。

> 这条**不必和第 5 步一起做**：它是独立的重构，验收方式同样是"各阶段 golden 不变"。
> 收益是可维护性，不是性能。

---

## 5. 落地顺序与验收

| 步 | 内容 | 验收 |
|---|---|---|
| **0** | 修好自举链（当前工作树 `yc.exe` 一进 `--ast` 即 SIGSEGV） | `yc` 自编译两遍 + `--ast` 不崩 |
| **1** | 单镜像 flat 基线收尾（顶层函数 flat） | 两遍自举、`make yc-iso`、`tests/compiler` 各阶段 |
| **2** | **顶层值静态化**（§2）：`scan_toplevel` 判据、`gval` / `gset`、发布时序、GC root | 顶层 letfun **全部** `ncap = 0`；LIR dump 里 `gval` 条数 == 顶层 `let` 条数 |
| **3** | **判据下沉**（§1）：`flat ⇔ 真捕获数 == 0`，作用域扩到所有 lambda | 函数体内无捕获的 `fun` 也生成静态 cell；各阶段测试 |
| **4** | **ANF 修正**（`DESIGN.md` §2.1）：尾位置结构化 `body = [bind*, tail]`；`letcallcc` 多值形状；`anf_expr` 兜底改为编译期报错 | 各阶段 golden 不变；删掉 `tail(x)` 谓词与 `maybe_tcall`；不再出现 `ticall`/`tailapply` 生成 |
| **5** | **L2 跨镜像**（§2.4）：cell 共享（jsess patch）+ `fn_entry` 策略点 | link 13 PASS、repl 26/26、`import compiler` + `compile("1+2")` e2e、`blob_len > 0` |
| **6** | **IR 收敛**（§4）：`call` / `tcall` / `ccall` + `caps` 字段 | 每阶段 golden 不变（纯重构：IR 形状变、语义不变） |
| **7** | **调用图 + well-known + lift**（§3.3） | 分配计数下降；`map` / `foldl` + 无捕获回调不再产出 `closure` 指令 |

第 2、3 步做完，"非顶层也能静态化"就成立了。第 7 步才是真正消灭分配的地方。

**暂不列入的：`letrec`（互递归函数组）。** ANF 已预留
`["letrec", [fnbind*], body]`（见 `docs/DESIGN.md` §2.1 「预留：`letrec`」），
但**现在不实现**。它的前置正好是上面几项：顶层名字预扫描（第 2 步）、
`calli` 对非 self 目标发 `tcall`（**跨函数 TCO**，后端 ≤6 参数的兄弟调用已就绪）、
绑定检查放开前向引用。缺了跨函数 TCO，互递归只是"能编译但长链爆栈"。

顶层互递归**不需要** `letrec` —— `scan_toplevel` 放行即可（两个 flat 函数、
入口是编译期常量，组协议是空操作）；真正需要它的只有**嵌套**互递归组。

### 可观测指标

否则"静态化做对了"没有可验证的判据：

- **静态化名字数**（应等于顶层 `letfun` + 顶层 `let` 数）
- **每个 lambda 的真捕获数**（`--dump-lir` 里 `proc` 头的 `ncap`）
- **分配计数**（第 6 步后应下降）

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
| 第 5 步收敛掉的 opcode | `xcall` / `apply` / `ticall` / `tailapply` / `iccall` / `gvld` / `gvst` / `gfnst` |

**能力不删，只改形态**：`apply` / 间接调用的**能力**保留（嵌套闭包 +
call/cc 式动态调用），但不再是独立 opcode —— 变成 `call` 的 `caps = ["dyn"]`。

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
| `⊢ program`（`_start`） | 直接跑各顶层 body | 顶层 `let` 要发 `gpub`（发布）+ 处理前向引用 |

**本轮已完成**：

1. §4.1 与 §2.1 的**两份 ANF 定义已合并为 §2.1 一份**（§4.1 只保留"与经典 ANF
   的差异"）；§2.1 的语法按 §1 更新（尾位置结构化 + `float` / `qvar` +
   `letcallcc` 多值形状 + 兜底报错），§2 的 `letif` / `callι` 规则与 §4.3、
   §7.1 的伪码同步改完。
2. **§2.1 的 LIR 语法已删除**，改为只保留 ANF→LIR 的翻译约定，并指向
   `docs/LIR.md`（LIR 权威定义）。`LIR.md` 同时是一份清点报告：32 条无生产者的
   死 handler、三处静默兜底、`--dump-lir` 不忠实、5 处闭包落点等。

**仍未处理**：上面表格里的三条规则。另注意 `emit_insn_go` 的兜底是 `else st`
（**静默跳过未知指令**）——这正是漂移长期没被发现的原因，也是 §6 清理时要更
谨慎的理由。

### 7.3 DESIGN.md §8.2 需要补一条

§8.2 的根集合列的是 `m->code` / `m->env` / 实参数组 / 原语回调里的临时值
——**没有"globals 区"**，因为原设计里 yac 值不驻留在静态区。

本文引入 cell 后：

> **cell 区必须登记为 GC root。** 且 §8.2 明确"不做保守扫描、只用显式值栈"，
> 所以需要一条**显式的登记路径**，不能指望被扫到。

---

# 附录 A：目标 IR 摘要（**权威定义见 `docs/LIR.md`**）

> **本文不是 LIR 的权威定义。** `docs/LIR.md` 是唯一权威：完整指令集、逐条标注
> 生产者、`$` 家族规格、定位与不变量、后端契约、校验规则、一致性问题清单。
>
> 下面只保留**与 flat / 静态化相关的那部分摘要**，方便在本文内对照 §4 的收敛方案。
> 指令的完整语义与现状**以 `docs/LIR.md` 为准**。

语法：指令是 list，首元素是名字。`s` = 栈槽号，`imm` = 立即数，`lbl` = label，
`nm` = 名字（字符串），`[x*]` = 列表。

程序结构：

```
["prog", [proc*], entry]
["proc",  name, nparams, ncap, [insn*], srcname]    ; ncap = 真捕获数
["$proc", name, nparams, ncap, [insn*], srcname]    ; 原生过程（rt/runtime.yac 手写）
```

## A.1 帧与栈

| 指令 | 形态 | 说明 |
|---|---|---|
| `local` | `["local", nslots, nparams]` | 建帧、读 argc/argv、写 GC `stack_hi`。**必须排在所有 `gset` 之前** |
| `$local` | `["$local", nslots, nparams]` | 原生帧（`$proc` 或首条 `$local` → 原生槽） |
| `$sp` | `["$sp", dst, k]` | 栈指针相对取址 |
| `$fp` | `["$fp", dst, k]` | 帧指针相对取址 |
| `$carg` | `["$carg", dst, i]` | 取 C 调用参数 |
| `$smap` | `["$smap", …]` | 栈图（GC 扫描用） |

## A.2 搬运与算术

| 指令 | 形态 |
|---|---|
| `mov_imm` | `["mov_imm", dst, imm]` |
| `mov` | `["mov", dst, src]` |
| `add` / `sub` / `mul` / `div` / `rem` | `["add", dst, a, b]` |
| `shl` / `sal` / `shr` / `sar` | `["shl", dst, a, b]` |
| `land` / `lor` / `xor` / `bnot` | `["land", dst, a, b]` |
| `$and` / `$or` / `$add` / `$addi` / `$sub` / `$shr` | raw 算术（不做 tag 处理） |
| `$lea` | `["$lea", dst, base, off]` |
| `$clamp0` | 夹到 0 |
| `$bt` / `$bts` | 位测试 / 置位 |

## A.3 比较与分支

| 指令 | 形态 | 说明 |
|---|---|---|
| `cmp` | `["cmp", op, dst, a, b]` | 带 tag 的数值比较，`op` ∈ `< <= > >= == !=` |
| `icmp` | `["icmp", op, dst, a, b]` | 整数比较 |
| `$icmp` | raw 整数比较 |
| `label` | `["label", lbl]` | |
| `jmp` | `["jmp", lbl]` | |
| `cmpjmp` | `["cmpjmp", cond, then, else]` | 条件为 0 跳 `else` |
| `$jcc` | raw 条件跳转 |

## A.4 调用（目标 3 条 + 1 raw）

| 指令 | 形态 | 说明 |
|---|---|---|
| `call` | `["call", dst, target, args, caps]` | `target` = `["name", nm]` 或 `["slot", s]` |
| `tcall` | `["tcall", target, args, caps]` | 尾位置，无 `dst` |
| `ccall` | `["ccall", dst, name, args]` | 调用 C；`iccall` 的形态是其动态变体 |
| `$icall` | `["$icall", dst, slot]` | raw 内部原语：直接 `call` 槽内指针，不做解包 |

```
caps ::= ["static", n]   ; n = 0 即 flat（无前导 caps）
       | ["dyn"]         ; 运行期 nenv，从 [obj+24] 取，caps 从 [obj+32+i*8] 取
```

## A.5 名字单元（静态化）

| 指令 | 形态 | 说明 |
|---|---|---|
| `gvar` | `["gvar", dst, nm]` | `dst = cell \| 1`（tagged）—— 函数当值用 |
| `gval` | `["gval", dst, nm]` | `dst = [cell + 0]` —— 读顶层值 |
| `gset` | `["gset", nm, off, src]` | `[cell + off] = src`，`off` ∈ `{0: 值, 16: 函数入口}` |

`gset` 同时取代旧 `gvst`（值）与 `gfnst`（函数入口）—— 两者只是 `off` 不同。

## A.6 闭包与对象

| 指令 | 形态 | 说明 |
|---|---|---|
| `closure` | `["closure", dst, fnName, [capSlots]]` | 堆闭包：`[next][mark][fnptr][nenv][env…]` |
| `alloc` | `["alloc", dst, bytes]` | 分配（GC 堆） |
| `alloc_s` | `["alloc_s", dst, bytes]` | 分配（跳过 GC 链表登记） |
| `obj_kind` | `["obj_kind", dst, obj]` | 取对象种类 |
| `obj_sti` | `["obj_sti", obj, off, src]` | 存字段（槽号源） |
| `obj_st_int` | `["obj_st_int", obj, off, imm]` | 存字段（立即数源） |
| `mref` | `["mref", dst, obj, off]` | 读字段 |
| `mset` | `["mset", obj, off, src]` | 写字段 |
| `mref8` / `mset8` | `["mref8", dst, obj, off]` | 按 8 位访问 |
| `is_int` | `["is_int", dst, v]` | 整数判定 |
| `tag` / `untag` | `["tag", dst, src]` / `["untag", dst, src]` | 打 / 去 tag |

## A.7 原生内存与系统

| 指令 | 形态 |
|---|---|
| `ld64` / `st64` | `["ld64", dst, addr]` / `["st64", addr, src]` |
| `$ld64` / `$st64` / `$ld8` / `$st8` | raw 64 / 8 位存取 |
| `$memcpy` / `$memset` / `memcpy` | 内存块操作 |
| `write1` | 写一个字节到 fd |
| `syscall` / `$syscall` | 系统调用 |
| `$glob` / `$gbase` | globals 区基址 / 偏移 |

## A.8 内联原语

| 类别 | 指令 |
|---|---|
| 列表 | `nil` `cons` `len` `nth` `tail` `append` `drop` `list_new` `list_push` `list_rev` `map` `foldl` |
| 字符串 | `strlit` `str_len` `str_ref` `str_cat` `str_slice` `int_to_str` |
| bytes | `bytes_new` `bytes_len` `bytes_ref` `bytes_put` `bytes_append` `bytes_extend` `bytes_to_str` |
| 浮点 | `$f64fromstr` `$f64binop` `$f64rel` `$f64print` |

> 这些是"内联原语"：语义上是过程调用，但 emit 直接展开，不走 `call`。
> 若某条最终没有内联实现，就退化成 `call(target=["name", "yac_xxx"])`。

## A.9 控制与运行时

| 指令 | 形态 | 说明 |
|---|---|---|
| `ret` | `["ret", slot]` | 返回（entry 过程是裸 `ret`，普通过程补 `leave`） |
| `exit` | `["exit", slot]` | 进程退出（`sar rax,1` 后走 OS） |
| `throwk` | `["throwk", k, v]` | 抛给续延 |
| `mkcont` / `cc_recv` | | 一等续延 |
| `clock` / `time_ms` / `time_str` | `["time_ms", dst]` | 时间 |
| `glob` / `gst` | | globals 读写 |
| `argc` / `argv` | `["argc", dst]` / `["argv", dst, i]` | 命令行 |
| `read_file` / `write_file` | | 文件 |

---

# 附录 B：指令对照表（旧 → 新）

| 旧 | 新 | 说明 |
|---|---|---|
| `fcall` | `call(target=["name", nm], caps=["static", 0])` | 同镜像静态调用 |
| `xcall` | `call(target=["name", nm], …)` | 名字解析交给 `fn_entry`（跨镜像 → 补丁槽） |
| `icall` | `call(target=["slot", s], caps=["dyn"])` | 运行期值，动态 nenv |
| `apply` | `call(target=["slot", s], caps=["static", n])` | 运行期值，caps 数编译期已知 |
| `tcall` / `ticall` / `tailapply` | `tcall` | 三条合一条，caps 走字段 |
| `ccall` / `iccall` | `ccall` | `ccall` 保留原名，`iccall` 为其动态变体 |
| `gcall` | `call(target=["name", nm], …)` | 消失 |
| `$icall` | `$icall` | **保留**（raw 家族，`rt/runtime.yac` 手写） |
| `gvar` | `gvar` | **不变** |
| `gvld` | `gval` | 改名（读 `[cell+0]`） |
| `gvst` | `gset(nm, 0, src)` | 合并进 `gset` |
| `gfnst` | `gset(nm, 16, src)` | 合并进 `gset` |
| `mref8` / `ld64` | `mref(dst, obj, off, w, retag)` | 宽度 / 偏移形式 / 是否重打 tag 走字段（§4.4） |
| `mset8` | `mset(obj, off, src, w)` | 同上 |
| `obj_sti` | `mset(obj, off, src, w)` | 源是槽的变体 |
| `obj_st_int` | `mset(obj, off, src, w)` | 源是立即数的变体 |

**结果：**

| 家族 | 现在 | 目标 |
|---|---|---|
| call | 10 条（+ 1 条死代码 `gcall`） | **3 条**（`call` / `tcall` / `ccall`） |
| 名字单元 | 4 条（`gvar` / `gvld` / `gvst` / `gfnst`） | **3 条**（`gvar` / `gval` / `gset`） |
| 内存访问 | 8 条 | **2 条**（`mref` / `mset`） |

全部改动都是**纯重构**：IR 形状变、语义不变，验收方式统一为"各阶段 golden 不变"。
