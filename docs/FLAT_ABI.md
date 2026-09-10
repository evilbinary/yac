# FLAT_ABI.md — 顶层函数 Flat 调用约定（12.14）

## 状态

已批准的设计方案。取代 12.13 为实现 `import compiler` + `compile(...)`
在 REPL 中工作而层层堆叠的 caps 链 + host 桥补丁。

## 问题

现有 ABI 下，在单次编译会话之外对其它包的函数做 first-class 引用是不可能的：

- 顶层 `letfun` 编译出的代码，其自由变量（同包的兄弟函数）是**从 `rdi`
  取的**：调用者必须把被调者的*包环境*作为前导参数传入。函数体 literally
  执行 `mov -0x10(%rbp),%rax; mov 0x90(%rax),...` 来装载 `compile_env_new`，
  然后才调用它。
- 在一次 AOT 编译内这是自洽的：每个函数的 caps 沿调用链传递（fvs 槽位），
  闭包由 `_start` 期的 `outer_caps` 构造。
- REPL 的 blob 是**第二次独立编译**：它能 patch 到被调者的*代码入口*
  （funsym），但无法伪造被调者的环境——那 15 个闭包指针只存在于宿主
  `_start` 运行时在堆上构造的闭包对象里。把第一个真参放进 rdi，函数体
  就会把字符串长度当函数指针解包 → SIGSEGV。

12.13 堆叠的所有东西（host 槽、`yac_host_call` 桥、按名字的闭包表）
都是为这一个事实打的补丁。

## 新 ABI

顶层函数**没有捕获环境**。对同包顶层函数的自由引用经过每镜像一份的
**GOT**（数据区里的代码入口地址扁平数组），调用就是普通的
`call [GOT 项]` / `call rel32`。结论：

- **rdi 永远是第一个真参。** 不再有前导 caps 约定。
- **函数值就是代码入口地址**（tagged）。顶层函数不再有闭包对象；
  `fcall`、`var`、`icall` 合并为同一件事。
- 嵌套的 `fun` 字面量（对局部变量的真闭包）保留现有闭包对象表示——
  只有*顶层*变 flat。（Phase 2 见下；Phase 1 只覆盖顶层。）

### GOT 布局

```
G + 448                       ; 紧跟 cimport 表之后
  + 0    : 包内第 0 个 let 的入口
  + 8    : 包内第 1 个 let 的入口
  ...
```

每个顶层 letfun 一个 GOT 槽，下标 = **发射顺序**（镜像内稳定；REPL 的
blob 把新条目追加在宿主条目之后）。AOT 镜像的表在 emit 期 bake（入口
地址是编译期常量），blob 在追加时 patch（入口 = dest + blob 内偏移）。

## 时序图

### 1. AOT 自举编译（形状不变，调用更简单）

```
make / yc1
  │
  ├─ lir: letfun_finish 不再发 caps 机制；
  │        对兄弟函数的自由引用发射 ["gcall", dst, got_idx]
  │        （目标在同一编译单元且可达时可用普通 rel32 fcall）
  │
  ├─ emit: _start 通过 call [GOT+8k] 调各 let；GOT 直接 bake
  │        LOAD_VADDR+TEXT_OFF+fn_offset —— 无 xset、无闭包
  │
  └─ 运行:  main → parse_args → ... → call [GOT_compile_native]
           → 直达，零环境流量
```

### 2. REPL 会话（以前会崩的场景）

```
yc.exe --repl --pkg ...
  │
  │   宿主镜像：GOT 已 bake；compile_native 入口 @ H
  │
  ├─ "import compiler"
  │    blob1 = compile(compiler.yac 源码，追加在 dest 处)
  │      ├─ letfun compile：发射到 blob1 偏移 o1 处
  │      ├─ 其函数体：call compile_native →
  │      │    call [GOT+idx(compile_native)]   ; GOT 槽 patch → H
  │      └─ blob1 自己的 let 的 GOT 槽：入口 = dest + o1
  │         （经 live 会话追加 patch，复用 jsess fmap）
  │
  ├─ "compile"  （first-class var）
  │    blob2: var → load [GOT+idx(compile)] → tagged 入口地址
  │      → 函数值就是入口，传递/打印都成立
  │
  ├─ "compile(\"1+2\")"
  │    blob2: call [GOT+idx(compile)]
  │      → rdi = "1+2"（第一个真参，无 caps！）
  │      → wrapper 体内：call [GOT+idx(compile_native)] → 宿主代码
  │      → 编译产物 bytes 返回 REPL
  │
  └─ "1+1" 等：不变（调 rt $proc，本来就是直达）
```

### 3. 跨 blob 引用（live 会话）

与宿主的 GOT 相同：blob 自己的 let 在 **blob 的数据区**里有 GOT 条目；
对更早 blob 的函数（或宿主函数）的引用在 `emit_apply_unres(dest)` 时
按 jsess fmap patch——即现有的 live 会话 patch 通道，现在用于所有
跨单元引用，而不只是应急。

## 删除清单

全绿后（link 13 PASS / repl 26/26 / import+compile e2e）删除：

| 删除对象 | 原用途 |
|---|---|
| `bind_caps` / `outer_caps` / `cap_slots` | 前导 caps ABI 机制 |
| `host_env_extra`（backend） | blob 侧 G+136 槽填充 |
| `rt_host_call_ins`（`yac_host_call` 桥） | 调用点的 caps 解包 |
| `rt_gtab_get` / `xset` 表 | 名字→闭包注册表 |
| `via_hid` / `gtcall` emit 路径 | blob 调用的特例 |
| `yjit` clos_list / host_tab_fill 残留 | 首会话槽拷贝 |

`apply`/`icall` **仅**为嵌套 `fun` 闭包保留（Phase 2 不变），
以及 call/cc 式的动态调用。

## 实施顺序

1. **lir**：`letfun_finish` 对顶层 let 停发 closure+caps 机制；顶层函数
   的 `fvs`/`ncap` 归零；兄弟引用发射 `["gcall", dst, got_idx]`；顶层名
   的 `lir_var` 发射 `["gentry", slot, got_idx]`（装载入口地址，tagged）。
2. **emit**：`gcall` → `call [GOT+8k]`（AOT bake；blob 经 live fmap
   patch）；`gentry` → `mov [GOT+8k]` 装载。删 `gtcall`、`via_hid`、桥调用序列。
3. **runtime**：删 `yac_host_call`、`yac_gtab_get`、`yac_host_env_set`
   （`yac_host_sym` 保留给 cimport 路径）。
4. **jit/backend**：删槽填充与 `host_env_extra`；blob 路径变成：
   编译 → 追加 → patch GOT/funsym → 跳转。
5. **验证**：自举两遍、link 13 PASS、repl 26/26、
   `import compiler` + `compile("1+2")` e2e、blob_len > 0。

## Phase 2（Phase 1 全绿后）

嵌套 `fun` 字面量保留闭包对象（它们捕获局部变量——真正的动态环境）。
其调用仍走 `icall`；其自由变量仍来自闭包。只有顶层 flat 化，而 100%
的跨包痛苦都在顶层。

## 性能说明

每处兄弟函数引用变成一次 GOT load + call——与现在的 caps 解包（一次
load）同价，还省掉了调用点的前导参数准备。AOT 代码体积缩小（`_start`
不再构造闭包、不再 push caps）。blob 内的跨包调用从"表搜索 + 动态展开"
降为单次间接调用。
