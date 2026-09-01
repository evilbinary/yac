# Yac — 一个可运行 ANF 与 CPS 的语言设计

## 1. 目标

Yac 是一个小型、纯粹的函数式语言，其核心设计目标是**在两种显式求值顺序的中间表示（IR）上直接运行**：

- **ANF（A-Normal Form）**：把求值顺序编码进 `let` 绑定，控制流仍然隐式。这是"直接风格"的机器。
- **CPS（Continuation-Passing Style）**：把求值顺序编码进续延（continuation），每个函数多收一个续延参数，控制流完全显式。这是"全控制风格"的机器。

同一个源程序可以被翻译成 ANF 或 CPS，并分别由两个解释器执行。二者共享同一套值表示与语义，唯一的语义差异是：

> **ANF 解释器无法表达一流续延（callcc）；CPS 解释器可以。**

这给出了一条贯穿全文的主线：*控制流在 ANF 里是"语法结构"，在 CPS 里是"数据"*。CPS 里续延只是普通的值，因此 `callcc` 几乎不需要额外机制——它只是把"当前续延"当作参数交给函数。

### 设计原则

1. **求值顺序显式化**：程序里"先算什么、后算什么"在 IR 里一目了然。
2. **尾调用即跳转**：两个解释器都不让 C 调用栈增长，长尾递归不会爆栈。
3. **单一值表示**：ANF 与 CPS 共用一套 `Value` 结构，解释器之间可以互相转换运行。
4. **可验证**：对同一程序，`evalANF(anf(p)) == evalCPS(cps(anf(p)))`，用属性测试保证翻译正确性。
5. **用 C 实现**：作为系统语言，C 要求我们把"堆对象、闭包、续延、GC"全部显式设计出来，正适合作为教学/原型核心。



## 2. 总体架构

```
                 ┌──────────┐    ┌─────────────┐    ┌───────────┐    ┌─────────────┐
  源程序  ──────▶│ lexer    │───▶│ parser      │───▶│ AST       │───▶│ ANF 归一化  │───┐
                 └──────────┘    └─────────────┘    └───────────┘    └─────────────┘   │
                                                                                       ▼
                                                                                 ┌───────────┐
                                                                                 │ ANF IR    │──▶ eval_anf ──▶ 结果
                                                                                 └─────┬─────┘
                                           ┌───────────────────────────────────────────┴────┐
                                           │                                                │
                                           ▼ CPS 转换                                       ▼ LIR转换
                                     ┌───────────┐                                    ┌───────────┐
                                     │ CPS IR    │──▶ eval_cps ──▶ 结果               │ LIR       │
                                     └─────┬─────┘                                    └─────┬─────┘
                                           │                                                │
                                           ▼ un-CPS（受限）                                 ▼low emit-*
                                     ┌───────────┐                                    ┌───────────┐
                                     │ ANF IR    │                                    │ 机器码    │──▶ ELF / PE / Mach-O
                                     └───────────┘                                    └───────────┘
```

路径：

- `source → AST → ANF → CPS`（C 解释器：`eval_anf` / `eval_cps`）
- `CPS → ANF`（**un-CPS**，仅在「续延不逃逸」时成立）
- `source → AST → ANF → LIR → 机器码`（自举编译器 `yc`，不经过 CPS）
- 每层都可以独立 dump（`--dump-anf` / `--dump-cps`；LIR 为 yac list）



### 2.1 机器码路径：ANF → LIR → 二进制

```
                       ┌───────────┐
                       │ ANF IR    │
                       └─────┬─────┘
                             │
                             ▼ anf ir-转lir
                       ┌───────────┐
                       │ LIR       │── 槽机：mov / add / mref / fcall / syscall …
                       └─────┬─────┘
                             │
                             ▼ lower emit-x86_64 / emit_arm64 / emit_riscv64
                       ┌───────────┐
                       │ 机器码    │── 1..n 条目标指令字节
                       └─────┬─────┘
                             │
                             ▼ pack
                       ┌──────────────────┐
                       │ ELF / PE / Mach-O│
                       └──────────────────┘
```

三层都是 **yac list**（除最后的容器字节）。槽号 `s` 是整数虚寄存器；emit 映到 `[rbp+off]` / FP 帧。值在 LIR/机器码里是 tagged：int 为 `n<<1`，堆指针为奇数。

#### ANF（`anf.yac` 实际产出）

一个表达式 = 绑定序列 + 尾原子。顶层程序 = 这种 body 的列表。

```
body      ::= [ binds, atom ]

atom      ::= ["int",  digits]
            | ["str",  bytes]
            | ["bool", "true" | "false"]
            | ["unit"]
            | ["nil"]
            | ["var",  name]
            | ["fun",  [name*], body]

bind      ::= ["let",      name, atom]
            | ["letbin",   name, op, atom, atom]
            | ["letcall",  name, atom, [atom*]]
            | ["letif",    name, atom, body, body]
            | ["letfun",   name, [name*], body]

op        ::= "+" | "-" | "*" | "/" | "%"
            | "==" | "!=" | "<" | "<=" | ">" | ">="
            | "and" | "or"
```

`letfun` 的 body、`letif` 的两支都是完整 `body`（可再嵌套）。`print e` 在 parser 里降成 `call print(e, true)`。

#### LIR（跨架构，接近机器）

