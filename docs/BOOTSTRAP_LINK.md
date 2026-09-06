# 自举链接模式设计（yc ↔ guest / pkg / C）

> 本文是 `docs/compiler-host.md` 的替代设计。旧文档对应的实现（`lower.yac`
> 的 `is_host_extern` / `lw_host_rewrite`、`emit.yac` 的 `host_id`、
> G+96 host 表、`yac_host_sym`、`yac_host_unimpl`）是**单模式草稿**，只覆盖
> "AOT 桩 / JIT 宿主跳转" 一种语义。本文重新设计为**四种链接模式 × 三类
> 链接对象**的统一框架，面向自举链路 L4→L5→L6→L7（`Makefile` 的 `yc_a` /
> `yc_b` / `yc` / `bootstrap`）。

---

## 1. 目标与动机

yac 程序（guest）里可以引用三类**外部实现**：宿主 yc 的 host 编译器函数、
普通 pkg 包、C 共享库。它们有一个共同点：**实现不（总是）在 guest 源码里**，
但 guest 编译期 / 运行期必须能调用到。

本设计给出**统一的链接模式族**，对三类对象分别指定"实现放哪、guest 怎么
找到它"：

| # | 模式 | 实现载体 | guest 自包含 | 运行时依赖 |
|---|------|---------|:---:|---|
| 1 | `embed`（打包进 guest） | 机器码块重定位后**内嵌进 guest 镜像** | ✅ | 无 |
| 2 | `dylib`（ELF/PE/Mach-O 动态库） | `*.so` / `*.dll` / `*.dylib`，导出符号 | ❌ | 需要库文件 + loader |
| 3 | `yjit`（Yac JIT 影像，自有格式） | `.yjit`（不是 ELF；头 + TEXT + import/export 表） | ❌ | 需要 `.yjit` + `jit_map` |
| 4 | `stub`（不进入） | 不存在，调用即桩报错 | ✅ | 无 |

模式之间对 guest **源码可见面完全一致**：名字照常可见、可解析、可调用；差别
只在链接期把"调用"接到哪里。模式由 CLI 开关按对象选择，默认 `stub`（最安全）。

### 为什么把 `yjit` 拆成独立模式（第 3 种）

`.yjit` 是 Yac 自己的**内存镜像格式**（`docs/JIT_IMAGE.md`），不是 ELF：
文件 = 64B 头 + TEXT + LINK（import/export 表）；运行时 = `jit_map` 到
`JIT_VADDR` 直接执行，GOT/rela 由 yac 自己回填，**不需要 dlopen/dlsym 和
C 工具链**。

- 从 `embed` 看，它是"代码不进 guest 文本段、独立文件兜底装载"；
- 从 `dylib` 看，它是"共享库，但用 yac 自己的加载器（`jit_load_yjit`）而非
  系统 ld.so"。

两者语义都有但都不等于，所以是第 4 个 mode。它天然支持 `--format yjit` 产出
的 REPL 影像，同一套 export/import 表可被 `:load`/`:dump` 往返。

---

## 2. 三类链接对象

| 对象 | 例子 | 现状（只有源码链接 / cimport） | 本篇新增 |
|------|------|-------------------------------|----------|
| **H** host 编译器函数 | `import compiler` 的 `compile`/`compile_file`/`load` 及其叶子 | `lower.yac` 识别 10 名 → JIT hostcall / AOT 桩 | 三模式复用 |
| **P** 普通 pkg 包 | `pkg/io.yac`、`pkg/str.yac`、`pkg/ffi.yac` | **只有源码链接**（`backend.yac::lir_extend` 现编进 guest） | 包级 embed/dylib/yjit/stub |
| **C** C 共享库 | `ccall("printf",…)`、`import ffi` | **只有 `ccall` + libc import**（`elf_cimport_*` / `pack_elf_libc`） | 任意 `.so` 的 embed/dylib/yjit/stub |

注：H 的"实现"是预编译的宿主 yc 函数；P 的实现**目前只能是源码**；C 的实现
是系统 `.so`。三种对象通过同一套模式开关控制，逐个对象可覆盖（H 全图、P 按
包名、C 按符号）。

---

## 3. 统一通道：一切调用都收敛到"绝对地址 call"

