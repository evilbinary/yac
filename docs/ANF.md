# ANF.md — ANF 权威定义（语法 + ANF → LIR）

> **本文是 ANF 的唯一权威定义**：语法、不变量、ANF → LIR 的转换规则。
> `DESIGN.md` §2.1 与 §4 的 ANF 部分已改为指向本文。
>
> 链条：`AST/AST.md → ANF.md → LIR.md → 机器码`
> 每份文档 = **本层语法 + 到下一层的转换**。

## 0. 定位

ANF（A-Normal Form）是**求值顺序显式化**的 IR：把"先算什么、后算什么"编码进
`let` 绑定。它的唯一职责就是这件事 —— **不谈闭包、不谈环境、不谈机器**。

一个表达式 = **绑定序列 + 尾**；顶层程序 = 这种 body 的列表。

```
body      ::= [ bind*, tail ]
```

## 1. 语法（权威）

```
body      ::= [ bind*, tail ]

tail      ::= ["atom", atom]              -- 返回值就是这个原子
            | ["call", atom, [atom*]]     -- 尾调用：TCO 在这里是**结构**，不是事后辨认

atom      ::= ["int",   digits]
            | ["float", digits]
            | ["str",   bytes]
            | ["bool",  "true" | "false"]
            | ["unit"]
            | ["nil"]
            | ["var",   name]
            | ["qvar",  pkg, name]        -- import P as a 之后的 a.x

bind      ::= ["let",       name, atom]
            | ["letbin",    name, op, atom, atom]
            | ["letcall",   name, atom, [atom*]]
            | ["letif",     name, atom, body, body]
            | ["letfun",    name, [name*], body]
            | ["letthrow",  name, atom, atom]
            | ["letcallcc", [k, r], atom, body]
            | ["letrec",    [fnbind*], body]      -- 预留（见 §5.3）：互递归函数组

fnbind    ::= ["letfun", name, [name*], body]

op        ::= "+" | "-" | "*" | "/" | "%"
            | "==" | "!=" | "<" | "<=" | ">" | ">="
            | "and" | "or"
```

`letfun` 的 body、`letif` 的两支都是完整 `body`（可再嵌套），因此**都可以以
`["call", …]` 结尾** —— 尾位置由结构决定。`print e` 在 parser 里降成
`call print(e, true)`。

**`atom` 不含函数字面量**（理由见 §5.1）。

## 2. 不变量

| # | 不变量 | 现状 |
|---|---|---|
| 1 | `atom` 的求值是**空操作**（变量、字面量、nil）；不含需要分配/计算的东西 | ✅（§5.1 删掉 `fun` atom 后成立） |
| 2 | 每个非原子计算**恰好绑定一次**到一个名字 | ✅ |
| 3 | `bind` 的右侧操作数**全是 `atom`** | ✅ |
| 4 | 求值顺序 = `binds` 的书写顺序，然后 `tail` | ✅ |
| 5 | `tail` 只有两种形态；**尾位置是结构**，不是"事后辨认的模式" | ⚠️ 本次修正 |
| 6 | 顶层程序 = `body` 的列表，一个顶层 item 一个 body | ✅（`anf_all`） |
| 7 | 无独立 `letrec` 时，递归只能靠**名字自引用**（`letfun` 绑到自己名字上） | ✅ |
| 8 | **未知 bind / 未知 atom 必须报错**，不得静默跳过 | ❌ 见 §4.2 |

第 5 条是本次（相对旧版）最实质的修正，见 §4.1。

## 3. ANF → LIR

产出的是 `docs/LIR.md` 定义的 LIR。环境 `Γ : name → slot`；当前过程名 `self`
（`_start` 或某个 `letfun` 的码名）；已定义过程 `Σ`（名 → `{ncap}`）。
`s*` 表示 fresh 槽；`I · J` 是指令拼接；`ε` 是空序列。

**判断**：

```
Γ ⊢ atom  ⇒  s  ▹  I
Γ ⊢ tail  ⇒  s  ▹  I  ▹  proc*        ; ["atom", a] 或 ["call", f, as]
Γ ⊢ bind  ⇒  Γ' ▹  I  ▹  proc*
self ; Γ ⊢ body  ⇒  s  ▹  I  ▹  proc*
⊢ program  ⇒  prog
```

### 3.1 原子