```
prog      ::= ["prog", [proc*], entryName]

proc      ::= ["proc", name, nparams, ncap, [insn*], srcname]
              ; 指令序列以 local 开头

insn      ::= ["local",    nslots, nparams]
            | ["nop"]
            | ["save"] | ["restore"]
            | ["mov",      s, s]
            | ["mov_imm",  s, int]
            | ["mref",     s, s, off]          ; qword，ptr 先去 tag
            | ["mset",     s, off, s]
            | ["mref8",    s, s, off]
            | ["mset8",    s, off, s]
            | ["add"|"sub"|"mul"|"div"|"rem"|"neg", ...]
            | ["sar"|"sal"|"shl"|"shr", s, s, s]
            | ["land"|"lor"|"xor", s, s, s]
            | ["lnot",     s, s]
            | ["tag"|"untag"|"is_int", s, s]
            | ["cmp"|"icmp", cop, s, s, s]     ; 写入 bool 槽
            | ["cmpjmp",   s, L, L]            ; 槽真→Lt 否则 Lf
            | ["jmp",      L]
            | ["label",    L]
            | ["fcall",    s, name, [s*]]      ; yac proc / yac_* runtime
            | ["ccall",    s, name, [s*]]      ; C ABI PLT（字面量符号名）
            | ["iccall",   s, s, [s*]]         ; C ABI 间接（dlsym 指针）
            | ["icall",    s, s, [s*]]         ; 闭包在槽里
            | ["apply",    s, s, ncap, [s*]]   ; emit：已知 ncap>0
            | ["tcall",    s, name, [s*]]
            | ["ticall",   s, s, [s*]]
            | ["tailapply",s, s, ncap, [s*]]
            | ["ret",      s]
            | ["syscall",  s, nr, [s*]]        ; nr 未 tag 立即数，≤6 参
            | ["print",    s]                  ; leftover; user print is runtime proc
            | ["glob",     s, i] | ["gst", i, s]
            | ["strlit",   s, bytes]           ; rodata，结果 tagged 指针
            | ["closure",  s, name, [s*]]      ; 分配闭包，patch 函数地址
            | ["alloc",    s, nbytes]          ; runtime kernel
            | ["write1"|"clock"|"memcpy", …]

cop       ::= "==" | "!=" | "<" | "<=" | ">" | ">="
off, nr, i, nslots, nparams, ncap, int ::= 整数
L, name, entryName, srcname            ::= 字符串
s                                      ::= 槽号（整数）
```

