# 自举链接模式设计（yc ↔ guest / pkg / C）

> 本文是 `docs/compiler-host.md` 的替代设计。旧文档提到的 `is_host_extern` /
> `lw_host_rewrite` / `yac_host_unimpl` **在当前代码里已不存在**（它们是单模式
> 草稿并被移除）；真正保留下来的是 `emit.yac::host_id` + patch tag 21 +
> `G+136..G+216` host 表 + `yac_host_sym`，只覆盖"JIT 宿主跳转"一种语义，
> AOT 侧是**空的**（调用即崩溃）。本文重新设计为**四种链接模式 × 三类
> 链接对象**的统一框架，面向自举链路 L4→L5→L6→L7（`Makefile` 的 `yc_a` /
> `yc_b` / `yc` / `bootstrap`）。使用kiss原则，支持扩展性，可复用，OCP原则。
>
> **2026-09 代码核对**：§1–§11 的"现状"描述有若干处与代码不符，已在正文中
> 就地订正（标注 `[订正]` 或加备注块）。落地顺序见 **§12**。

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
| **H** host 编译器函数 | `import compiler` 的 `compile`/`compile_file`/`load` 及其叶子 | `emit.yac::host_id` 识别 10 名（`emit.yac:267-272`）；宿主 yc 由 `bake()` 把自身叶子烘进 `G+136` 槽、REPL 经 `host_tab_fill`（`jit.yac:25-34`）读取；**guest 作用域不含 host 名**（裸调即 `unbound`，12.1）；要用编译器须 `import compiler`（靠 `--pkg src-self` 源码内联） | 三模式复用 |
| **P** 普通 pkg 包 | `pkg/io.yac`、`pkg/str.yac`、`pkg/ffi.yac` | **只有源码链接**（`backend.yac::lir_extend` 现编进 guest） | 包级 embed/dylib/yjit/stub |
| **C** C 共享库 | `ccall("printf",…)`、`import ffi` | **只有 `ccall` + libc import**（`elf_cimport_*` / `pack_elf_libc`） | 任意 `.so` 的 embed/dylib/yjit/stub |

注：H 的"实现"是预编译的宿主 yc 函数；P 的实现**目前只能是源码**；C 的实现
是系统 `.so`。三种对象通过同一套模式开关控制，逐个对象可覆盖（H 全图、P 按
包名、C 按符号）。

---

## 3. 统一通道：一切调用都收敛到"绝对地址 call"

无论哪种对象哪种模式，guest 内对外部名的调用最终都收敛到"取一个绝对地址 →
间接 call"。**当前实现没有独立的 `hostcall` LIR**，而是普通 `fcall`：emit 在
`host_id(name) >= 0` 且本地无同名 proc 时改走 host 分支，发射
`mov rax, imm64 <槽地址>; mov rax, [rax]; call rax`，并挂一条 patch tag `21`
（`emit_x86_64.yac:570-583`、`emit.yac:744-745`）。**区别只是 addr 在装载时怎么定**。

> 下文 §4–§9 仍用 `hostcall` 作为这条通道的**抽象简称**；实现上它就是上述
> `fcall` + patch tag 21，不是独立的 LIR 指令。