无论哪种对象哪种模式，guest 内对外部名的调用最终都编译成
`["hostcall", dst, id, args]`（已有 LIR 指令），后端生成
`mov imm64 addr; call`。**区别只是 addr 在装载时怎么定**：

```
对象 H（host 表，G+96 已存在）:
  embed : GUEST_LOAD_VADDR + TEXT_OFF + (内嵌 blob 内 fnOff)
  dylib : loader 用 dlsym 在 yc.so 查得，写入 G+96 host 表
  yjit  : loader 读 .yjit import/export 表（jit_load_yjit），填入 G+96
  stub  : yac_host_unimpl（本镜像内）

对象 P（新：包级符号表，guest 内一张 name→addr 映射）:
  embed : 该包预编译 machine blob 内嵌 guest，重定位后绝对地址
  dylib : 包单独编成 pkg.so（--shared），guest 启动 dlopen/dlsym 填槽
  yjit  : 包单独编成 pkg.yjit（--format yjit --shared=包级），jit_load 填槽
  stub  : 包内函数调用 → yac_host_unimpl

对象 C（现有 cimport GOT，libc / 任意 .so）:
  embed : 无（C 代码无法嵌入 yac 镜像；不支持，报错）
  dylib : 现有 libc path（DT_NEEDED + dlsym/GOT）；任意 .so 复用此机制
  yjit  : .yjit 的 import 表已定义「dlsym → GOT 槽」（flags=0），现有
          cimport_jit_bind 已在做；等价于 dylib 的子集
  stub  : ccall 存在但符号解析失败 → 桩（返回 0 / 打印错误）
```

> 关键洞察：**H/P 走同一 `hostcall` LIR 与 G+96 式符号表**，P 只是把"host 名
> 集合"从硬编码 10 名扩展成"包导出符表"；C 保持现有 cimport 不动。

### 3.1 名词解释：`G+96` 是什么

`G+96` 是 yac 运行时的一个内存约定，**只存在于宿主 / JIT 场景**，AOT 产物
不直接依赖它（详见下文 3.2）。

- **G** = guest 程序的**全局区基址寄存器**（x86_64 为 r15），guest 全局变量
  都相对 G 偏移寻址。
- **+96** = 全局区中偏移 96 字节处的那个槽，专门存放 **host 函数地址表**
  （10 个已烘焙绝对地址的指针槽）。

机制沿此展开（见 `emit.yac:22-50`、`runtime.yac:1894-1916`）：

1. **id 编码**：`host_id(name)` 把 10 个宿主叶子映射为 0–9（`compile`=0,
   `compile_file`=1, `load`=2, `compile_native`=3 … `host_format`=9）。
2. **收集**：emit 时 `host_off_add(name, off)` 记录"名字 → 本镜像内文本偏移"。
3. **烘焙**：AOT 收尾把 10 个条目的**绝对地址表**写进 G+96，即"给宿主 yc
   自己用"的跳转表——JIT/REPL 代码靠它跳回宿主进程内的编译函数。
4. **取址**：`hostcall` 指令执行时 `yac_host_sym(id)` 读 G+96 槽 → 绝对
   `call`；槽为 0 表示无此函数 → 落到 `yac_host_unimpl` 桩。
   （x86_64 在 `emit_x86_64.yac:577-599`；arm64/riscv64 同理。）

### 3.2 `G+96` 与 AOT 的差别：谁真正拥有这张表

| 场景 | 谁是宿主 | host 表位置 | 填充者 |
|---|---|---|---|
| JIT/REPL（`JIT_VADDR`） | 本进程 yc | **宿主自己的 G+96** | emit 收尾烘焙 |
| AOT 独立 ELF（`T_VADDR`） | 无（guest 自己跑） | **不存在**，需新作 | —— |

所以三模式针对 AOT 加热解决"guest 没有宿主进程、没有 G+96"的问题：

- `stub`：调用直接指本镜像内 `yac_host_unimpl`（返回 0）；
- `embed`：把宿主 yc 的 host blob（重定位后）内嵌进 guest，**在 guest 自己
  的全局区新开一张等价 host/包地址表**并烘焙绝对地址；
- `dylib`/`yjit`：guest 启动时装库（dlopen/dlsym 或 jit_load），把解析到的
  地址写进**同一张新表**。