`mov_imm` 写入 **已经编码好的** 64 位模式（翻译时完成 tag：int 为 `n<<1`，nil 为 `1`）。源级 `nth`/`cons`/`len`/`str_cat`/`foldl`/`map`/`bytes_`*/*`argc`*/*`argv`*/*`time_`/`read_file`/`write_file` 不是 LIR 指令，一律 `fcall yac_`*（或 `time_ms` 等 runtime 名）。用户 `+`/`-`/`*`/`/` 走 `yac_num_*`（整数 insn 快路径，否则 `yac_num_slow`）。backend 把 `num.yac` 的 `let` ANF→LIR 后经 `runtime_add` 挂进客镜像。`ccall("name", …)` 降成 LIR `ccall`（C ABI / libc；x86_64 ELF）。`str_len`/`str_ref`/`bytes_len` 是对象字段读取（同 mref），emit 直出。

emit 认同一套 DESIGN 标签（`local`/`add`/`cmpjmp`/`fcall`/`ccall`/`tcall`/`icall`/`apply`/`mref`/`mset`/`mref8`…）。`apply`/`tailapply` 带已知 `ncap`；`icall`/`ticall` 无 ncap（运行时读闭包）。

`exit` 是 emit 糖（untag + 架构 exit），手写测试仍可用。`_start` 走 `untag` + `syscall 60`。`syscall` 的 nr **60 表示进程退出**（x86-64 Linux 的 exit 号）；arm64/riscv64 映成 93。槽参数已是要进寄存器的位模式。

#### ANF → LIR（`lir.yac` 的 `lir_expr`，对标 `anf_expr`）

环境 `Γ : name → slot`。当前过程名 `self`（`_start` 或某个 `letfun` 的码名）。已定义过程 `Σ`（名 → `{ncap}`）。`s*` 表示 fresh 槽。`I · J` 是指令拼接。`ε` 是空序列。

判断：

```
Γ ⊢ atom  ⇒  s  ▹  I
Γ ⊢ bind  ⇒  Γ' ▹  I  ▹  proc*
self ; Γ ⊢ body  ⇒  s  ▹  I  ▹  proc*
⊢ program  ⇒  prog
```

**原子**

```
Γ ⊢ ["int",  n]              ⇒  s  ▹  [["mov_imm", s, n<<1]]
Γ ⊢ ["bool", "true"]         ⇒  s  ▹  [["mov_imm", s, 2]]
Γ ⊢ ["bool", "false"]        ⇒  s  ▹  [["mov_imm", s, 0]]
Γ ⊢ ["unit"]                 ⇒  s  ▹  [["mov_imm", s, 0]]
Γ ⊢ ["nil"]                  ⇒  s  ▹  [["mov_imm", s, 1]]
Γ ⊢ ["str",  b]              ⇒  s  ▹  [["strlit",  s, b]]
Γ ⊢ ["var",  x]              ⇒  Γ(x)  ▹  ε
Γ ⊢ ["fun",  ps, body]       ≡  Γ ⊢ ["letfun", x, ps, body] ; ["var", x]   x fresh
```

**绑定**

```
Γ ⊢ ["let", x, a]  ⇒  Γ[x ↦ s]  ▹  I  ▹  ∅
  where  Γ ⊢ a  ⇒  s  ▹  I

Γ ⊢ ["letbin", x, op, a, b]  ⇒  Γ[x ↦ s]  ▹  Iₐ · Iᵦ · [ι]  ▹  ∅
  where  Γ ⊢ a  ⇒  sₐ  ▹  Iₐ
         Γ ⊢ b  ⇒  sᵦ  ▹  Iᵦ
         ι = bin(op, s, sₐ, sᵦ)

bin("+",s,a,b)  = ["add", s, a, b]
bin("-",s,a,b)  = ["sub", s, a, b]
bin("*",s,a,b)  = ["mul", s, a, b]
bin("/",s,a,b)  = ["div", s, a, b]
bin("%",s,a,b)  = ["rem", s, a, b]
bin("and",s,a,b)= ["land", s, a, b]          ; 不短路
bin("or",s,a,b) = ["lor",  s, a, b]          ; 不短路
bin(cop,s,a,b)  = ["cmp",  cop, s, a, b]     ; cop ∈ {==,!=,<,<=,>,>=}

Γ ⊢ ["letcall", x, print, [v, nl]] 走普通 fcall（runtime `print`）。

Γ ⊢ ["letif", x, c, bodyₜ, bodyₑ]  ⇒  Γ[x ↦ s]  ▹  I  ▹  Pₜ ∪ Pₑ
  where  Γ ⊢ c  ⇒  s_c  ▹  I_c
         self ; Γ ⊢ bodyₜ  ⇒  sₜ  ▹  Iₜ  ▹  Pₜ
         self ; Γ ⊢ bodyₑ  ⇒  sₑ  ▹  Iₑ  ▹  Pₑ
         I = I_c ·
             [["cmpjmp", s_c, Lₜ, Lₑ],
              ["label", Lₜ]] · Iₜ · [["mov", s, sₜ], ["jmp", L],
              ["label", Lₑ]] · Iₑ · [["mov", s, sₑ],
              ["label", L]]

Γ ⊢ ["letcall", x, f, as]  ⇒  Γ[x ↦ s]  ▹  I_f · I_as · [ι]  ▹  ∅
  where  Γ ⊢ f          ⇒  s_f  ▹  I_f
         Γ ⊢ as_i       ⇒  s_i  ▹  I_i     （逐参）
         I_as = I_0 · … · I_{n-1}
         ι   = callι(self, x, f, s, s_f, [s_i])

callι(self, x, ["var", g], s, _, ss) =
    ["tcall",  s, ĝ, cap·ss] if  tail(x) ∧ ĝ = self ∧ |cap·ss| ≤ 6
  | ["fcall",  s, ĝ, cap·ss] if  ĝ = self ∧ n > 0 ∧ |cap·ss| ≤ 6
  | ["fcall",  s, ĝ, ss]     if  g ∈ Σ ∧ n = 0
  | ["fcall",  s, rt(g), ss] if  g 是 runtime 名   ; 在 Σ / env 之后，避免遮蔽 let len
  | ["ccall",  s, name, ss]  if  g = ccall 且首参是字符串字面量
  | ["iccall", s, s_f, ss]   if  g = ccall 且首参不是字符串字面量
  | ["apply",  s, Γ(g), n, ss] if  Γ(g) 有已知 ncap = n > 0
  | ["icall",  s, Γ(g), ss]  otherwise        ; 槽里是闭包，nenv 运行时读
  where ĝ = Σ 中 g 的码名（重名加 #uid）
        n   = Σ(g).ncap
        cap = [1..n]                         ; 当前帧捕获槽，self 调用要原样传入

callι(self, x, f, s, s_f, ss) =
    ["ticall", s, s_f, ss]   if  tail(x) ∧ f 是 self 的闭包槽 ∧ |ss| ≤ 6
  | ["icall",  s, s_f, ss]   otherwise

rt("cons")="yac_cons"  rt("nth")="yac_nth"  rt("len")="yac_len"
rt("foldl")="yac_foldl"  rt("map")="yac_map"  rt("argc")="yac_argc"  …
runtime 名以 yac_* / time_* / gc_collect / argc / argv / print_val 为准。

tail(x)  当且仅当该 letcall 是 body 的最后一条绑定，且尾原子是 ["var", x]。
只对 self 做 TCO；`ccall`（C）不做 TCO。
命名 self 走第一条（`fcall`/`tcall` + 捕获槽），不要把所有尾 `icall` 收成 `ticall`（`twice(f,x)=f(f(x))` 会错）。

Γ ⊢ ["letfun", f, ps, body]  ⇒  Γ[f ↦ s]  ▹  I_out  ▹  {proc} ∪ P
  where  caps = FV(body) \ ({f} ∪ ps)
         ncap = |caps|
         Γ_f  = { caps_i ↦ i+1 } ∪ { ps_j ↦ ncap+j+1 }
                ∪ (ncap>0 ∧ f ∈ FV(body)  ?  {f ↦ ncap+|ps|+1}  :  ∅)
         f ; Γ_f ⊢ body  ⇒  s_r  ▹  I_b  ▹  P
         proc = ["proc", f, ncap+|ps|, ncap,
                 [["local", N, ncap+|ps|]]
                 · (ncap>0 ∧ f ∈ FV(body)
                      ? [["closure", Γ_f(f), f, [1..ncap]]]
                      : ε)
                 · I_b
                 · [["ret", s_r]],
                 f]
         I_out = [["closure", s, f, [Γ(caps_i)]]]
                 ; ncap=0 也分配闭包：函数当值（map/filter）时槽里必须是闭包。
                 ; 按名调用走 fcall，不读这个槽。不做「只按名」分析。
```

槽布局：`1..ncap` 捕获，`ncap+1..` 形参，之后是局部。`N` 是本过程用到的最大槽号。

**body / 程序**

```
self ; Γ ⊢ [ b1, …, bn ], a  ⇒  s  ▹  I₁ · … · Iₙ · I_a  ▹  P₁ ∪ … ∪ Pₙ
  where  Γ ⊢ b1  ⇒  Γ₁ ▹ I₁ ▹ P₁
         Γ₁ ⊢ b2 ⇒  Γ₂ ▹ I₂ ▹ P₂
         …
         Γₙ ⊢ a  ⇒  s  ▹ I_a

⊢ [body₁, …, bodyₘ]  ⇒  ["prog", [_start] · runtime · procs, "_start"]
  where  _start ; ∅ ⊢ body₁;…;bodyₘ  ⇒  s  ▹  I  ▹  procs
         _start = ["proc", "_start", 0, 0,
                   [["local", N, 0]] · I ·
                   [["untag", t, s], ["syscall", _, 60, [t]]],
                   "_start"]
```

`runtime` 是 `yac_*` 等过程，不从 ANF 来。调用约定最多 6 个参数。

**例子**

```
源:   let x = 1 in x + 2

ANF:  [[["let", "x", ["int", "1"]],
        ["letbin", "t", "+", ["var", "x"], ["int", "2"]]],
       ["var", "t"]]

LIR:  ["proc", "_start", 0, 0,
       [["local", 4, 0],
        ["mov_imm", 1, 2],          ; 1<<1
        ["mov_imm", 2, 4],          ; 2<<1
        ["add", 3, 1, 2],
        ["untag", 4, 3],
        ["syscall", 0, 60, [4]]],
       "_start"]
```



#### 机器码（不是第三种 IR）

LIR 一条 insn 变成 **1..n 条目标指令字节**，再打进容器。没有「机器码语法」的 yac list；形态是文件：

```
image     ::= ELF64 | PE | Mach-O
ELF64     ::= ehdr  phdr*  text  (globals…)

text      ::= encoded(insn)*     ; 按 --arch 选 emit-*
encoded   ::= x86-64 | arm64 | riscv64 字节
```

约定（各 arch 各自实现，LIR 不变）：

- 槽 `s` → 帧上 8 字节格；临时值走返回寄存器（x86 `rax`，arm `x0`，riscv `a0`）
- `mov_imm`：按立即数原样写入，不再 `<<1`
- `fcall`：按名 rel32/`bl`/`jal` 到本镜像符号表（yac proc / yac_*）；参数 ≤6
- `ccall`：C ABI PLT（字面量名）；`iccall`：C ABI 间接调用。x86_64/arm64/riscv64 ELF 经 PLT + `DT_NEEDED libc.so.6`。`cload`/`csym` 是 `rt/ffi.yac` 普通函数（`dlopen`/`dlsym`）。JIT 在 `jit_run` 前 `dlsym` 填 GOT。整数去 tag、堆对象传 payload 指针。参数走各 arch 整数 ABI：x86_64 前 6 个寄存器其余压栈；arm64/riscv64 前 8 个寄存器其余压栈；调用前 SP 16 字节对齐。`-g`/`--syms` 时 pack 写 `.symtab`/`.strtab`（gdb/`nm`）；默认不写。无 DWARF 行号
- `syscall`：x86 `syscall`，arm `svc #0`，riscv `ecall`；`nr` 进 syscall 号寄存器（60 = 退出，见上）
- `cmpjmp`：测 tagged 条件槽（非 0 为真；`true` 的 tag 为 2）
- `_start`：`local` 后跑顶层绑定，最后 `untag` + `syscall 60`



## 3. 源语言

调用约定为 **call-by-value**，n 元函数，无高阶类型变量（核心阶段不设类型系统，见 §9）。

### 3.1 语法

```
program   ::= top*
top       ::= package | import | export
            |  let name = expr          -- 全局绑定（可为递归）
            |  expr                    -- 表达式，最后一个表达式的值即程序结果

expr      ::= integer | float | true | false | string | ()
            |  name
            |  e e ...                 -- n 元函数应用
            |  fun (name*) -> e        -- n 元 lambda
            |  if e then e else e
            |  let name = e in e'
            |  e binop e               -- 中缀原语：+ - * / % == != < <= > >= and or
            |  not e
            |  print e
            |  callcc e                -- 捕获当前续延（CPS 特性）
            |  throw e e'              -- throw k v：向续延 k 投值 v（CPS 特性）

binop     ::= + | - | * | / | % | == | != | < | <= | > | >= | and | or
```

注释：`--` 单行，`/* ... */` 块注释。

### 3.2 语义要点

- 整数为 64 位（`int64_t`），浮点为 `double`，布尔为真/假，字符串为字节串。
- `print` 打印并返回原值（保持表达式性质）。默认换行；`print(e, false)` 不换行。
- `callcc f`：`f` 是一元函数，收到一个**当前续延**（一个一等值）；调用 `throw k v` 即以 `v` 作为整个 `callcc` 表达式的结果跳回。
- 顶层最后一个表达式的结果就是程序退出值（ANF 的 `halt`、CPS 的 `halt`）。

### 3.3 包（语言层：`package` / `import` / `export`）

包是**命名空间与信息隐藏**，不是链接或版本边界。物理切分（CRP/CCP、是否进镜像）见 `docs/SELFHOST.md` 的「编译单元」；语言里没有 `unit` 关键字。不要把 `import` 和 PE/ELF 的 `cimport` 混为一谈。

规则：

- 一个包对应一个 `.yac` 文件（`.` → `/`）：`import rt.os` → `rt/os.yac`。目录只是包名前缀，不是「一目录多文件同一包」。包内可前向引用同包绑定。
- 未 `export` 的名字只在包内可见。LIR 符号日后为 `rt.os/os_has` 这种键；当前阶段客程序仍用裸名调用已链接的 stdlib 过程，隐藏只在绑定检查里执行。
- `import` 只引入该包的导出集。没有默认 `import *`。
- 查找根（每个根下再拼 `rt/os.yac` 这类相对路径）：`--pkg DIR[,DIR...]`（从左到右）、当前目录 `pkg/`、再是 `yc` / `yac` 可执行文件所在目录。空段（`a,,b`）为错误；`--pkg` 只能出现一次。本仓库开发用 `--pkg src-self`（提供 `rt.*`）；`./pkg` 提供常见库。独立项目把 `rt/` 放在 `yc` 旁边，或 `--pkg` 指向含 `rt/` 的根。
- 原语名（`cons`、`ccall`、`str_cat` 等）走 `is_prim_name`，不是包。客库不得再导出这些名字。
- 没有 `package` 的文件属于匿名主包（与改前行为相同）。

三层库（不要混）：

| 层 | 名字 | 位置 | 用法 |
|---|---|---|---|
| 内核 | 无包名 | `src-self/rt/runtime.yac` 进镜像 | `print` / `cons` / `read_file` 等原语 |
| 语言运行时 | `rt.*` | `src-self/rt/{num,os,ffi}.yac` | `import rt.os`；客默认还链 `rt.num` |
| 常见库 | 短名，不用 `rt` 前缀 | 仓库根 `pkg/*.yac` | `import path`；项目自己的库也放 `./pkg` |

`src-self/lib`（`log` / `map` / `pass`）只给编译器用，不是客库。不要用包名 `os`（与 `rt.os` 冲突）。

`rt.*` 维持现状：`rt.num`（慢路径算术，默认链）、`rt.os`（`uname` / `host_*`）、`rt.ffi`（`cload` / `csym`）。不要再拆 `rt.str` / `rt.list`（已是原语）。

常见库按需增加，不一次写完：

- 已落地：`path`、`str`、`io`、`list`、`hash`、`fmt`、`log`、`test`、`net`、`bytes`、`ffi`、`json`（tagged：`["N",n]` / `["S",s]` / `["A",xs]` / `["O",pairs]` / `["T"]` `["F"]` `["Z"]`；`parse` / `stringify` / `get`。整数，无 frac/exp。`str` 另有 `join`/`trim`/`find`/`contains`；`io` 另有 `write`/`read_or`）。
- 有真实调用再做：`yui`（从 `app/native/yui.yac` 收）、`json`、`http`（建在 `net` 上）。
- 先不要：`re` / `crypto` / `thread`。

包查找器（`backend.yac` 的 `pkg_src`）本身不能 `import path` / `import io`，否则加载 `path.yac` 会循环。

语法（`program` 的顶层还可出现下列形式；`as` 不是关键字）：

```
package   ::= package ident ("." ident)*
import    ::= import ident ("." ident)*
            | import ident ("." ident)* "{" ident ("," ident)* "}"
            | import ident ("." ident)* as ident
export    ::= export ident ("," ident)*
```

`import P as a` 现阶段与 `import P` 相同（尚无 `a.x` 限定名）。`import P { x, y }` 只引入列出且确为导出的名字。

客程序绑定检查：内核名（`runtime_funs`）加 `import` 的导出。未 import 则 `host_os` / `cload` 为未绑定。链接同样按 import：默认 `kernel`+`rt.num`，`import rt.os` / `rt.ffi` 才链对应文件。`os_has` 留在包 `rt.os` 且不导出。


### 3.4 示例

```
-- 阶乘：普通程序，ANF/CPS 都能跑
let fact(n) =
  if n <= 1 then 1 else n * fact(n - 1)
in
fact(10)                          -- 3628800
```

```
-- 使用 callcc：只能跑 CPS 机器
let k = callcc(fun (k) -> k) in   -- k 绑定到"当前续延"
throw k 42                        -- 直接跳到程序出口，输出 42
```

```
-- 用 callcc 提前退出（跳出多层递归）
let exit = callcc(fun (k) -> k) in
let f(n) = if n > 100 then throw exit 999 else f(n+1) in
f(0)                              -- 999
```



## 4. 核心 IR 之一：ANF

ANF 的核心理念：**"计算"与"绑定"分离**。原子值（Atom）无副作用、无需再求值；一切计算都绑定到变量后再继续。

### 4.1 语法

```
Atom  A ::= x | lit | prim                    -- 原子：变量、字面量、原语名
Exp   E ::= halt A
        |  let x = call(A, A*) in E           -- 绑定一次 n 元调用
        |  let x = prim(p, A*) in E           -- 绑定一次原语运算
        |  let x = A in E                     -- 别名绑定
        |  let f = λ(x*).E in E               -- 非递归函数
        |  letrec f = λ(x*).E in E            -- 递归函数
        |  if A then E else E
        |  call(A, A*)                        -- 尾调用
        |  prim(p, A*)                        -- 尾原语
        |  A                                  -- 返回原子
```



### 4.2 说明

- **原子（Atom）不会触发求值**：变量、字面量、原语名求值结果立即可得。
- 每个 `let x = … in E` 只绑定**一次**计算，因此 `E` 中的 `x` 一定是一个"已算好的值"——求值顺序在语法里被写死。
- 尾位置的 `call / prim / A` 不绑定结果，直接把控制权交给调用者/顶层，天然支持尾调用优化。
- 函数体本身就是一个 ANF 表达式；`λ(x*).E` 是值的一部分（闭包）。



### 4.3 ANF 解释器（eval_anf）

一个直接的树遍历器：`let x = v in E` 先算 `v`，扩展环境后继续算 `E`；尾调用直接替换状态、进入循环，**C 调用栈不增长**。

```
eval(E, env):
  case E:
    halt A            → atom(A, env)
    let x = call(f,a*) in E'  → let vf = atom(f); va = atom(a*) in
                                E'[env ⊢ x = apply(vf, va)]
    let x = prim(p,a*) in E'  → E'[env ⊢ x = do_prim(p, atom(a*))]
    let x = A in E'   → E'[env ⊢ x = atom(A)]
    let f = λ… in E'  → E'[env ⊢ f = closure]
    letrec f = λ… in E' → E'[env ⊢ f = closure(self-rec)]
    if A then E1 else E2 → if atom(A) then eval(E1) else eval(E2)
    call(f, a*)       → tail jump: apply(atom(f), atom(a*))
    prim(p, a*)       → tail jump: do_prim(p, atom(a*))
    A                 → atom(A, env)
```

**ANF 机器不能跑的构造**：`callcc` / `throw`。源程序中若出现它们，ANF 归一化阶段会为它们生成 `callcc(A)` / `throw(A,A')` 节点；ANF 解释器遇到时**直接报错**："callcc/throw 只能在 CPS 模式下运行"。这就是两套机器语义差异的落点。

## 5. 核心 IR 之二：CPS

CPS 的核心理念：**求值顺序即续延，续延即值**。每个函数多收一个续延参数 `k`；函数从不"返回"，只调用 `k` 传递结果。

### 5.1 语法

```
CVal V ::= x | lit | prim | λ(x*, k).C            -- 值；k 是续延参数
CExp C ::= let x = V in C
        |  V V*                                    -- 尾调用：f a₁…aₙ k，或 k v
        |  if V then C else C
        |  halt V
```

- 应用 `V V*`：最后一个实参是**续延**。若头部是函数，则调用它并把其余参数和续延传进去；若头部是续延值，则是一次**跳转**（等价于 `throw k v`）。
- 一个**程序** = 一个 CPS 表达式 + 一个初始续延 `halt`（打印/返回结果）。
- `let x = V in C` 中的 `V` 是原子值，不求值、不调用——所有"副作用性"计算都发生在应用位置。



### 5.2 关键观察：callcc 在 CPS 里是免费的

因为续延就是普通值：

```
⟦callcc f⟧   ≡   f k        -- k 就是本调用点的续延参数
⟦throw k v⟧  ≡   k v        -- 对续延值做一次尾调用
```

实现时，`callcc` 和 `throw` 可以就是两个原语：

- `callcc(f)`：把机器当前的续延（一个闭包值）作为参数调用 `f`。
- `throw(k, v)`：直接尾调用 `k(v)`。



### 5.3 CPS 解释器（eval_cps）

机器状态只有 `(code, env)`，配合**显式帧栈**或**直接续延值**，所有控制流都是循环：

```
eval(C, env):
  case C:
    halt V        → atom(V, env)
    let x = V in C' → C'[env ⊢ x = atom(V, env)]
    V V*          → tail jump: 求值头部与实参得值，替换 (code, env) 继续循环
    if V then C1 else C2 → if atom(V, env) then eval(C1) else eval(C2)
```

由于 CPS 中续延已经是显式参数，最简单且正确的实现是**纯蹦床（trampoline）**：

```
run(prog):
  code = prog; env = empty
  loop:
    switch code:
      LET:   env = bind(x, evalVal(V, env)); code = body;      continue
      IF:    code = evalVal(cond, env) ? then : else;          continue
      HALT:  return evalVal(V, env)
      CALL:  f = evalVal(head, env); args = evalVals(rest, env)
             if f 是原语 → 在循环内执行（见 §6.4 原语回调）
             if f 是闭包 → env = bind(闭包参数, args); code = 闭包体;  continue
```

`evalVal` 只求值原子（变量查表、字面量、闭包、原语名），是浅层操作，不会造成 C 栈递归。**所有调用都是循环迭代，C 栈永不增长。**

`callcc` 的实现：当 CALL 的头部是 `callcc` 原语时，把**当前机器续延**（即"算完这个 callcc 之后剩下的计算"）物化为一个闭包。由于 CPS 下续延就编码在调用点的参数里，这里只需要把调用点传入的续延参数重新打包为值即可——不需要额外的运行时栈。

（当我们在 §7.3 讨论"续延栈"表示时，callcc 则捕获帧栈的一个后缀。）

## 6. 求值的基础设施（C 实现）



### 6.1 值表示

```c
typedef enum { V_INT, V_FLOAT, V_BOOL, V_STR, V_FUN, V_PRIM } ValTag;

typedef struct Value {
    ValTag tag;
    union {
        int64_t  i;            /* V_INT */
        double   f;            /* V_FLOAT */
        bool     b;            /* V_BOOL */
        Str     *s;            /* V_STR  */
        Closure *clo;          /* V_FUN  */
        Prim     *prim;        /* V_PRIM */
    } u;
} Value;