```
Γ ⊢ ["int",  n]              ⇒  s  ▹  [["mov_imm", s, n<<1]]
Γ ⊢ ["float", f]             ⇒  s  ▹  [["strlit", s, f], ["fcall", s, "yac_f64_from_str", [s]]]
Γ ⊢ ["bool", "true"]         ⇒  s  ▹  [["mov_imm", s, 2]]
Γ ⊢ ["bool", "false"]        ⇒  s  ▹  [["mov_imm", s, 0]]
Γ ⊢ ["unit"]                 ⇒  s  ▹  [["mov_imm", s, 0]]
Γ ⊢ ["nil"]                  ⇒  s  ▹  [["mov_imm", s, 1]]
Γ ⊢ ["str",  b]              ⇒  s  ▹  [["strlit",  s, b]]
Γ ⊢ ["var",  x]              ⇒  Γ(x)  ▹  ε
; atom 不含函数字面量 —— 它在 anf.yac 里就降成 ["letfun", tN, ps, body] + ["var", tN]
```

`["qvar", pkg, nm]` 先由 `qvar_key` 解析成 LIR 名 `pkg/nm`，再按 `["var", …]` 处理
（查 `Σ`）。

### 3.2 绑定

```
Γ ⊢ ["let", x, a]  ⇒  Γ[x ↦ s]  ▹  I  ▹  ∅
  where  Γ ⊢ a  ⇒  s  ▹  I
```

> **别名是免费的**：`["let", "y", ["var", "t"]]` 走 `["var"]` 规则，`s = Γ(t)`，
> 于是 `y` 与 `t` **共享同一个槽**，不分配、不发指令。这就是 ANF 里
> `let y = t0` 这类别名不产生开销的原因。

```
Γ ⊢ ["letbin", x, op, a, b]  ⇒  Γ[x ↦ s]  ▹  Iₐ · Iᵦ · [ι]  ▹  ∅
  where  Γ ⊢ a  ⇒  sₐ  ▹  Iₐ
         Γ ⊢ b  ⇒  sᵦ  ▹  Iᵦ
         ι = bin(op, s, sₐ, sᵦ)

bin("+",s,a,b)  = ["fcall", s, "yac_num_add", [a, b]]
bin("-",s,a,b)  = ["fcall", s, "yac_num_sub", [a, b]]
bin("*",s,a,b)  = ["fcall", s, "yac_num_mul", [a, b]]
bin("/",s,a,b)  = ["fcall", s, "yac_num_div", [a, b]]
bin("%",s,a,b)  = ["fcall", s, "yac_num_rem", [a, b]]
bin("and",s,a,b)= ["land", s, a, b]          ; 不短路
bin("or",s,a,b) = ["lor",  s, a, b]          ; 不短路
bin(cop,s,a,b)  = ["cmp",  cop, s, a, b]     ; cop ∈ {==,!=,<,<=,>,>=}
```

> ⚠️ `+ - * /` 现在每次都走一次运行时过程调用（→ 见 `LIR.md` §11 C2，将来要做
> 内联的 int 快路径 + 溢出检查）。

```
Γ ⊢ ["letcall", x, print, [v, nl]] 走普通 fcall（runtime `print`）。
```

### 3.3 `letif`（join point，无死代码）

```
Γ ⊢ ["letif", x, c, bodyₜ, bodyₑ]  ⇒  Γ[x ↦ s]  ▹  I  ▹  Pₜ ∪ Pₑ
  where  Γ ⊢ c  ⇒  s_c  ▹  I_c
         self ; Γ ⊢ bodyₜ  ⇒  sₜ  ▹  Iₜ  ▹  Pₜ
         self ; Γ ⊢ bodyₑ  ⇒  sₑ  ▹  Iₑ  ▹  Pₑ
         join(B, s_b) = [["mov", s, s_b], ["jmp", L]]  if  B = [_, ["atom", _]]
                      | ε                              if  B = [_, ["call", _, _]]
         I = I_c ·
             [["cmpjmp", s_c, Lₜ, Lₑ],
              ["label", Lₜ]] · Iₜ · join(bodyₜ, sₜ) ·
              ["label", Lₑ]] · Iₑ · join(bodyₑ, sₑ) ·
              ["label", L]]

; 分支以 ["call", …] 结尾时控制已经转移，join 是死代码 → 直接省略。
; （旧版无条件发 mov/jmp，那些指令永远不会执行。）
```

**为什么 `letif` 是一个 `bind` 而不是尾形式。** 经典 ANF 的 `if` 只在尾位置，
要"取一个条件表达式的值"必须复制后续代码：
`if A then (let x = … in E) else (let x = … in E)`。`letif` 天然是 join point，
**这是 ANF 相对经典形式最实质的一处偏离**（见 §6）。