文中凡称"G+96 host 表/包符号表"均指这张抽象地址表：JIT 时它落在宿主
G+96，AOT 时它是 guest 内新建的等价数据结构（§5.3）。

---

## 4. 模式详解

### 4.1 `embed`（打包进 guest；对象 H、P）

语义：guest ELF 的文本段后**追加一段外部机器码**（宿主 yc 的 host blob / 某
pkg 的预编译 blob），并把调用重定位到该块内的绝对地址。guest 是**单个自包含
可执行文件**。

核心难点：外部 blob 是**按固定 vaddr 布局**编译的（`T_VADDR=0x400000`），内部
绝对引用（`mov imm64; call`、strlit、globals、host 表）都无效，需要整体重定位：

```
NEW_BASE = GUEST_LOAD_VADDR + TEXT_OFF + len(guest code)
blob 内每个绝对地址 A' = A(原布局) + (NEW_BASE - 原TEXT_BASE)
```

需要：
- **H**：宿主 yc 构建时落盘 `yc.host`（text 全字节 + `name→fnOff` 符号表 +
  绝对引用补丁表 `[fnOff+patchPos]`）——一次，放 `build/yc_tmp/`。
- **P**：包构建时同样产出 `pkgname.host`（该包 procs 的 emit 结果 + 补丁表）。

guest 编译（`--link embed`）时：读 `*.host` → 照常 emit guest 自身 code →
append blob → 重定位每个绝对引用 → 写 G+96 host 表（H）或包符号表（P）→
pack 成 ET_EXEC。

### 4.2 `dylib`（链接系统动态库；对象 H、P、C）

语义：guest 与其依赖的描述**分离**——H/P 各自编成独立动态库，C 用系统 `.so`；
guest 携带对这些库的引用，运行期由 loader 解析出绝对地址，之后照常绝对 call。

| 对象 | 库产物 | 装载 | 现有基础 |
|------|--------|------|----------|
| H | `yc.so`（`--shared` 已支持） | guest 启动 `dlopen/dlsym`（`cls`？现有 `rt/ffi.yac::cload/csym`） | G+96 host 表等待填充 |
| P | 每包 `pkgname.so` | 同 H | 包级 `--shared` 需新开关 |
| C | 任意 `libxyz.so` | **系统 ld.so**（`DT_NEEDED` + dynsym，`pack_elf_libc` 已做） | `elf_cimport_*` GOT 已有 |

环：`pack_elf_libc` 已支持 ET_DYN + dynsym（`elf_dynexp_set` 控制的导出表）+
RELA/JMPREL/PLT/GOT + `DT_NEEDED`（现在写死 `libc.so.6`）。新需求：
- DT_NEEDED 表可指向 `yc.so` / `pkgname.so`（把"libc.so.6"名字泛化）；
- 导出符号表里多写 H 的 10 名 + P 的每个导出函数名（`funsym_get` 已有）；
- PE/Mach-O 对应 `pe_finish_dll` / dylib 路径已存在。

### 4.3 `yjit`（链接 Yac JIT 影像；对象 H、P，C 是子集）

语义：依赖与 guest 分离，但载体是 Yac 自己格式 `.yjit`，装载走
`jit_load_yjit` / `jit_map`，不经过 dyld/ld.so。

| 对象 | 影像产物 | 装载 | 现有基础 |
|------|----------|------|----------|
| H | `yc.yjit`（`--format yjit` + host 导出） | `jit_load_yjit` 填 G+96 | `.yjit` 已有 export/import 表；`cimport_jit_bind` 已 bind C 导入 |
| P | 每包 `pkgname.yjit` | `jit_load_yjit` 填包符号表 | 同上 |
| C | 任意 `.so` 符号 | 影像 import 表 flags=0 → `dlsym(RTLD_DEFAULT)`（现有 `cimport_jit_bind`） | 已有 |

`.yjit` 的 LINK 段（`docs/JIT_IMAGE.md` §4.4）已经把
`import`（符号→GOT 槽）+ `export`（名称→TEXT/DATA 偏移）打通，所以 H/P 的
"符号表"不需要新格式，只是**在运行时 jit 会话里多登记一份 name→addr**。