typedef struct Closure {
    Value    fun;              /* 被捕获环境中的函数值（自引用，供递归） */
    CExp    *body;             /* CPS 下：λ(x*,k).C；ANF 下：λ(x*).E */
    int      nparams;
    char   **params;
    Env     *env;              /* 词法环境（被捕获的绑定） */
} Closure;
```

- 闭包在 ANF 与 CPS 之间复用同一结构；差别只在 `body` 指向的 IR 与参数约定（CPS 多一个续延参数）。
- `Env` 采用**链表作用域**或**扁平数组+快照**（见 6.3）。



### 6.2 IR 表示

CPS IR：

```c
typedef enum { C_LET, C_CALL, C_IF, C_HALT } CExpKind;

typedef struct CExp {
    CExpKind kind;
    union {
        struct { Value x, v;      CExp *body; } let;
        struct { Value head; int nargs; Value *args; } call;   /* args[nargs-1] 是续延 */
        struct { Value cond;      CExp *then, *els; } if_;     /* then/els 的续延相同 */
        struct { Value v; } halt;
    } u;
} CExp;
```

ANF IR 与之同构，差别是：

- `call`/`prim` 出现在 `let` 的右侧而不是整体节点；
- 尾调用 `call/prim` 是独立节点；
- 多出 `callcc`/`throw` 节点（ANF 机器拒绝执行，CPS 机器接受）。



### 6.3 环境（Env）

C 中的环境是一个从变量名到 `Value` 的映射。设计选择：

- **链式环境**：`typedef struct Env { char *name; Value val; struct Env *prev; } Env;` 简单，但线性查找慢。
- **扁平环境 + 闭包快照**：编译器先把自由变量编号，闭包携带一个 `Value[]` 快照，解释器用下标访问。更贴近真实编译器的做法。

设计建议：**先用链式环境（M1 快速跑通），M4 再换扁平快照**。两种方案在《目录与里程碑》中标注。

### 6.4 原语

原语签名按所在 IR 区分：

- **ANF**：`Value prim_ANF(Value *args, int nargs, PrimCtx *ctx)` —— 返回结果值。
- **CPS**：`void prim_CPS(Value *args, int nargs, Machine *m)` —— 不返回，直接改写 `m->code/m->env` 继续循环（回调式原语）。`callcc`、`throw`、`print` 都是这种签名。

```c
typedef struct Prim { const char *name; int arity; PrimFn fn; } Prim;
```

算术/比较原语（`+ - * / % == …`）两套签名共用同一组计算内核，只在外层适配返回/回调。

## 7. ANF ↔ CPS 转换



### 7.1 ANF → CPS（CPS 转换）

定义 `⟦·⟧` 把 ANF 表达式映到 CPS 表达式，同时引入续延参数 `k`：

```
⟦halt A⟧            = halt A
⟦let x = call(f,a*) in E⟧ = call f a* (λx. ⟦E⟧)          -- 续延 = 绑定 x 后继续
⟦let x = prim(p,a*) in E⟧ = call p a* (λx. ⟦E⟧)
⟦let x = A in E⟧    = call (λx. ⟦E⟧) A
⟦let f = λ(x*).E in E'⟧ = call (λf. ⟦E'⟧) (λ(x*,k). ⟦E⟧)
⟦letrec f = λ(x*).E in E'⟧ = call (λf. ⟦E'⟧) (λ(x*,k). ⟦E⟧[f := 自身])
⟦if A then E1 else E2⟧ = if A then ⟦E1⟧ else ⟦E2⟧
⟦call(f, a*)⟧      = call f a* k                          -- 尾调用：续延原样传递
⟦prim(p, a*)⟧      = call p a* k
⟦A⟧                = call k A                             -- 把结果投给续延
```

要点：

- 尾调用位置的续延就是**外层传入的 k**，因此 CPS 天然保留 TCO。
- 函数体的转换以"函数的续延参数 k"为环境入口，被调用时收到真实续延。
- `callcc`/`throw` 在转换中保持不变（CPS 机器原生支持）。



### 7.2 CPS → ANF（un-CPS）

**前提**：CPS 程序中的续延从不逃逸、只在尾位置被调用（"续延封闭"）。在此条件下可反变换，把续延吸收回语法结构。算法思路：

```
unCPS(C):  // 假设续延都形如 λx. E 且只在尾位置被调用
  把 C 中每个调用点 f a₁…aₙ (λx.E) 还原为 let x = call(f,a₁…aₙ) in unCPS(E)
  续延参数 k 在函数内部被调用的位置 ⟦k v⟧ 还原为"返回 v"（对应 ANF 的 halt/返回）