### 3.4 调用

```
Γ ⊢ ["letcall", x, f, as]  ⇒  Γ[x ↦ s]  ▹  I_f · I_as · [ι]  ▹  ∅     ; 非尾
  where  Γ ⊢ f          ⇒  s_f  ▹  I_f
         Γ ⊢ as_i       ⇒  s_i  ▹  I_i     （逐参）
         I_as = I_0 · … · I_{n-1}
         ι   = callι(self, #f, f, s, s_f, [s_i])

Γ ⊢ ["call", f, as]  ⇒  _  ▹  I_f · I_as · [ι]  ▹  ∅                  ; 尾位置
  where  Γ ⊢ f          ⇒  s_f  ▹  I_f
         Γ ⊢ as_i       ⇒  s_i  ▹  I_i
         ι   = callι(self, #t, f, s, s_f, [s_i])
```

```
callι(self, tail?, ["var", g], s, _, ss) =
    ["tcall",  s, ĝ, cap·ss] if  tail? ∧ ĝ = self
  | ["fcall",  s, ĝ, cap·ss] if  ĝ = self ∧ n > 0 ∧ (tail? ∨ |cap·ss| ≤ 6)
  | ["fcall",  s, ĝ, ss]     if  g ∈ Σ ∧ n = 0
  | ["fcall",  s, rt(g), ss] if  g 是 runtime 名   ; 在 Σ / env 之后，避免遮蔽 let len
  | ["ccall",  s, name, ss]  if  g = ccall 且首参是字符串字面量
  | ["iccall", s, s_f, ss]   if  g = ccall 且首参不是字符串字面量
  | ["apply",  s, Γ(g), n, ss] if  Γ(g) 有已知 ncap = n > 0
  | ["icall",  s, Γ(g), ss]  otherwise        ; 槽里是闭包，nenv 运行时读
  where ĝ = Σ 中 g 的码名（重名加 #uid）
        n   = Σ(g).ncap
        cap = [1..n]                         ; 当前帧捕获槽，self 调用要原样传入

callι(self, tail?, f, s, s_f, ss) =
    ["ticall", s, s_f, ss]   if  tail? ∧ f 是 self 的闭包槽 ∧ |ss| ≤ 6
  | ["icall",  s, s_f, ss]   otherwise

rt("cons")="yac_cons"  rt("nth")="yac_nth"   rt("len")="yac_len"
rt("foldl")="yac_foldl"  rt("map")="yac_map"  rt("argc")="yac_argc"  …
; runtime 名以 yac_* / time_* / gc_collect / argc / argv / print_val 为准
```

**尾位置由结构给出**：`tail?` 为真当且仅当该调用出现在 `tail` 非终结符里
（即 `["call", f, as]`）。因此旧的 `tail(x)` 谓词（"最后一条 `letcall` + 尾原子是
它"）**不再需要**，LIR 层的 `maybe_tcall` 事后改写**也不再需要**
（见 `LIR.md` §5.5 S1）。

TCO 规则：只对 self 做 TCO；`ccall`（C）不做 TCO。self `tcall` 在 prologue 之后的
`$tco` 回跳（槽搬运，不拆帧），arity 不限。命名 self 走第一条（`fcall`/`tcall` +
捕获槽），**不要**把所有尾 `icall` 收成 `ticall` —— `twice(f,x)=f(f(x))` 会错。

> ⚠️ **现状 vs 目标**：上面的 `callι` 产出的是 `LIR.md` §5.1 的**现状**形态
> （`fcall`/`xcall`/`icall`/`apply`/`ticall`/`tailapply`）。指令集收敛（`LIR.md`
> §5.1）之后，这张表会简化成 `call` / `tcall` / `ccall` + `caps` 字段：
> `fcall`/`xcall` → `call(["name",·])`；`icall` → `call(["slot",·], caps=["dyn"])`；
> `apply` → `call(["slot",·], caps=["static",n])`；`ticall`/`tailapply` → `tcall`。
>
> 另：`xcall` 现在是"Σ 里找不到名字"的兜底，这会把**同单元的前向引用**误判为
> 跨镜像引用。修法见 `LIR.md` §9.3（`topfn_has` 应作为参数传入，`calli` 补分支）。

### 3.5 `letfun`