### 4.4 `stub`（不进入；对象 H、P、C 缺省）

- **H**：现有 `yac_host_unimpl`（打印 "host fn unavailable"，返回 0）。
- **P**：包只提供**声明**（导出名 + arity 进 scope，report_unbound 放行）；
  函数体不链接；调用改写为 `yac_host_unimpl`。
- **C**：`ccall` 保留，但导入解析失败时报错/桩；已有 `pe_unimp_off_box` 类
  桩在 Windows 路径。

---

## 5. 普通 pkg 包（对象 P）——本篇重点新增

### 5.1 现状：只有源码链接

`backend.yac::pkg_src(pkg)` 读 `.yac` 源码 → `lir_extend` 现编入 guest。
优点（无重定位、类型一致）；缺点（每次编译重复、包不能预编译分发）。

### 5.2 包级模式开关（构建命令，不进语法）

> **原则：链接模式属于"分发/构建"语义，不属于"语言"语义。** 与 C 的
> `-static`/`-ldl`、Rust 的 Cargo features、Go 的 `-linkmode` 一致——
> import 表达"用到哪个接口"，模式表达"实现放哪"，两者分开。
> yac 现状同样如此：`--shared`/`--format yjit`/`--arch`/`--os` 全部命令行，
> 且 `import compiler` 在 AOT 自动是 stub、JIT 是 hostcall，**源码从未
> 写过模式**。因此不新增 import 语法、不改 lexer/parser/AST。

```
import io                        # 源码：保持现状，完全不写模式
import json {encode, decode}     # 选择器照常
import io as mio                 # 别名照常

# 模式全部走命令行（复用 `--link` 一个开关）：
yc --link dylib  main.yac                       # 无 `=`：全局唯一模式
yc --link embed,dylib,yjit main.yac             # 全局多模式 = 优先级链（全部支持）
yc --link io=dylib,json=yjit main.yac           # 包级覆盖：逗号分隔多个
yc --link compiler=embed main.yac               # 单个包覆盖，其余默认 stub
```

> **为什么不叫 `--pkg`**：`--pkg DIR[,DIR]`（`-p`）已被占用为**包搜索路径**，
> 语义是"去哪找包"，不是"包怎么链接"。改由 `--link` 统一：参数不含 `=` →
> 全局默认模式；含 `=` → `<pkg>=<mode>` 列表，按包覆盖。二者形态截然不同，
> 不会歧义，且不新增参数名。

- **CLI**：`--link <mode>[,<mode>…]` 是**优先级链**——模式均可单独给出
  （`--link embed` / `--link dylib` / `--link yjit`），逗号分隔多个表示
  "全部支持，按序回退"：如 `--link embed,dylib` = embed 优先、找不到产物
  回退 dylib。模式 ∈ `embed|dylib|yjit|stub`（stub 恒为链尾兜底，实际无产物）。
- **包级覆盖**：`--link <pkg>=<mode>[,<mode>…]`（如
  `--link io=dylib,yjit,embed`），可与全局链不同；一次可列多个包
  （`--link io=dylib,json=yjit`），重复 `--link` 可累积，同名后者胜。
  默认为 `stub`（包只有声明、调用走桩，现状语义）。
- **分发路径**：按链上模式依次找产物文件：`pkg_src` 先找 `pkg/io.yac`
  （源码），再按模式找 `pkg/io.yac.host` / `pkg/io.yac.so` /
  `pkg/io.yac.yjit`，命中即用。
- **编译期降级**：整条链（除 `stub` 外）都找不到对应文件 → 报错（不是静默回
  源码）；链含 `stub` 时落到 `stub`。

### 5.3 三实现的具体差异

| 步骤 | embed | dylib | yjit |
|------|-------|-------|------|
| 包编译 | 包单独 emit → `*.host` | 包单独 emit → `--shared` pack 成 `*.so` | 包独立 emit → `--format yjit` pack |
| 里表 | 符号表 + 补丁表落盘 | dynsym 导出（`elf_dynexp_set`） | `.yjit` LINK export |
| guest 装载 | append blob + 重定位 | 启动 `dlopen/dlsym` 填包符号表 | `jit_load_yjit` 填包符号表 |
| 调用 | 绝对地址 call | 绝对地址 call | 绝对地址 call |
| guest 文件 | 单文件 | 多一个 `.so` | 多一个 `.yjit` |