```

实现上用一个**续延逆环境** `k ↦ 期望的表达式模板` 做抽象求值。凡遇到 `callcc` 或续延被多次/以值方式保存的程序，un-CPS 直接失败并报"该程序不可去 CPS 化"。

### 7.3 续延的两种运行时形态（影响解释器设计）


| 形态       | 实现                          | callcc 支持         |
| -------- | --------------------------- | ----------------- |
| A. 续延即值  | 蹦床，续延是普通闭包                  | 免费（续延已经是值）        |
| B. 显式续延栈 | 机器维护 `Frame *cont` 帧栈，续延是帧链 | `callcc` = 捕获帧链后缀 |


设计上 **先实现 A**（简单、正确）；B 作为后续优化/教学展示（更接近"控制栈"的直觉，也便于接 `setjmp/longjmp` 风格的异常原语）。两者结果应一致，可交叉测试。

## 8. 内存管理



### 8.1 为何不用引用计数

CPS 中闭包可捕获续延，续延再捕获闭包——**环**不可避免，引用计数会泄漏。因此采用**追踪式 GC**。

### 8.2 标记-清除（mark-sweep）

- 堆对象：`Closure`、`CExp`（若动态构造）、`Env`、`Str`。
- **根集合**：机器状态——`m->code` 引用的闭包、`m->env`、正在求值的实参数组、以及原语回调里的临时值。
- 根集合用显式**值栈**（`Value *vstack`）维护：进入原语回调前 `gc_push` 保护临时值，返回后 `gc_pop`。C 局部变量不当作根（不做保守扫描），保证可移植与精确性。
- 回收：`mark(roots) → sweep(堆)`，空闲块用 free 链维护，`alloc` 优先复用。



### 8.3 简化方案（M1–M2 先用）

- **arena/无 GC 版**：解释器运行期只增不减地分配（跑完统一释放），配合 `--limit-nodes` 限制节点数以防失控。正确性测试用它跑小输入。
- M3 引入 mark-sweep 后，arena 版保留为 `--no-gc` 调试开关。



## 9. 类型系统（可选扩展，非核心）

核心设计不设类型系统（untyped）。扩展方向：

- **表层 + ANF**：Hindley-Milner 类型推断（`let` 泛化）。
- **CPS**：续延类型 `τ ⇒ ⊥`，CPS 版本的类型为 `(A→⊥)→⊥`，采用回答类型多态（answer type polymorphism）。`callcc : ((τ⇒α)→τ)→τ`。
- 该扩展不影响 §4–§8 的任何求值语义，独立成模块。



## 10. 目录结构与模块划分

```
yac/
  DESIGN.md
  README.md
  Makefile
  src/
    main.c            -- 驱动：CLI 选项、读取源文件、装配管线
    lexer.c/h         -- 词法
    parser.c/h        -- 语法 → AST
    ast.c/h           -- AST 节点与 dump
    anf.c/h           -- AST → ANF 归一化
    cps.c/h           -- ANF → CPS
    uncps.c/h         -- CPS → ANF（受限，可失败）
    value.c/h         -- Value/Closure/Prim、原语实现
    env.c/h           -- 环境
    eval_anf.c/h      -- ANF 解释器（含 callcc 拒绝逻辑）
    eval_cps.c/h      -- CPS 解释器（蹦床 + callcc/throw）
    gc.c/h            -- mark-sweep GC
    print.c/h         -- 值/IR dump 输出
  tests/
    run_tests.sh      -- 回归脚本
    *.yac             -- 测试用例
    props/            -- 属性测试（随机程序，ANF 与 CPS 结果比对）