```
Γ ⊢ ["letfun", f, ps, body]  ⇒  Γ[f ↦ s]  ▹  I_out  ▹  {proc} ∪ P
  where  caps = FV(body) \ ({f} ∪ ps)                    ; ← fvs：捕获清单
         ncap = |caps|
         Γ_f  = { caps_i ↦ ["cap", i] } ∪ { ps_j ↦ j+1 }   ; capRef：捕获不占槽（LIR.md §4.4.7）
                ∪ (ncap>0 ∧ f ∈ FV(body)  ?  {f ↦ ncap+|ps|+1}  :  ∅)
         f ; Γ_f ⊢ body  ⇒  s_r  ▹  I_b  ▹  P
         proc = ["proc", f, ncap+|ps|, ncap,
                 [["local", N, ncap+|ps|]]
                 · (ncap>0 ∧ f ∈ FV(body)
                      ? [["closure", Γ_f(f), f, [1..ncap]]]     ; 自引用闭包
                      : ε)
                 · I_b
                 · [["ret", s_r]],
                 f]
         I_out = [["closure", s, f, [Γ(caps_i)]]]
                 ; ncap=0 也分配闭包：函数当值（map/filter）时槽里必须是闭包。
                 ; 按名调用走 fcall，不读这个槽。不做「只按名」分析。
```

**槽布局**：`1..ncap` 捕获（由调用方按前导参数传入），`ncap+1..ncap+|ps|` 形参，
之后是局部。`N` 是本过程用到的最大槽号。

**`caps`（= `fvs`）是本节的核心**：它是"这个函数捕获了哪几个名字"。目前
`lir.yac` 算出来只用一次就丢掉，`proc` 里只剩一个数字 `ncap`。轻量版改动是把
它落进 `proc` 的末尾（见 `LIR.md` §6）。

> ⚠️ **`I_out` 与 `flat` 的关系**：现在是 `flat = topfn_has(f) and ncap == 0` ——
> flat 时不发 `closure`、不 `env_bind`。**目标**是把 `ncap` 的语义从"自由变量总数"
> 改成"**真捕获数**"（顶层函数名、顶层值名都不计入），于是**所有顶层 letfun 恒
> flat**。前提是顶层值静态化（`FLAT_ABI.md` §2）。

### 3.6 body / 程序

```
self ; Γ ⊢ [ b1, …, bn ], tail  ⇒  s  ▹  I₁ · … · Iₙ · I_tail  ▹  P₁ ∪ … ∪ Pₙ
  where  Γ ⊢ b1  ⇒  Γ₁ ▹ I₁ ▹ P₁
         Γ₁ ⊢ b2 ⇒  Γ₂ ▹ I₂ ▹ P₂
         …
         Γₙ ⊢ tail  ⇒  s  ▹ I_tail

⊢ [body₁, …, bodyₘ]  ⇒  ["prog", [_start] · runtime · procs, "_start"]
  where  _start ; ∅ ⊢ body₁;…;bodyₘ  ⇒  s  ▹  I  ▹  procs
         _start = ["proc", "_start", 0, 0,
                   [["local", N, 0]] · I ·
                   [["untag", t, s], ["syscall", _, 60, [t]]],
                   "_start"]
```

`runtime` 是 `yac_*` 等过程，**不从 ANF 来**。内部 yac ABI：x86_64 前 6 个寄存器
其余入栈；arm64/riscv64 前 8 个寄存器其余入栈（见 `LIR.md` §7.4）。

**顶层关键性质**：所有顶层 item 编进**同一个 `_start` 过程**，`Γ` 线性向下传。
所以顶层 `let x = 5` 的"静态"实现是 **`_start` 的一个 frame slot**，不是 globals 区
—— 这正是"顶层函数必须捕获它"的原因，也是 `FLAT_ABI.md` §2 要改的那一级。

### 3.7 例子

```
源:   let x = 1 in x + 2

ANF:  [[["let", "x", ["int", "1"]],
        ["letbin", "t", "+", ["var", "x"], ["int", "2"]]],
       ["atom", ["var", "t"]]]

LIR:  ["proc", "_start", 0, 0,
       [["local", 4, 0],
        ["mov_imm", 1, 2],          ; 1<<1
        ["mov_imm", 2, 4],          ; 2<<1
        ["add", 3, 1, 2],
        ["untag", 4, 3],
        ["syscall", 0, 60, [4]]],
       "_start"]
```

（注：现状 `+` 走 `fcall yac_num_add`；上例用 `add` 是为了展示形状。`x` 与 `t`
共享槽的话槽号会更少 —— 槽复用尚未实现，见 `LIR.md` §11 C1。）

## 4. 本次修正（相对旧版 ANF）

### 4.1 尾位置结构化