> 包符号表 = guest 里一张「包导出名 → 绝对地址」的表（与 G+96 host 表同构，
> 放 glob 区一段连续槽）。`hostcall` 指令复用，`id` 改为"包名+函数名"的哈希
> 或包内序号。

### 5.4 import → 加载时序（四种模式）

import 语句本身永远是**声明**：把包的导出名绑定到 guest 内的**地址槽**。
四种模式只差"槽位在哪个时机、由谁填充"，对名字的调用统一编译成
`["hostcall", dst, id, [args]]` → `mov imm64 addr(slot); call dst`。

```
import io                          # 语义：声明（名字 → 槽位），模式不参与
      │
      ▼ LOWER
"io.f" 调用 → ["hostcall", dst, id, ["f", args]]   # id 索引包符号表 / host 表
      │
      ▼ EMIT
mov imm64 <addr槽>; call dst        # init: 槽=0（未绑定）或桩地址
      │
      ├──────────────┬──────────────┬──────────────┐
      ▼ stub         ▼ embed        ▼ dylib        ▼ yjit
  编译期          编译期          编译期+运行期     编译期+运行期
 ────────        ────────        ────────────    ────────────
 pkg_src 不找    pkg_src 读      pkg_src 读      pkg_src 读
 产物。槽直接    io.yac.host    io.yac.so       io.yac.yjit
 指向桩：        = code +        → guest 记      → guest 记
 yac_host_unimpl name→fnOff      DT_NEEDED       .yjit 文件名
                 + 补丁表       "io.so"
 运行期：        emit 后 append 运行期启动：     运行期启动：
 调用→打印       + 重定位        cload(path)     jit_load_yjit
 "host fn       NEW_BASE=        +csym(name)     map 影像到
 unavailable"   TEXT_OFF         填符号表槽       JIT_VADDR，
 返回 0         +len(guest)                       读 LINK export
                 槽=烘焙好的     （C 库还走      （C import 已
                 绝对地址        系统 ld.so      在影像 import
                                 的 dlsym/GOT）   表里 bind）
      │              │              │              │
      ▼ guest 单文件自包含；调用 = 绝对地址 call（四种模式终点一致）
```

| | stub | embed | dylib | yjit |
|---|---|---|---|---|
| 槽填充时机 | 编译期 | 编译期 | 运行期 | 运行期 |
| 填充者 | 编译下场 | 重定位公式 | `cload`/`csym` | `jit_load_yjit` |
| 是否需要装载器 | 无 | 无 | 系统 ld.so / dlopen | yac 自带 loader |
| guest 自包含 | ✅ | ✅ | ❌（需 `.so`） | ❌（需 `.yjit`） |

> host 编译器函数（对象 H）复用同一时序：`stub` → `yac_host_unimpl`；
> `embed` → `yc.host` append 进文本段填 G+96；`dylib` → `cload("yc.so")` +
> `csym("yc_<name>")`；`yjit` → `jit_load_yjit("yc.yjit")` 读 host export 表。

### 5.5 `--link` 解析 → 编译 → import 装配时序

§5.4 画的是"名字已声明后，槽位怎么填"。这里补前半段：**CLI 的模式链如何
进到编译器，并最终落到某个 import 身上**。模式是*构建期*输入，不改变源码。