```
对象 H（host 表，G+136 已存在）:
  embed : GUEST_LOAD_VADDR + TEXT_OFF + (内嵌 blob 内 fnOff)
  dylib : loader 用 dlsym 在 yc.so 查得，写入 G+136 host 表
  yjit  : loader 读 .yjit import/export 表（jit_load_yjit），填入 G+136
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

> 关键洞察：**H/P 走同一条 fcall-host 通道与 G+136 式符号表**，P 只是把"host 名
> 集合"从硬编码 10 名扩展成"包导出符表"；C 保持现有 cimport 不动。

### 3.1 名词解释：`G+136` 是什么

`G+136` 是 yac 运行时的一个内存约定，**只存在于宿主 / JIT 场景**，AOT 产物
不直接依赖它（详见下文 3.2）。

- **G** = guest 的**全局区**（glob area）。注意**它不是寄存器**：yac 代码不用
  r12–r15（`encode_x64.yac:794`），全局区是 TEXT 内的一段（紧跟代码之后），
  所有全局按**绝对 imm64** 寻址（`emit_x86_64.yac:1054-1070`、`:2014-2016`）。
- **+136..+216** = 全局区偏移 136 起的 **10 个 8 字节槽**，专门存放 **host
  函数地址表**（`emit.yac:256-263` 的 216 = 112 GC + 8 jit_map + 8 prof cell
  + 8 prof busy + 10×8 host 槽）。

机制沿此展开（见 `emit.yac:265-285`、`runtime.yac:2103-2117`）：

1. **id 编码**：`host_id(name)` 把 10 个宿主叶子映射为 0–9（`compile`=0,
   `compile_file`=1, `load`=2, `compile_native`=3 … `host_format`=9）。
   名字表是 `emit.yac:267-272` 的硬编码列表，可用 `host_names_set` 覆盖。
2. **收集**：emit 收尾时 `funsym_set(funOffsRev)`（名字 → 本镜像内文本偏移），
   pack 侧用 `funsym_get` 读。**没有** `host_off_add`（全库 0 处）。
3. **烘焙**：`bake()`（`emit_x86_64.yac:2028-2042`）遍历 10 个名字，在本镜像
   `funOffsRev` 里查得到才写 `LOAD_VADDR + TEXT_OFF + off` 进槽；查不到（且
   `T == 0`，AOT）写 **`yac_host_unimpl` 桩地址**（12.1 后；此前留 0）。
   只有 bundle 构建（`Makefile` 把 `src-self` cat 成一个文件，名字保持裸名）
   才会让前 3 个之外的大多数叶子命中 —— 这是 `skip_local_imp`
   （`backend.yac:341-348`）丢掉"同源自声明包"的 import 的结果。
4. **取址**：调用点读 `G+136+8*id` 槽 → 间接 `call`。
   **guest 语义（12.1 后定稿）**：host 名不在 guest 作用域，裸调在编译期即
   `unbound variable`，因此 AOT guest 不会发出 tag-21 host 调用；桩只在
   宿主 yc 自身 / 防御路径上被读到。
   （x86_64 在 `emit_x86_64.yac:570-583`；arm64/riscv64 因 host 名不再进
   guest 作用域，无需实现 host 分支——见 12.1.3。）

### 3.2 `G+136` 与 AOT 的差别：谁真正拥有这张表

| 场景 | 谁是宿主 | host 表位置 | 填充者 |
|---|---|---|---|
| JIT/REPL（`JIT_VADDR`） | 本进程 yc | **宿主自己的 G+136** | emit 收尾烘焙 + `host_tab_fill`（`jit.yac:25-34`） |
| AOT 独立 ELF（`T_VADDR`） | 无（guest 自己跑） | 槽被 `bake()` 填成桩地址（12.1 后） | `bake()`（空槽 → `yac_host_unimpl`） |

> AOT 下容易误读的事实：槽**存在**（`emit_glob_data` 每次都写 216 字节），
> 但 host 名**不在 guest 作用域**（12.1 后）：guest 裸调 `compile(...)` 在编译期
> 就报 `unbound variable`。guest 想用编译器只有一条正路：`import compiler`
> （配合 `--pkg src-self`），`pkg/compiler.yac` 里的 `import back.backend` 被
> `pkg_src` 解析到**源码**，于是**整个编译器被源码内联进 guest**（guest 变成
> 4MB 级）。也就是说 host 表在 AOT 下**从未被 guest 使用**，它只在 bundle
> 构建的 yc 自己身上有值，并由 REPL 会话读取；12.1 的桩保证任何读到的空槽
> 都指向可打印错误的过程，而不是 `call [0]`。

所以三模式针对 AOT 要解决"guest 没有宿主进程、host 调用落点从哪来"的问题：

- `stub`：槽指本镜像内的 `yac_host_unimpl`（打印后返回 0）——**12.1 已落地**；
- `embed`：把宿主 yc 的 host blob（重定位后）内嵌进 guest，**在 guest 自己
  的全局区新开一张等价 host/包地址表**并烘焙绝对地址；
- `dylib`/`yjit`：guest 启动时装库（dlopen/dlsym 或 jit_load），把解析到的
  地址写进**同一张新表**。

文中凡称"G+136 host 表/包符号表"均指这张抽象地址表：JIT 时它落在宿主
G+136，AOT 时它是 guest 内新建的等价数据结构（§5.3）。

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
append blob → 重定位每个绝对引用 → 写 G+136 host 表（H）或包符号表（P）→
pack 成 ET_EXEC。

### 4.2 `dylib`（链接系统动态库；对象 H、P、C）

语义：guest 与其依赖的描述**分离**——H/P 各自编成独立动态库，C 用系统 `.so`；
guest 携带对这些库的引用，运行期由 loader 解析出绝对地址，之后照常绝对 call。

| 对象 | 库产物 | 装载 | 现有基础 |
|------|--------|------|----------|
| H | `yc.so`（`--shared` 已支持） | guest 启动 `dlopen/dlsym`（`cls`？现有 `rt/ffi.yac::cload/csym`） | G+136 host 表等待填充 |
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
| H | `yc.yjit`（`--format yjit` + host 导出） | `jit_load_yjit` 填 G+136 | `.yjit` 已有 export/import 表；`cimport_jit_bind` 已 bind C 导入 |
| P | 每包 `pkgname.yjit` | `jit_load_yjit` 填包符号表 | 同上 |
| C | 任意 `.so` 符号 | 影像 import 表 flags=0 → `dlsym(RTLD_DEFAULT)`（现有 `cimport_jit_bind`） | 已有 |

`.yjit` 的 LINK 段（`docs/JIT_IMAGE.md` §4.4）已经把
`import`（符号→GOT 槽）+ `export`（名称→TEXT/DATA 偏移）打通，所以 H/P 的
"符号表"不需要新格式，只是**在运行时 jit 会话里多登记一份 name→addr**。

> **⚠ 格式前置未满足（见 12.7）**：按当前代码，作为**链接模式**的 `yjit` 走
> 不通，与"符号表格式"无关，是两处格式/运行时限制：
> 1. `.yjit` **不可重定位** —— `pack_yjit_ex`（`yjit.yac:72-86`）只写
>    hdr + TEXT + LINK，**没有 rela 段**；`YJIT_REL_*` 常量定义了 6 处、
>    使用 **0 处**（死常量）。影像按 `JIT_VADDR` 绝对寻址烘焙。
> 2. **一个进程只能有一张影像** —— `yac_jit_run`（`runtime.yac:2059-2066`）
>    固定 `mmap` 16MiB @ `2^33`，基址存在宿主 `G+112`，后续 load 只是往同一
>    映射里拷。`jit_load_yjit`（`jit.yac:382-412`）是**替换当前会话**、把
>    export 表塞进 `jit_live_box`，**没有"装载一个库并填另一张符号表"的语义**。
>
> 所以"guest 影像 + pkg 影像共存"做不到。本模式降级为**设计保留**，不进
> `--link`；`.yjit` 仍是 REPL `:dump`/`:load` 的往返格式，保持不变。

### 4.4 `stub`（不进入；对象 H、P、C 缺省）

- **H**：`yac_host_unimpl`（打印后返回 0）—— **当前不存在，由 12.1 引入**。
  在此之前 AOT 下调用 host 叶子是 **SIGSEGV**（槽为 0 → `call [0]`）。
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

> 包符号表 = guest 里一张「包导出名 → 绝对地址」的表（与 G+136 host 表同构，
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
> `embed` → `yc.host` append 进文本段填 G+136；`dylib` → `cload("yc.so")` +
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
- P 的"包符号表"是新增数据结构（glob 区一段连续指针槽，与 G+136 平行）。

---

## 8. 与现有 hcall / JIT 的关系

| 场景 | target | 现有行为 | 落点 |
|---|---|---|---|
| REPL / `--cps` | `JIT_VADDR` | `["hostcall"]` 跳宿主 yc（G+136 表烘焙在宿主） | = yjit 的会话内特例（保留） |
| AOT 无开关 | `T_VADDR` | `yac_host_unimpl` 桩 | = **stub**（默认） |
| AOT `--link embed` | `T_VADDR` | (新) host/pkgs blob 内嵌 + 重定位 | 模式 1 |
| AOT `--link dylib` | `T_VADDR` | (新) host/pkgs `.so` + 启动 dlopen/dlsym | 模式 2 |
| AOT `--link yjit` | `T_VADDR` | (新) host/pkgs `.yjit` + jit_load | 模式 3 |

JIT / REPL 本质是 `yjit` 的"宿主即 guest"特例：host 表烘焙在宿主自己的
G+136，`hostcall` 绝对地址直接指向本进程已加载代码——与三模式共享同一套
`yac_host_sym` + `call` 指令路径。

---

## 9. 实现改动清单

| 文件 | embed | dylib | yjit | 共用 |
|---|---|---|---|---|
| `back/emit/emit.yac` | `host_blob_export()`（H/P 通用 code+abs patch+符号表） | — | `.yjit` rela 导出（`JIT_IMAGE` §5） | `host_id` 保持；新增 `pkg_sym_*` 表 |
| `back/emit/emit_x86_64/arm64/riscv64.yac` | blob 重定位 + guest 尾端符号表 | GOT/重定位槽 + 启动填槽 | 未 resolve 模块 + rela（`emit_apply_unres` 复用） | `funsym_set` 收集 + `bake()` 烘焙保持 |
| `back/lower.yac` | hcall 落点 = 可重定位绝对地址 | 同左（loader 填） | 同左（jit_load 填） | 保持识别；无 `lw_rewrite_ins`（已删）；包名映射在 `lir.yac::resolve_call`/`pkg_qn` 侧 |
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
| `src-self/back/lower.yac` | 包名映射在 `lir.yac::resolve_call`/`pkg_qn`；host `id` 索引包符号表 | 保持 10 名 host 识别 |
| `src-self/back/emit/emit.yac` | `host_blob_export()`（H/P 通用）；新增 `pkg_sym_*` 表 | `host_id`/`host_names` 保持（**无** `host_off_*`） |
| `src-self/back/emit/emit_x86_64|arm64|riscv64.yac` | blob 重定位 + 客端符号表槽 | `funsym_set` 收集 + `bake()` 烘焙保持 |
| `src-self/back/pack/elf.yac` | `DT_NEEDED` names 数组化；dynsym 多导出 | `pack_elf_libc`、`elf_dynexp_*` |
| `src-self/back/pack/yjit.yac` | 包独立 emit→`.yjit`；`jit_load_yjit` 填包表 | export/import 表已有 |
| `src-self/rt/ffi.yac` | `cload`/`csym` 填 H/P 符号表槽（dylib 运行期） | 已有 dlopen/dlsym |
| `src-self/rt/runtime.yac` | 包符号表槽区（与 G+136 平行的 glob 连续段） | G+136 host 表烘焙逻辑保持 |
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

- **host 语义 + stub（默认回归）** ← **12.1 的验收依据**：
  - **语义（2026-09 定稿）**：10 个 host 叶子名**不在 guest 作用域**。裸调
    `compile(...)` → 编译期 `unbound variable 'compile'`（与其它未导入名一致）；
    必须 `import compiler`（经 `pkg/compiler.yac` 引入真实实现）才可用。
    `report_unbound_ex` 已把 `host_fun_names` 移出 guest 作用域
    （`backend.yac`）；host 表仅供宿主 yc 自己（REPL `host_tab_fill`）使用。
  - **现状（修复前基线）**：裸调 `compile(...)` **编译通过、运行 SIGSEGV**
    （`G+136` 槽为 0 → `call [0]`）。
  - **目标**：裸调在编译期即报错；`import compiler` / `import ffi` guest 编译
    运行正常；`yac_host_unimpl` 桩（打印 "host fn unavailable" 返回 0）作为
    **防御性兜底**仍被 `bake()` 填进任何空的 host 槽（例如宿主 yc 自己缺
    `compile`/`compile_file`/`load` 三个叶子的槽），避免未来任何路径读到 0。
  - 该两项已实测（Windows 原生）：裸调 rc=1 报 unbound；直接调用
    `yac_host_unimpl(0)` 打印消息并返回。`make test` 需在 Linux 跑全量。
- **embed（H）**：含 `import compiler` 的 guest `--link embed` 产出单文件；
  objdump 确认 host 函数落在 guest 文本段内；**无 yc 二进制环境**单独运行成功；
  与 JIT 同输入对拍。
- **embed（P）**：`pkg/str.yac` 预编成 `.host` → 新 guest `--link str=embed` →
  单文件可运行，`str_cat` 等真执行。
- **dylib（H/P/C）**：`make yc.so` + `pkg/io.yac` → guest `--link dylib` →
  只有 `.so`、无 yc 可执行文件的环境运行成功；删除 `.so` → 报错显示依赖。
  `import ffi; ccall("printf",…)` 同环境互通。
- **yjit（H/P/C）**：**阻塞**（格式前置未满足，见 §4.3 备注与 12.7）。
  原计划 `--format yjit` 产出 `yc.yjit` + `pkg.yjit` → guest `--link yjit`
  运行时 `jit_load_yjit` 两影像 —— 当前一个进程只能有一张影像且 `.yjit` 无
  rela 段，无法执行。重启条件见 12.7.2。
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

---

## 12. 落地计划

> 2026-09 按当前代码核对后重排。§1–§11 的分类学（4 模式 × 3 对象）保留，
> 但落地**不按矩阵一次铺开**，按下表顺序推进；做完一条打勾一条。
>
> 排序原则：先修正确性 → 再做与 emit 解耦的低风险面 → 再钉接口 → 再打通
> "运行时填表"通道 → 最后才动 emit 核心语义 → `embed` 收尾。越往后改动越
> 靠近编译器核心，回归面越大。
>
> 依赖：`0 ─► 1 ──┬─────────────► 4`
> `     2 ───────┘`
> `     3 ─────────► 4 ─► 5 ─► 6`；`7` 因格式前置未满足，外置。

### 12.0 文档勘误（P0）

本文 §3.1/§3.2/§4.3/§9/§10 的"现状"有 6 处与代码不符，按错的描述动手会打偏
（例如去改已删除的 `lower.yac::is_host_extern`）。先订正，再动代码。

- [x] 12.0.1 host 表偏移订正为 **`G+136..G+216`**（`emit.yac:256-263`）；
      并删掉"G 是 r15 基址寄存器"的说法——yac 代码不用 r12–r15
      （`encode_x64.yac:794`），全局区按**绝对 imm64** 寻址
- [x] 12.0.1b 不存在 `host_off_add`（全库 0 处）；收集靠 `funsym_set/funsym_get`
      + emit 收尾的 `bake()`（`emit_x86_64.yac:2028-2042`）
- [x] 12.0.2 不存在 `hostcall` LIR；是普通 `fcall` + patch tag `21`
      （`emit_x86_64.yac:570-583`、`emit.yac:744-745`）
- [x] 12.0.3 不存在 `yac_host_unimpl` / "host fn unavailable"；AOT 无开关时
      **不是桩，是崩溃**（槽为 0 → `call [0]`）
- [x] 12.0.4 `lower.yac::is_host_extern` / `lw_host_rewrite` 已删除
      （另：`host_off_add`、`lw_rewrite_ins` 也都不存在）
- [x] 12.0.5 `.yjit` **没有 rela 段**（`pack_yjit_ex` 只写 hdr+TEXT+LINK；
      `YJIT_REL_*` 常量定义 6 处、使用 0 处）
- [x] 12.0.6 `--shared` **已实现且测试覆盖**（`run.yac:572-639`），但导出是
      **C ABI int**（`emit_cabi.yac:126-134`），不是 yac 值
- [x] 12.0.7 §10 补"AOT host 槽为 0 → SIGSEGV"作为 12.1 的验收依据
- [x] 12.0.8 §4.3 / §10 标注 `yjit` 为设计保留、格式前置未满足

### 12.1 P0 — H 的 host 语义 + `stub`

> **定稿语义（编码时与用户确认）**：10 个 host 叶子名**不进 guest 作用域**。
> guest 要使用必须 `import compiler`（`pkg/compiler.yac` 把实现作为源码/宿主
> 依赖引入）；裸调 `compile(...)` 是 `unbound variable`。host 表与桩只作为
> **宿主 yc 自身的运行机制 + 防御性兜底**。

- [x] 12.1.1 `rt/runtime.yac` 新增 `yac_host_unimpl`（write1 打印
      "host fn unavailable\n" + 返回 int 0，0 参、标签 `proc`），登记进
      `runtime_funs`（`rt_host_unimpl_ins` 紧邻 `rt_host_sym_ins`）
- [x] 12.1.2 `emit_x86_64.yac` 的 `bake()`：先查 `yac_host_unimpl` 在
      `funOffsRev` 的偏移（`stub_off`）；host 叶子不在本镜像时把槽写桩地址
      而非留 0；`T != 0`（REPL 会话）分支保持不动
- [x] 12.1.3 host 名移出 guest 作用域：`backend.yac::report_unbound_ex` 去掉
      `host_fun_names(0)`。这使 arm64/riscv64 不再需要"host 分支"——裸调
      host 名在**前端**即报 `unbound variable`，不会到达 emit 的静默错跳；
      且 `pkg/compiler.yac` 通过 `import back.backend {compile_native, …}`
      照常绑定这些名字，`import compiler` 路径不受影响
- [x] 12.1.4 验收（Windows 原生已实测）：
      - 裸调 `compile(...)` → 编译 rc=1，`1:1: unbound variable 'compile'`
      - 直接调用 `yac_host_unimpl(0)` → 打印 "host fn unavailable"，程序正常
      - `l4_42`=42、`recursion`=120、`tests/pkg/path.yac`=42 不回归
      - `make test` 全量需在 Linux 跑（Windows 上 `tests/pkg/compiler.yac`
        因嵌套 PE 自编译在改动前后均 SIGSEGV，为既有问题，非本改动引入）
- [ ] 12.1.5 跟进：`make test` / `test-pkg` 在 Linux 上跑全量确认无回归

### 12.2 P0 — C 的 `DT_NEEDED` 泛化

> **暂缓（2026-09 决策）**：① 任意 `.so` 在 AOT 已通过运行时
> `cload`/`csym` + `ccall(ptr,…)` 支持（`tests/compiler/cases/ccall_cload`）；
> ② `DT_NEEDED` 只影响"字面名 ccall 由 ld.so 静态解析"，需要一个承载
> "符号→额外库"的入口（CLI `--lib` / `ffi.need` / 仅 packer 泛化），属公开
> API 决策；③ 本机为 Windows/PE，ELF 多 `DT_NEEDED` 只能做结构性验证。
> 待 Linux 环境 + 入口语义确定后重开。不做的话用 12.3 先行。

- [ ] 12.2.1 `cimport_*` 结构从 `names: [str]` 扩成 `[(libname, symname)]`
- [ ] 12.2.2 `pack_elf_libc`（`elf.yac:398+`）：dynstr 多库名 + `DT_NEEDED` 循环
- [ ] 12.2.3 `elf_cimport_bind`（`elf.yac:219-225`）启发式改按显式 libname 判定
      （避免 Win32 `LoadLibraryA` 被当 JUMP_SLOT → loader exit 127）
- [ ] 12.2.4 PE / Mach-O import 表同构改动
- [ ] 12.2.5 验收：字面名 `ccall` 绑到额外库（`libm`/自建 `.so`）在 AOT 可用

### 12.3 P1 — `--link` CLI 与 `pkg_src` 产物探测（先只探测 + 报错）

> 实现说明（2026-09）：状态不塞 `spec` flags，而由 `backend.yac` 内部两个
> box 承载（`link_global_box` / `link_pkg_box`），`yc.yac` 只调 `link_set(csv)`
> ——同样**不改 spec 元组宽度**。`stub` 在当前阶段 = **回到现有源码链接**
> （未给 `--link` 或链尾含 stub 时都源码链接，保证现状逐字节不变）；真正的
> "只声明不链接"语义等 loaders（12.4/12.5）落地后再精化。

- [x] 12.3.1 `yc.yac` 照抄 `--pkg` 分支加 `--link`；语法：无 `=` → 全局链；
      有 `=` → 包级覆盖（裸 mode 段续接最近的 `pkg=`；`--link` 可重复累积，
      **不做** `pkg_set` 的 once-only）；非法 mode / 空段 / 无 `=` 时混全局段
      → 解析失败
- [x] 12.3.2 `backend.yac` 加 `link_global_box` / `link_pkg_box` +
      `link_chain_of(pkg)`：**包覆盖（后写者胜，反查）> 全局链 > `["stub"]`**
- [x] 12.3.3 产物探测 `link_artifact(pkg, mode)`：包根下查
      `<pkg>.yac.host` / `.yjit`；`dylib` 按目标格式探测 `.so` / `.dll` /
      `.dylib` 三种后缀（`link_suffixes`），**只判存在**，返回内容或 0
- [x] 12.3.4 `lir_extend` 入口统一链解析（`pkg != rt.num` 时 `link_check_pkg`）：
      - 链无非 stub 模式 → 直接源码链接（默认/纯 stub，现状不变）
      - 命中产物但模式未实现 → `error: ... not implemented yet`
      - 全链（除 stub）无产物 → `error: no precompiled artifact (链)`
      - 链含 stub → 回落到源码（当前语义）
- [x] 12.3.5 Windows 原生行为验证：
      - 默认（无 `--link`）：`l4_42`=42 不回归
      - `--link dylib` + 无产物 → 编译 rc=1，报 `no precompiled artifact`
      - `--link path=dylib,stub` → 回源码，运行 42
      - `--link bogus` → `error: bad arguments` rc=2
- [x] 12.3.7 回归套件（Windows 实测 **9/9 PASS**）：`tests/run.yac` 新增
      `link` 套件 + `Makefile` `test-link` 目标。用例：
      default source-link(42) / global dylib no artifact / override
      dylib,stub→source / repeat override later-wins→source / bad mode /
      artifact-not-implemented（`.so` / `.dll` / `.dylib` 三种假产物各一）/
      help 含 `--link`
- [ ] 12.3.6 跟进：Linux 上 `make test` / `yc-iso` 确认**不带 `--link` 时
      产物与改动前逐字节一致**

### 12.4 P1 — H 的 `dylib`（**重定义 2026-09**）

> 原"10 个 host 叶子从 `yc.so` dlsym 填 `G+136`"的图景在 12.1 定稿语义下
> 不成立：guest 作用域不再含 host 名（裸调 = `unbound`），且 C ABI 只透
> int（`emit_cabi` shl/sar），H 的叶子几乎全是对象/字符串/bytes，无一可
> int-only 表达。真正有用的形态是 **P 包级 loader**：`import io` 在
> `--link io=dylib` 时，把 `io.yac.dll/.so` 的导出函数地址填进 guest 的一张
> **包符号表**，调用走间接跳转。这与 12.5 是同一机制（名字级外部符号 +
> 槽表），12.4 是它的应用面。因此重定义如下：

- [x] 12.4.A 决策（**被 option B 取代**）：不做 host/包连续槽扩展；
      dylib 绑定改由**合成包装包**实现（见 §12.5 阶段三）
- [x] 12.4.B 前置（**被 option B 取代**）：无 `emit_patch_rel` 名字级改动
- [x] 12.4.C 装载（**option B 实现，2026-09**）：guest 启动/调用经
      `ccall("dlopen"…)` + `ccall("dlsym"…)` 真调 C-ABI 产物，不需要启动段
      写槽的机器码 loader；extern 槽/`extsym_bind`/`--shared-int` 保留备用
- [x] 12.4.D 验收（option B，Windows PE 实测）：自建 `dadd.yac` `--shared`
      成 `dll` → guest `--link dadd=dylib` import 后调用 `add(2,3)` 真执行
      （绑定退出码 0，无桩输出）；`make test-link` 10/10、compiler 170/170
- [ ] 12.4.E 范围外（后续）：非 int 参数/返回值（值 ABI + 跨镜像 GC），
      `yc.so`（H）形态，Mach-O/ELF 装载

> 手工打样参考（当前即可跑，无需 loader）：yac `--shared` 编 `.dll/.so`，
> 驱动 `import ffi; load()/sym()/ccall(ptr,…)` —— 见仓库内
> `tests/compiler/cases/ccall_cload.yac` 与 §12.4.D 描述。

### 12.5 P2 — 名字级外部符号（槽间接调用；`embed`/loader 前置）

> **实现方案（2026-09 定稿）**：不另起"未解析 patch 携带名字 + 绝对值补丁"
> 体系，而是**复用现有 host 槽间接调用**（tag 21：`mov imm slot; mov rax,[rax];
> call`）：新增外部符号注册表，每个符号分到一个 **host 表之后的 8B 槽**
> （`G+216+8*i`，tag21 id = 10+i）。调用点与 host 同形态；启动期 loader 把
> `dlsym/dlopen` 解到的地址写槽；默认槽填 12.1 的桩（打印而非 `call [0]`）。
> 优点：无需超 2G 绝对调用、无跨镜像 rela；与 host 共用解析路径。
> **阶段一（本项，2026-09）已落地并回归**：
> - `emit.yac` 外部符号注册表 `extsym_add/find/n/reset`（名字列表 box）；
>   `emit_glob_data` 表区按 `216 + 8*extsym_n` 扩容（默认空 → 与之前逐字节一致）
> - `emit_x86_64` `fcall` 泛化：`host_id` **或** `extsym_find` 命中且非本地
>   proc → 槽间接调用（`id = host_id` 或 `10+extsym`），patch tag21
> - AOT（`T==0`）时 extern 槽默认写 `yac_host_unimpl` 桩地址
> - 回归：`link` 9/9、`compiler` 170/170（Windows 原生）
> - 边界：`tcall`/`closure` 走外部符号尚未覆盖（阶段二做）；arm64/riscv64
>   未接（同 host 现状，无需裸名则无影响）
>
> **阶段二（2026-09，已提交 f3cedb6）**：backend 侧落地"dylib 链包登记外部
> 符号、且不 source 链接"——`link_operative/link_is_dylib/pkg_extern_register`，
> `rt_for_link`/`visit_go` 跳过 dylib 包，`lir_extend` 对 dylib 包入口短路；
> `lir.yac` 导出 `pkg_qn`（登记键 = qname `pkg/f`）。link 套件 dylib 假产物
> not-implemented 三条换成真实 E2E（绑定编译 → 运行打桩 rc42；默认源码链接
> rc0），embed/yjit 产物仍 not-implemented。回归 `make test-link` 11/11；
> `pkg` 20 通过 + 1 既有失败（`pkg compiler` 139，历史编译器同样失败）。
>
> **⚠ ABI 前提（2026-09，编码 12.4.C 之前必须先纠偏）**：阶段一/二的 extern
> 槽调用点是**内部 ABI**（tag21 `mov imm slot; mov rax,[rax]; call rax`，
> yac 寄存器约定 + tagged 值），只能调"yac 编译出的函数"。而 `--shared` 产物
> 导出是 **C ABI**（`emit_cabi.yac` 的 `shl/sar` 包装，入口期望 C int）。
> 所以即便 loader 把槽填成 DLL 导出地址，内部 ABI 直调会把 tagged 值当 C int
> 传，结果错或崩——阶段二能跑只因为槽=桩、从未真调 C 导出。
> **两条出路（择一）**：
> - **A call-site cabi-shim**：emit 对 dylib 类 extern 在调用点内联
>   `emit_cabi` 式转换（参 `sar`、返回 `sal`），槽存 dlsym 地址；贴合 12.4
>   「只能传 int」的定位，改动集中在 emit。
> - **B guest 包装预置包**：生成合成包，包装形如
>   `add(a,b)=ccall(csym(cload(path),"add"),a,b)`，qname 落到 sigma 本地 fcall；
>   需惰性加载/缓存 + 导出 arity 来源，且须先确认 PE 下 guest 内 `dlopen/dlsym`
>   可用。
> 无论 A/B：对象 H（10 叶子）与普通 pkg 包的 `dylib` 都限于"C ABI int 只透"
> 边界，仅返回简单 int 的函数可表达；字符串/列表返回值需走 embed + 共享 GC
> 域（12.6），不在 12.4/12.5 承诺内。

> **阶段三（2026-09，option B 定稿并落地）**：dylib 绑定不再走 extern 槽 +
> 机器码 loader，而是 `rt_for_link` 为每个 dylib 包**编译一个合成包装包**
> （`pkg_dylib_synth`）：每个导出 `e(a0..a5) = ccall(ccall("dlsym",0,"e"),
> ccall("dlopen","path",258), a0..a5)`，按 `package pkg` 前缀编译，import 调用
> 经正常 sigma 路径解析到本地包装、经 ccall 真调 C-ABI 产物（int-only 边界）。
> 无新增机器码、无入口注入。`lir_extend` 拆出 `lir_extend_go(pkg,src,acc,
> synth)`：synth=1 时跳过"dylib 不编源码"短路但仍过 link_check。
> **验证**：`--link dadd=dylib` guest `add(2,3)` 真跑 DLL 退出码 0、无桩输出；
> `make test-link` 10/10。extern 槽/`extsym_bind`/`--shared-int` 保留备用。

- [x] 12.5.1 外部符号注册表（`emit.yac::extsym_*`）+ glob 表区条件扩容
- [x] 12.5.2 x86 `fcall` 对 host/extern 统一走槽间接调用（tag21，
      extern id = 10+i）；AOT 空槽默认填桩
- [x] 12.5.3b dylib 链包登记外部符号 + 不 source 链接（`f3cedb6`，阶段二前半）
- [x] 12.5.3 `tcall`/`closure` 的外部符号路径 —— **不需要**：option B 下 dylib
      包是 sigma 本地 proc，tcall/closure 走普通路径
- [x] 12.5.4 guest 启动段装载 —— **由 option B 取代**：包装包内部
      `dlopen/dlsym/ccall`（int-only 边界），无启动段机器码
- [x] 12.5.5 验收：自建 int 包 `--shared` → guest `--link pkg=dylib` import
      调用真执行（`add(2,3)` → 退出码 0，无桩输出；option B 包装包，2026-09）
- [ ] 12.5.6 arm64/riscv64 槽间接分支；x86 先行已通

> **loader 注入方式（2026-09 试验记录，避免重走；已被 option B 取代，不再需要
> 机器码 loader）**：试过在 `emit_x86_64`
> 入口函数 `local` op 的 `emit_chkstk` 之后直接 `call_rel32` 预留 loader 调用、
> 文本末尾追加 stub 再回填 rel32 —— **失败**：运行期跳进了 `win` system 桩
> （`CreateProcessA`）而非追加的 stub，说明入口与 `win_begin` bootstrap / 桩区
> 的坐标或执行顺序交互没对齐，且与 `pe_dlopen/pe_dlsym` 桩的关系也需理顺。
> **推荐改用"入口重定向"**：不在已发射函数体内插字节，而是 append 一段独立
> loader 函数（保存/恢复入口寄存器与 rsp），把 PE/ELF 的 entry 指到它，loader
> 填完槽后 `jmp` 原 `_start` 入口。入口选择位置在 pack 侧
> （`funsym`/`_start`），需先确认各格式 entry 字段从哪个 box/offset 取。

### 12.6 P3 — `embed`（依赖 12.1 / 12.3 / 12.5）

> **2026-09 立项决策：保持"设计保留"，不排实现。**
> 12.5 之后（option B）重新评估，`embed` 的独立收益在**当前 ABI/GC 约束下
> 基本消失**：
> - 若只做 **int-only** 的跨镜像调用 → 12.5 的 dylib(option B) 已覆盖且更简单，
>   单文件并不能带来增量价值；
> - 若目标是**单文件、无 `.so` 依赖、且能传 yac 值（字符串/列表）** → 这才是
>   真正场景，但它等价于"把包源码合入 guest"（= 现在的默认 source-link），
>   无需 embed；而把**预编译 blob** 合入则有 §4/§12.5 反复确认的硬伤：
>   blob 自带 runtime/GC（`drop_unreachable` 强制保留 `yac_*`），跨镜像传对象
>   → use-after-free；唯一解法是 blob **不带 runtime** 且 `yac_*` 在链接期指向
>   guest 副本（需"名字级未解析符号 + 共享 GC 域"，即已被 option B 替代的
>   `emit_patch_rel` 路线，重开成本高）。
> - 结论：**12.6 不为"实现"立项**；重启条件 = 出现真实使用场景要求"单文件 +
>   真值传递"且无 `.so`/源码可用。到时可复用 §12.6.1–12.6.6 的既有清单
>   （blob export/重定位/`bake` 表/GC 域方案），此处保留以备参考：

- [ ] 12.6.1（参考）`host_blob_export()`：`*.host` = code + funsym + reloc 位置表
- [ ] 12.6.2（参考）`pack` append blob：`NEW_BASE = LOAD_VADDR + TEXT_OFF +
      len(guest)`，reloc `value += NEW_BASE - OLD_BASE`
- [ ] 12.6.3（参考）`bake` 按包符号表填；槽区从 `G+216` 往后开
- [ ] 12.6.4（参考）smap / strlit pool 一并重定位
- [ ] 12.6.5（参考）体积：先全量后收闭包（§11.1）
- [ ] 12.6.6（参考）验收：objdump 确认函数在 guest TEXT 内；无 yc 环境运行；对拍

### 12.7 `yjit` 作为链接模式：暂缓，不进 `--link`

- [x] 12.7.1 在 §4.3 / §10 / §11 标注"设计保留，格式前置未满足"
- [x] 12.7.2 重启条件设计文档已出：**`docs/LINK_YJIT.md`**（`.yjit` rela 段 +
      `yac_jit_run` 多影像/影像表 + import 侧复用 `dlsym(RTLD_DEFAULT)` +
      int-only 边界说明 + 最小验收 5 条）。实现仍暂缓，需真实需求再投入。

### 12.8 工期参考（1 人全职、Linux、已通读 emit/pack）

| 里程碑 | 累计人日 | 日历 |
|---|---|---|
| M1 = 12.1+12.2+12.3 | 4–7d | 1–1.5 周 |
| M2 = M1+12.4 | 7–12d | 2–2.5 周 |
| M3 = M2+12.5+12.6（x86/ELF 单 arch） | 12–22d | 3–4.5 周 |
| M4 = 三 arch + 三格式完整 | 20–32d | 4–6.5 周 |

置信度：M1 高（±1d）；M2 中；**M3/M4 低**——12.5/12.6 悲观值可能再翻倍。
主要成本不是写代码而是定位：emit 出错只表现为 SIGSEGV 或静默错编，改
`src-self` 要走两轮原生自编译（`Makefile:58-68`），且要留意
`SELFHOST.md:258` 记的"编译器自己 `_start` 里 33 元 LIR cons 字面量会错编"。

压缩办法：先只做 x86_64 + ELF（省约 40%）；12.2 先只做 ELF；12.5/12.6
先用 3 函数的假包做最小可证伪用例。

### 12.9 link 测试清单（`make test-link`，实现见 `tests/link/run.yac`）

自包含 runner：`yc --pkg src-self tests/link/run.yac` 编译，带编译器路径运行。
guest 样例 = `tests/pkg/path.yac`（import `path`）；dylib 样例在运行时生成到
`build/test_tmp/`。

| # | 名称 | 场景 | 断言 |
|---|---|---|---|
| 1 | default source-link | 不带 `--link` 编译 import 包 guest | rc 42 |
| 2 | global dylib no artifact | `--link dylib` 但无产物 | 报 `no precompiled artifact` |
| 3 | override dylib,stub -> source | `--link path=dylib,stub` | rc 42（回落源码）|
| 4 | repeat override later wins | 两次 `--link path=…` | 后者生效 → rc 42 |
| 5 | bad mode | `--link bogus` | 报 `bad arguments` |
| 6 | artifact .host not implemented | 假产物 `.host` + embed | 报 not implemented |
| 7 | artifact .yjit not implemented | 假产物 `.yjit` + yjit | 报 not implemented |
| 8 | dylib bind real exec rc | 真 `dadd.yac.dll` + `--link dadd=dylib` | rc 0（DLL `add` 真跑：`add(2,3)=5`、`add(20,22)=42`）|
| 9 | dylib default rc | 同 guest 不带 `--link` | rc 0 |
| 10 | missing artifact diag | 删产物后重跑绑定程序 | rc 1 + 输出含 `load failed` |
| 11 | link help | `yc -h` | 含 `--link` |

> 测试组织说明：link 套件内联在 `tests/run.yac` 时不便通读，已按 `tests/boot`
> 先例抽到 `tests/link/run.yac`；`tests/run.yac` 的 `link` dispatch 与 `all`
> 链经 `link_front` 委托子 runner（无代码重复）。

### 12.10 后续小特性：dylib 解析缓存（2026-09 立项，暂缓实现）

**动机**：option B 的合成包装包当前**每次调用**都 `dlopen/dlsym`
（`pkg_dylib_synth`，`backend.yac`）。重复调用会反复加载/查符号，且每导出泄漏
一个句柄。想缓存"已解析的 C 导出函数指针"，让后续调用直接 `ccall(ptr, …)`。

**已排除的路径（勿重走）**：包内顶层 `box` 缓存**不可行**——包作用域函数引用
同包顶层 `box` 在运行时崩溃（仅 main 文件支持该写法；已实测并回退，见
`a258755`）。

**拟定方案（二选一，实现时再定）**：
- **A 镜像内全局槽**：在 `emit_glob_data` 的表区（`G+216` 之后或扩一段）为每个
  extern/导出预留 8B 槽；启动前槽值 = 0（= 未解析）。合成包装判断槽为 0 才
  `dlopen/dlsym`，成功后把解析地址写回该槽；后续调用直接读槽走 `ccall`。
  写槽需要 guest 侧写绝对地址 → 需要一个 kernel 叶子（仿 `yac_host_sym`，
  `runtime.yac:2103-2117` 用 `$gbase` 读槽）提供"写第 i 个缓存槽"的能力。
- **B kernel 叶子 + 全局缓存**：kernel 提供一个
  `yac_dylib_cache(i, dlsymArgs…)` 型叶子，内部用 `$gbase` 定位缓存区并完成
  "已缓存→返回指针 / 未缓存→dlopen+dlsym+写缓存"。
  依赖：先确认 kernel LIR 里能做 `dlopen/dlsym` 的等价调用（ccall/import 通路），
  或由缓存叶子只存/取、解析仍放 guest。

**验收**：guest 连续调用 `add(2,3)`、`add(20,22)`（复用 `tests/link/run.yac` 的
样例）仍 rc 0；加断言/日志证明第二次调用不再走 `dlopen`（例如临时计数或删掉
产物后缓存命中仍能跑——注：缓存语义下删产物后第二次调用**应仍成功**，这是与
12.9 第 10 条"删产物报错"的区别点，实现时需厘清两者关系）。

### 12.11 `@host` import 级 host 绑定（2026-09 落地）+ 遗留问题

> 与 §5.2"模式全部走命令行、import 不进语法"的例外：host 是实现来源的一种
> 标注，但被它标注的名字**不进 guest 源码链接闭包**（性质与 dylib 的链模式
> 相同），因此把选择放 import 内、以 `@` 前缀表达，比再造一个 `--link` 段
> 更贴近"这是该名字的固有来源"。§5.2 的"import 只是声明接口"仍然成立：
> `@` 不改接口语义，只标注实现来源=宿主叶子。

**动机**：`import compiler` 旧路径把整棵 `back/front/emit` 编译器树
source-embed 进 guest（REPL 实测 ~37s）。`pkg/compiler.yac` 只是对 10 个
宿主叶子的薄包装，embed 整树纯属浪费。

**已落地（2026-09，Windows 原生实测）**：
- 语法：`import pkg {@name}`，选择项 `@name` → AST `[name, name, 1]`
  （长度 3 = host 标记；lexer 无需改动，`@` 天然是 punct）。
  不带别名/`as`。
- `front/parser.yac::parse_imp1` 识别 `@`；`@` 判定用字符比较（str-slice 与
  字面量 `==` 不可靠，见项目已知坑）。
- `front/lir.yac`：`host_slot_names`（10 名）+ 顶层递归
  `host_match_i`/`is_host_name`；`imap_add_pair` 对长度 3 spec 存 **bare 名**
  （不加 `pkg/` 前缀）→ `resolve_call` 保持裸名 → emit 的 `fcall` 走既有
  host 槽分支（tag 21）。**注意不要**把 `host_match` 写成 `is_host_name` 内的
  局部递归 `let go(i)=…`，那会在自举产物中引发不稳定段错误（见遗留 2）。
- `back/backend.yac`：`import_all_host` + `fill_import`（纯 host import 不读
  目标包导出、不进 `link_need_box`）+ `ast_imports`（DFS 不深入全 host import
  的包）。
- `pkg/compiler.yac` 改为 host 视图：6 个 import 全部 `@`，只留薄壳包装。

**验收现状**：
- REPL `import compiler` 0.6s（原 ~37s）；AOT/REPL 均不再段错误；
  `make test-link` 12/12；平凡 AOT 编译无回归。
- 裸 `compile(...)`（未 import）仍 `unbound`（12.1 语义保持）。
- REPL 会话内调用 host 编译函数（如 `compile`）会**污染宿主 REPL 状态**，
  会话后续行可段错误；单次调用后立即 `:q` 不崩（详见遗留 1）。本小节
  `import compiler` 的验收只承诺"瞬时 + 绑定 + 不触 host 不崩"。
  **语义澄清**：`compile` 正常返回值是**机器码 blob（bytes）**；当前 REPL
  里返回 int 0 是宿主后端在无 pkg 根/状态被污染下失败（非桩、非"编译出
  整数 0"）。判成功用 `compile(...) != 0`（或 len>0），不能把 0 当合法产物。

**遗留问题（后续处理，勿丢）**：
1. **host leaf 真值 + REPL 状态隔离（2026-09 修正认知）**：
   - **bake 事实**：宿主 yc 是 cat bundle 自举（`skip_local_imp` 吞 import、
     函数保裸名），故宿主映像里确实存在裸名 `compile_native` 等 → `bake()`
     命中 → **宿主槽指向真实函数**，不是桩。REPL `host_tab_fill` 把它拷进
     guest 槽。
   - **REPL 污染**：REPL 里 guest 一调用 `compile`（真 host 后端函数），
     就在**宿主进程内**递归跑编译器后端，改写宿主 REPL 正在用的全局状态
     （imap/pkg_prefix/link box/emit_jsess…）；该会话**后续行**即段错误
     （实测 `compile;load;1+1`、`compile;compile;compile_file` 均崩，
     `compile` 单次后立即 `:q` 不崩、`load`（文件缺失早退，不触 host）多行
     不崩、无 compiler 的多行不崩）。`compile("1+2")` 返回 0 是 host 后端
     在无 `--pkg`/pkg 根状态下失败返回（不是桩）。
   - 因此要让 REPL `compile/load` 真正可用，必须做**宿主编译状态隔离**
     （host 调用前保存/恢复宿主全局，或 host leaf 只暴露无状态入口）——
     架构级，与 12.4.C loader 无关。在此之前可承诺的语义：`import compiler`
     瞬时、绑定、无 host 副作用；调用 host 编译函数会污染当前 REPL 会话。
   - 补测试时只断言"import 瞬时 + 不崩 + 裸 `compile` unbound"，不把
     host 调用纳入（直到隔离落地）。
2. **free_vars host-skip 不能加回（2026-09 实测，已绕过）**：
   在 `free_vars` 中加 `is_host_name` 跳过捕获后，编译器自举产物对**任意**
   AOT guest（含空文件）段错误。已二分与实现形态无关，疑"free_vars 引用
   另一顶层函数"的代码形态触发编译 bug，根因未定。
   **替代方案（已实现并规避此问题）**：不动 free_vars；在 lir 增加
   `sigma_host_seed(fs)`（从 imap 取 host bare 名，向 sigma map 预置
   `[name,0,-1]` 条目，procs 列表不变 → 不进 emit ids、仍是 host 槽
   fcall）。`backend.lir_extend_go` 的初始 sigma 用
   `sigma_host_seed(sigma_of_rt(acc))`。效果：free_vars 把 host 名视为
   ncap0 已知函数 → 不捕获；包装函数 ncap=0，调用形态正常。**遗留中 2 的
   "load 段错误"已因此消失**（`load("a.yac")` 文件缺失早退返回 1）。
   若日后要彻底去掉 seed，可再回头查 free_vars 崩溃根因。
3. **`@name` 的宿主表校验缺失**：`@x` 若不在 host_names，当前**不报错**，
   会按 bare 名绑定 → emit `host_id = -1` 且无 extern → 落入普通 fcall
   却无 label，行为未定义。后续应在 `fill_import`/ub 阶段校验并报
   "host 包 `pkg` 的导出 `x` 无宿主实现"。
4. **`compiler.yac` 实现体已删**：未来 `embed` 形态（`--link compiler=embed`）
   需要源码实现，届时需把包装函数体恢复为普通 `import back.backend`
   版本或另设文件（与 host 视图互斥：同一文件同时 host 快 + embed 可用
   不可兼得）。

### 12.12 REPL host 状态隔离——方向调研（2026-09，未动手）

遗留 1 的 REPL 污染到底污染了什么、怎么隔离，先做静态盘点：

- `backend.compile_native`（`backend.yac:867`，host 槽指向的真函数）**已有
  保存/恢复**：`emit_jsess` / `imap_box` / `malias_box` / `pkg_prefix` /
  `link_need_box` / `link_local_box` / `yjit_layout`，主路径结束时逐项还原。
- **缺陷 A（2026-09 已修）**：早退分支（syntax/unbound）只恢复
  `emit_jsess`。已改为统一走 `compile_native_restore`（imap/malias/
  pkg_prefix/link_need/link_local/yjit_layout/emit_jsess），主路径同用。
  link/repl 套件无回归。
- **实测仍崩 → 缺陷 B 升级**：修复 A 后在 REPL 里
  `compile("<语法错误串>")` 或 `compile("1+2");load;1+1` 之后会话**依旧**
  段错误。说明污染来自保存清单之外，候选：emit/pack 侧全局（`funsym`/
  `elf_dynexp_*`/`extsym_*`）、12.5 `link_*`/`extbind_box`、或更深层
  （host 后端在宿主进程内跑完整 pipeline 的副作用）。下一步应对照
  "REPL 逐行编译"与"host compile_native"各自触碰的全局求差集，逐一
  save/restore；若仍崩则怀疑 guest↔host 调用/堆层（非 box 状态）。
- **方向（候选，实现时选）**：
  a. 把 compile_native 的保存清单补成"贯穿全局全集"，主路径与早退一致；
     自举/REPL 共用，纯增量。
  b. host leaf 改暴露无状态包装（宿主侧新建独立编译 session：自带一份
     imap/link/emit 状态，不碰宿主 REPL 的盒子），更彻底但改动面大。

### 12.13 编译器环境 ctx 化（阶段 2 实施计划；2026-09 立项，按序执行）

> 目标：编译器**可重入**——每个编译会话有独立环境，host leaf / REPL /
> 未来的 dylib loader / embed 都能"编译里再编译"而互不踩踏，最终删除进程级
> 环境全局。改动集中在自举核心，**每一批必须自举成功 + link/repl 套件回归**
> 再进下一批（中途失自举会阻塞一切）。

**ctx（编译会话环境）实体**：yac 值（list/record）。初版字段 = 现全局盒的
归属映射：

| 字段 | 现全局（进程级） | 归属 | 备注 |
|---|---|---|---|
| imap | `imap_box` | 会话 | import 名→目标映射 |
| malias | `malias_box` | 会话 | 模块 as 别名 |
| pkg_prefix | `pkg_prefix_box` | 会话 | 当前包前缀 |
| link_need / link_local | `link_need_box`/`link_local_box` | 会话 | --link/import 闭包 |
| emit_jsess / yjit_layout | 同上 | 会话 | REPL/JIT 会话层 |
| funsym / elf_dynexp / extsym / patches | `funsym_box`/… | 单次 emit/pack | 逐次产出 |
| host_names / kernel 名 / pkg 导出缓存 / pkg_fail | box | **进程级只读/引导** | 不随会话变，保持全局 |

> **2026-09 前置调研结论（B1 开工前）**：缺陷 A 修复后做 REPL 组合实验，
> 崩溃**与具体 host 调用无确定对应**，而与"会话内继续求值的行数/分配量"
> 相关：单个 `compile("1+2")` 后接一行普通表达式**不崩**；再接第二行就崩
> （`compile,load,1+1`、`compile,compile,1+1` 稳定崩；纯普通行、纯 `load`
> 多行不崩）。这**不符合纯 box 状态泄漏**（那些应在首次后续行即崩），
> 更像宿主后端大分配活动与 REPL 会话**共享 GC 堆/根表**的相互干扰。
> 因此 B1 若走"扩展全局切换清单"可能仍不解决；需先区分：是 box 残留
> （切清单可解）还是堆/GC 层（ctx 化亦不够）。若后者，务实方案可能是
> "REPL host 编译走独立子进程"或"REPL 不支持调用 compiler（import 仅视图）"。
> 实施 B1 前先做一次判别实验（在 compile 返回后人为触发 GC 再执行下一行；
> 或临时禁止 host 后端分配后观察）。

**批次**（每批独立提交）：
1. **B1 ctx 实体 + 入口会话化**：`backend` 新 `compile_ctx(src, t, ctx)`；
   `compile_native` = `compile_ctx(src, t, new_ctx())`；host 调用方（REPL host
   leaf 的入口封装）每次 new 一个 ctx，退出即弃。B1 实现上仍以"进入时把
   进程全局绑定切到 ctx 值、退出切回"垫底（保证自举不中断），**验收**：REPL
   `import compiler` 后 `compile("1+2")` / 语法错 / `load` 均不崩、后续行可用；
   link 12/12、repl 26/26。此步同时把 §12.12 缺陷 B 的遗漏全局（funsym/
   dynexp/extsym/link_*）纳入 ctx 切换清单——先实测定位哪些必须切。
2. **B2 lir 词法环境参数化**：`front.lir` 的 imap/malias/pkg_prefix 从全局读
   改为显式参数（`resolve_call`/`imap_load` 族/ub/free_vars/lir_all st 携带
   ctx）；backend/jit 调用点传 ctx。**验收**：自举 + 全套件无回归 + REPL
   host compile 仍不崩（此时嵌套编译已不依赖进程全局切回）。
3. **B3 emit/pack 会话化**：funsym/dynexp/extsym/emit_jsess/yjit_layout 经 ctx
   贯穿（emit/pack 调用链签名扩展）。**验收**：同上 + 并行两路编译（同一进程
   两 ctx 交错）正确。
4. **B4 收尾**：删/闲置被取代的进程级环境全局；冻结 `compile_ctx` 接口文档；
   新增嵌套编译回归用例（REPL host `compile`/`load` 调用并入 repl/link 套件）。