```

CLI：

```
yac file.yac                    # 默认：走 ANF 解释器
yac --cps file.yac              # 走 CPS 解释器
yac --dump-anf file.yac         # 只打印 ANF，不运行
yac --dump-cps file.yac         # 打印 ANF→CPS 结果
yac --both file.yac             # 两个解释器各跑一遍并比对（属性测试入口）
yac --no-gc file.yac            # 关掉 GC（arena 调试模式）
```



## 11. 验证策略

1. **golden tests**：同一 `.yac` 程序分别跑 ANF 与 CPS，输出必须一致。
2. **属性测试**：随机生成 AST → 转 ANF → 转 CPS，比对 `evalANF` 与 `evalCPS` 结果；随机程序里混入 `callcc`/`throw` 时，只对 CPS 断言（ANF 应报"不支持"）。
3. **TCO 压测**：`let f(n) = if n==0 then 0 else f(n-1) in f(10000000)` 必须不爆栈（ANF 与 CPS 各一遍）。
4. **callcc 语义测试**：`callcc`+`throw` 的经典用例（提前退出、生成器式背靠背续延、K 组合子小剧场）。



## 12. 里程碑


| 里程碑 | 内容                                      | 验收                           |
| --- | --------------------------------------- | ---------------------------- |
| M1  | lexer、parser、AST→ANF、ANF 解释器（arena 分配）  | 普通程序可跑；golden 测试过            |
| M2  | ANF→CPS、CPS 解释器（蹦床）、`callcc`/`throw` 原语 | `--both` 对普通程序一致；callcc 用例跑通 |
| M3  | mark-sweep GC、un-CPS（受限）、`--dump-*`     | TCO 压测 10⁷ 级不爆栈；un-CPS 往返一致  |
| M4  | 扁平环境快照、CPS 化简（常量折叠/eta 归约）、属性测试、文档      | 性能可测量改进；随机程序比对稳定             |




## 13. 参考文献与灵感

- Andrew W. Appel, *Compiling with Continuations*（CPS 作为编译 IR 的经典）
- A. Sabry, M. Felleisen, *Reasoning about Programs in Continuation-Passing Style*（CPS 等价性、un-CPS 条件）
- C. Flanagan et al., *The Essence of Compiling with Continuations*（ANF 与 CPS 的关系）
- S. L. Peyton Jones, *Compiling Haskell by Program Transformation*（归约/化简示例）