```
yc --link dylib --link io=dylib,yjit main.yac
│
├─[1] parse_args（yc.yac）
│    spec_flags.link_global = [dylib]                    # 无 `=` → 全局链
│    spec_flags.link_pkg    = {io: [dylib, yjit]}        # 有 `=` → 按包链
│
├─[2] parse（front/parser.yac）                ← import 语法不变
│    main.yac → ["program", [["import", "io"], …, 表达式…]]
│        每个 import 仍是 4 元组 ["import", pkg, pairs, alias]
│
├─[3] 解析 import（backend.yac::fill_import）
│    "io" → 查 spec_flags.link_pkg：命中 {io:[dylib,yjit]} → 记为包模式链
│    "compiler" → 无按包覆盖 → 用 link_global [dylib]
│    （二者之后统一进 pkg_src 的候选列表）
│
├─[4] pkg_src(io) 按链找产物             ← 每包独立分发
│    候选 = [io.yac(dylib→io.yac.so), io.yac(dylib→io.yac.so), yjit→…]
│    实际 = 按 源[io.yac]→dylib[io.yac.so]→yjit[io.yac.yjit] 顺序找
│            命中即停；stub 只做声明，链尾兜底
│
├─[5] lower（back/lower.yac）
│    "io.f" 调用 → ["hostcall", dst, id, ["f", args]]   // id 编 pkg+fn
│
├─[6] emit（back/emit/*.yac）+ pack（back/pack/*.yac）
│    embed : append blob + 重定位，槽烘焙绝对地址        // 编译期定稿
│    dylib : 记 DT_NEEDED"io.so"                         // 槽待运行期
│    yjit  : 记 .yjit 文件名                             // 槽待运行期
│    stub  : 槽 → yac_host_unimpl                        // 编译期定稿
│
└─[7] 产物 ELF / .yjit 落盘（回到 §5.4 运行期装载）
```

要点：

- **模式在 [1] 定、在 [4] 用**——import 语句（[2][3]）完全不知情，源码
  与模式零耦合。
- **链只会发生在 [4]**：全局链与按包覆盖合并成"每包的候选集"，
  顺序 = 源码 → 链上模式逐个找产物，首次命中即定稿。
- **链含 stub** 时，stub 只起"声明可见 + 槽指桩"作用，不参与产物搜索；
  链不含 stub 且全找不到 → [4] 报错。
- 同一包被 `--link compiler=embed` 与全局 `--link dylib` 同时命中时，
  **按包覆盖优先**（open question 可再细化为"链合并"）。

---

## 6. C 共享库（对象 C）——dylib 的现有实现与泛化

现状：
- `ccall("name", args)` 由 C 端 `rtio.c` 的 GOT/PLT 机制处理（ELF
  `pack_elf_libc` 写 `DT_NEEDED libc.so.6` + JMPREL；PE 走 `pe_finish_imports`
  + `emit_patch_pe_dlsym`）。
- JIT：`cimport_jit_bind` 对影像 import 表做 `dlsym(RTLD_DEFAULT)` 填 GOT。

泛化要点：
- 把 `libc.so.6` 的硬编码改成一个 **DT_NEEDED 名字数组**（libc / yc.so /
  pkgname.so / 任意客户 `.so`），`ccall` 与 `import ffi` 都能指到非 libc 库。
- JIT 的 `cimport_jit_bind` 已经是 dlsym→GOT，天然支持任意 `.so`；AOT 的
  `pack_elf_libc` 需要把名字参数从固定串改为列表。
- `rt/ffi.yac`（`cload`/`csym`）是 guest 侧手动 `dlopen/dlsym` 的入口，作为
  运行期兜底（dylib 模式下 host/包表也可用它填）。

这样 C 对象就**天然贯通三种模式的装载通道**：dylib=yjit（影像 import 表即
dlsym 槽），embed 对 C 无意义（报错）。

---

## 7. 统一接口与配置

```
yc --pkg DIR[,DIR]              # 现有：包搜索路径（不动）
   --link <mode>[,<mode>…]      # 全局优先级链（H/P/C 通用），默认 stub
   --link <pkg>=<mode>[,<mode>…]  # 包级覆盖：逗号分隔多模式；重复累积
   --embed-host build/yc_tmp/yc.host
   --yc-so    build/yc_tmp/yc.so
   --yc-yjit  build/yc_tmp/yc.yjit
```

- `yc.yac::parse_args` 增加 `--link`（参数含 `=` → 逗号分隔的按包覆盖列表，
  否则全局模式链；写 `spec_flags`，已有机制；import 语法与 AST 不变）。
- target **不新增字段**（避免改 `mk_target` 与三处后端签名）；link 模式作为
  lower/pack 的开关透传：`lower_expr(ast, t)` 增加可选 link 参数。
- H 的 host 名集合保持现有 10 名（`host_id` 映射不变），不破坏 host 表槽位。
- P 的"包符号表"是新增数据结构（glob 区一段连续指针槽，与 G+96 平行）。

---

## 8. 与现有 hostcall / JIT 的关系