旧版把"最后一条 `letcall` + 尾原子 `["var", x]`"拼起来当尾调用（`tail(x)` 谓词），
代价有两个：

1. **`letif` 的两支总带一段永不执行的死代码** —— 分支体以 `tcall` 结尾时，其后
   的 `mov` / `jmp` 不可达。
2. **尾位置被泄漏进 LIR opcode** —— 要在 LIR 层用 `maybe_tcall` 事后把 `fcall`
   改写成 `tcall`，于是多出 `ticall` / `tailapply` 两个 opcode。

改成 `tail` 非终结符后，**谓词和 `maybe_tcall` 一起消失**。

### 4.2 兜底不再是假 bind

旧版对未处理的 AST 节点返回 `[["error","anf"], ["unit"]]` —— 一条**假 bind**。
而 `lir_expr_i` 对未知 bind 是 `else lir_expr_i(ast, pack_i(st, i + 1))`，
**静默跳过**。两者合起来会**静默产出一段错误代码而不报错**。

改成编译期报错；同时 `lir_expr_i` / `lir_atom` 的兜底也要改成报错
（`LIR.md` §9.1）。

### 4.3 `letcallcc` 的多值形状

`letcallcc` 绑**两个**名字（`k` 续延、`r` 结果），旧版
`["letcallcc", k, r, f, body]` 是整套语法里唯一"一个 bind 绑两个名字"的例外。
改成 `["letcallcc", [k, r], f, body]`，让"绑了几个名字"在结构上可见。

### 4.4 补进 `float` / `qvar`

`anf.yac` 一直在产出这两种 atom，旧文档漏了。

## 5. 现状与待办

### 5.1 删掉 `["fun", [name*], body]` atom（已决定）

三个理由：

1. **违反 ANF 的核心不变式。** `atom` 的定义是"求值空操作"，而函数字面量在会
   分配闭包的编译器里需要**一次分配** —— 那是计算。旧版的原子判断自己也承认这
   点：`Γ ⊢ ["fun", ps, body] ≡ Γ ⊢ ["letfun", x, ps, body] ; ["var", x]`，
   即当场把它降级。
2. **它从不出现。** `anf.yac` 的两条路径（`anf_atom`、`anf_expr` 的顶层 `fun`
   分支）都把 `fun` 降成 `["letfun", tN, ps, body]` + `["var", tN]`，所以
   `["fun", …]` 在 ANF 输出里**不可达**。
3. **三个消费者对它不一致**：

| 消费者 | 对 `["fun", …]` |
|---|---|
| `anf.yac`（生产者） | 不产出 |
| `free_vars`（`lir.yac`） | 处理了，但**有 bug**：不进新 shadow 作用域，嵌套 `fun` 的形参会被当成自由变量 |
| `lir_atom`（`lir.yac`） | **不处理** → 落进 `else`，**静默返回槽 0** |

糖已经在 **AST 层**有了（`expr ::= fun (name*) -> e`），`anf.yac` 直接降成
`letfun`。在 ANF 层再留一份没有自由变量信息的裸 `["fun", ps, body]` 是重复的。

> 将来若做显式闭包层（`clos`），函数字面量应当在**新层**以带自由变量表的形式
> 出现，例如 `["fun", [fv*], [ps], body]` —— 而不是在 ANF 层留裸形式。

### 5.2 代码待改（3 处）

| 文件 | 改什么 | 风险 |
|---|---|---|
| `front/anf.yac` | `anf_expr` 产出 `[bind*, tail]`；`letcallcc` → `[k, r]`；兜底改报错 | 中 |
| `front/lir.yac` | `lir_expr_i` 处理 `tail`；`lir_letif_finish` 去死代码；删 `maybe_tcall` | 中 |
| `front/lir.yac` | 删 `free_vars` 的 `fun` 分支；`lir_atom` / `lir_expr_i` 兜底改报错 | 低 |
| `src/anf.c` / `src/eval_anf.c` | ⚠️ **C 解释器也消费 ANF**，形状变了要一起改，否则两个实现分叉 | 高 |

最后一条是关键决策点：**要么两个实现一起改，要么明确接受临时分叉**。

### 5.3 预留：`letrec`（互递归函数组）

**语法已预留，`anf.yac` 暂不产出。** `letrec` 把一组函数绑成**互相可见**的组 ——
组内任意成员可以直接引用其它成员，**包括向后引用**：

```
["letrec", [["letfun", "even", [n], B₁],
            ["letfun", "odd",  [n], B₂]],  body]
```