| 场景 | target | 现有行为 | 落点 |
|---|---|---|---|
| REPL / `--cps` | `JIT_VADDR` | `["hostcall"]` 跳宿主 yc（G+96 表烘焙在宿主） | = yjit 的会话内特例（保留） |
| AOT 无开关 | `T_VADDR` | `yac_host_unimpl` 桩 | = **stub**（默认） |
| AOT `--link embed` | `T_VADDR` | (新) host/pkgs blob 内嵌 + 重定位 | 模式 1 |
| AOT `--link dylib` | `T_VADDR` | (新) host/pkgs `.so` + 启动 dlopen/dlsym | 模式 2 |
| AOT `--link yjit` | `T_VADDR` | (新) host/pkgs `.yjit` + jit_load | 模式 3 |

JIT / REPL 本质是 `yjit` 的"宿主即 guest"特例：host 表烘焙在宿主自己的
G+96，`hostcall` 绝对地址直接指向本进程已加载代码——与三模式共享同一套
`yac_host_sym` + `call` 指令路径。

---

## 9. 实现改动清单

| 文件 | embed | dylib | yjit | 共用 |
|---|---|---|---|---|
| `back/emit/emit.yac` | `host_blob_export()`（H/P 通用 code+abs patch+符号表） | — | `.yjit` rela 导出（`JIT_IMAGE` §5） | `host_id` 保持；新增 `pkg_sym_*` 表 |
| `back/emit/emit_x86_64/arm64/riscv64.yac` | blob 重定位 + guest 尾端符号表 | GOT/重定位槽 + 启动填槽 | 未 resolve 模块 + rela（`emit_apply_unres` 复用） | `host_off_add` 收集保持 |
| `back/lower.yac` | hostcall 落点 = 可重定位绝对地址 | 同左（loader 填） | 同左（jit_load 填） | 保持识别；`lw_rewrite_ins` 扩展包名 |
| `back/backend.yac` | `--link` 透传；`pkg_src` 支持 `.host/.so/.yjit` | `DT_NEEDED` 名字数组 | `--format yjit` pack 路径 | `pkg_src` 分发 |
| `back/pack/elf.yac` | blob 追加 + 重定位 | dynsym 多导出 + `DT_NEEDED` 数组 | — | `elf_dynexp_*`、`pack_elf_libc` 泛化 |
| `back/pack/yjit.yac` | — | — | 包独立 emit → `.yjit`；`jit_load_yjit` 填包表 | export/import 已有 |
| `rt/ffi.yac` | — | `cload/csym` 填 H/P 符号表 | 同左 | 已有 |
| `src-self/yc.yac` | `--link <m>,…`（全局链）CLI | `--link <pkg>=<m>,…` 覆盖 CLI | `--yc-yjit` CLI | `--link`/`--pkg` 解析（语法不变） |
| `Makefile` | 构建 yc 时产出 `yc.host` | 构建 `yc.so` | 构建 `yc.yjit` | bootstrap 选择模式 |

### 9.1 目录规划与影响位置

```
docs/BOOTSTRAP_LINK.md              # 本文档
build/yc_tmp/                       # 自举构建产物（现：yc_bundle/yc_a/yc_b）
  ├─ yc.host                        # (新) H embed：host blob = code + fnOff + 补丁表
  ├─ yc.so                          # (新) H dylib：--shared 产物
  └─ yc.yjit                        # (新) H yjit：--format yjit 产物
pkg/<name>.yac.host|.so|.yjit      # (新) 可选包分发产物，pkg_src 按模式链命中
```

改动文件一览（含现状耦合点）：

| 文件 | 影响 | 现状关联 |
|---|---|---|
| `src-self/yc.yac:12-118` | `parse_args` 增 `--link <m>,…` / `--link <pkg>=<m>,…` | `--pkg DIR[,DIR]` 已被占用（包搜索路径，不改） |
| `src-self/back/backend.yac` | `pkg_src` 按模式链分发 `.host/.so/.yjit`；`--link` 透传 | `pkg_src`/`lir_extend` 现为源码静态链接 |
| `src-self/back/lower.yac` | `lw_rewrite_ins` 包名映射；hostcall `id` 索引包符号表 | 保持 10 名 host 识别 |
| `src-self/back/emit/emit.yac` | `host_blob_export()`（H/P 通用）；新增 `pkg_sym_*` 表 | `host_id`/`host_off_*` 保持 |
| `src-self/back/emit/emit_x86_64|arm64|riscv64.yac` | blob 重定位 + 客端符号表槽 | `host_off_add` 收集保持 |
| `src-self/back/pack/elf.yac` | `DT_NEEDED` names 数组化；dynsym 多导出 | `pack_elf_libc`、`elf_dynexp_*` |
| `src-self/back/pack/yjit.yac` | 包独立 emit→`.yjit`；`jit_load_yjit` 填包表 | export/import 表已有 |
| `src-self/rt/ffi.yac` | `cload`/`csym` 填 H/P 符号表槽（dylib 运行期） | 已有 dlopen/dlsym |
| `src-self/rt/runtime.yac` | 包符号表槽区（与 G+96 平行的 glob 连续段） | G+96 host 表烘焙逻辑保持 |
| `Makefile:18-19,24-29` | 新增 `yc.host/yc.so/yc.yjit` 目标；bootstrap 选模式 | `YC_SRCS`/`YC_BUNDLE` 结构不变 |

不动的部分：

```
src-self/front/{lexer,parser,anf,cps,uncps,lir}.yac   # import 语法零改动
src-self/back/pack/{target,dlib,pe,macho,pack}.yac     # target 不新增字段
src-self/back/{jit,profile}.yac                        # REPL/JIT 行为保持（yjit 特例）
src/*.c                                                # C 参考实现不改（除非 libc 名数组）
```

新增逻辑块（无新文件）：

- **包符号表**：glob 区连续槽，`hostcall` `id` 索引（H 走 host 表、P 走新表）。
- **编译期降级**：链上除 `stub` 全找不到 → 报错；含 `stub` → 落 `stub`。

---

## 10. 验证

- **stub（默认回归）**：`import compiler` / `import ffi` guest 编译运行，
  `compile_file` 打印 "host fn unavailable" 返回 0；`make test` 全绿。
- **embed（H）**：含 `import compiler` 的 guest `--link embed` 产出单文件；
  objdump 确认 host 函数落在 guest 文本段内；**无 yc 二进制环境**单独运行成功；
  与 JIT 同输入对拍。
- **embed（P）**：`pkg/str.yac` 预编成 `.host` → 新 guest `--link str=embed` →
  单文件可运行，`str_cat` 等真执行。
- **dylib（H/P/C）**：`make yc.so` + `pkg/io.yac` → guest `--link dylib` →
  只有 `.so`、无 yc 可执行文件的环境运行成功；删除 `.so` → 报错显示依赖。
  `import ffi; ccall("printf",…)` 同环境互通。
- **yjit（H/P/C）**：`--format yjit` 产出 `yc.yjit` + `pkg.yjit` → guest
  `--link yjit` 运行时 `jit_load_yjit` 两影像 → 函数可调、C import 可 bind。
- **iso**：embed/dylib/yjit/stub 对 L4/L5 用例输出一致（host/pkgs 行为相同，
  仅落点不同），`make yc-iso` 保持。
- **三 arch**：`--arch arm64|riscv64` 下 embed host/yjit blob 用对应后端符号
  表/补丁；`run_tests.sh` 增补 qemu 用例。

---

## 11. 决策与开放问题

1. **embed 全量还是裁剪**：先全量嵌入 host blob（正确性优先），后按
   `drop_unreachable` 只收 reachable 闭包减体积。
2. **yc.so / 包 .yjit 导出符号命名**：建议 `yc_<name>` / `<pkg>_<name>`
   （防与 guest 内部符号冲突），dynsym `STB_GLOBAL`。
3. **loader 放哪**：优先 guest 启动代码（`rt/ffi.yac` 提供叶子），避免 C 端
   改动；`.yjit` 装载已有 `jit_load_yjit`。
4. **包符号表实现**：glob 区一段连续槽、`hostcall` 指令按 id 索引——最简单；
   需要时再升级为 hashmap。
5. **默认值**：保持 `stub`，不改变 `make test` / `bootstrap` 产物行为。
6. **C 对 embed 不支持**：设计明确报错（C 代码不可嵌入 yac 镜像）。