契约：

| 约束 | 说明 |
|---|---|
| 成员**必须都是函数** | 这是 `letrec`（而非 `letrec*`）的关键限制：只有函数才能"先分配、后填自由变量"。非函数成员一律拒绝 |
| 组内互相引用**不算捕获** | `capsᵢ = FV(Bᵢ) \ ({f₁…fₙ} ∪ psᵢ)` —— 这是 `letrec` 唯一真正新增的语义 |
| **先全部分配，再填槽** | 组内所有闭包先一次性建出来（自由变量槽未初始化），再回填互相引用的部分。**不允许**读到未初始化的成员 |
| 空组等价于无操作 | `["letrec", [], body]` ≡ `body` |

**为什么只是"预留"而不是实现。** 前置有三件（见 `FLAT_ABI.md` §5）：顶层名字
预扫描（`scan_toplevel`）、`calli` 对非 self 目标发 `tcall`（**跨函数 TCO**，后端
≤6 参数的兄弟调用已就绪）、绑定检查放开前向引用。缺了跨函数 TCO，互递归只是
"能编译但长链爆栈"。

**顶层会退化。** 若组内成员都是顶层 flat 函数（真捕获数 = 0），**根本不需要组
协议**：名字在顶层注册表里、入口是编译期常量。所以 `letrec` 真正有用的场合是
**嵌套**的互递归组；顶层互递归应当由 `scan_toplevel` 直接放行，不经 `letrec`。

**消费端必须报错。** `lir_expr_i` 现在对未知 bind 是静默跳过。既然语法预留了
`letrec`，消费端要么实现它，要么**明确报错**，不能落进静默分支。

## 6. 与经典 ANF 的差异

文献里的经典 ANF（Flanagan et al.）是：

```
Atom  A ::= x | lit | prim
Exp   E ::= halt A
        |  let x = call(A, A*) in E
        |  let x = prim(p, A*) in E
        |  let x = A in E
        |  let f = λ(x*).E in E
        |  letrec f = λ(x*).E in E
        |  if A then E else E
        |  call(A, A*)
        |  prim(p, A*)
        |  A
```

yac 的三处**有意**偏离：

| 经典 | yac | 理由 |
|---|---|---|
| `let x = call/prim/A in E` + 独立的尾形式 | `bind` 的种类就是运算种类（`letbin` / `letcall` / `let`） | ANF→LIR 变成纯 kind-dispatch，不需要判断"这个 callee 是不是原语" |
| `if A then E else E`（**只在尾位置**） | `["letif", x, cond, body, body]` —— 一个 bind | 经典形式下"取一个条件表达式的值"必须复制后续代码：`if A then (let x=… in E) else (let x=… in E)`。`letif` 天然是 join point。**这是最实质的一条** |
| `halt A` 作为显式出口 | `body = [bind*, tail]`，`tail = ["atom", a] \| ["call", f, as]` | 出口就是 body 的尾；同时让**尾位置结构化** |

**没有独立的 `letrec`**：单递归由 `letfun` + 按名自引用表示；互递归的
`["letrec", [fnbind*], body]` 已预留（§5.3）。

## 7. 与 Chez 的层对照

| Chez（`s/cpnanopass.ss`） | yac |
|---|---|
| `cpnanopass` Lsrc→L1、`np-recognize-let` L1→L2、`np-discover-names` →L3 | **无对应层** —— yac 从 AST 一步到 ANF（`anf.yac`） |
| `np-convert-assignments` L3→L4（把被赋值的变量装箱） | **无**（yac 无赋值；别名的槽共享已由 `["let",y,["var",t]]` 规则处理） |
| L4.5 `np-recognize-mrvs`（多值） | **无** |
| L4.875 `np-recognize-loops`（`loop` 形式） | **无** |
| L4.9375 `np-recognize-attachment`（续延附件） | 对应 yac 的 `letcallcc` / `letthrow` |
| L5 `lambda`，**无自由变量信息** | ANF 的 `letfun`，**无 fv 信息** ✅ 同一位置 |
| `np-convert-closures` L5→L6（引入显式 `closures`） | **无对应层** —— 压进 `lir_letfun_*`（见 `LIR.md` §6） |
| `np-expand/optimize-closures` L6→L7（决定闭包表示） | 一行 `flat = topfn_has(name) and ncap == 0` |

**yac 的 ANF 大致相当于 Chez 的 L4.9–L5**：赋值已消（本来就不可变）、`letrec`
尚未展开、闭包尚未显式化。
