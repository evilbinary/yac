# `.yjit` 作为链接模式 — 设计文档（BOOTSTRAP_LINK.md §12.7）

> 状态：**设计保留 / 立项草稿**。本文是重启条件的设计文档，不是承诺实现。
> 相关：`docs/JIT_IMAGE.md`（`.yjit` 影像格式）、`docs/BOOTSTRAP_LINK.md` §12.7。

## 1. 目标

让 `.yjit` 能作为 `--link` 的一种链接模式：一个进程里同时存在 **guest 影像**
和若干 **包影像**，guest 调到包导出函数时走内部 ABI 直调，包影像内的 C import
照常 bind。当前 `.yjit` 只是 REPL `:dump`/`:load` 的往返格式。

## 2. 现状与两个格式级阻塞（已核对代码）

1. **`.yjit` 不可重定位**
   - `pack_yjit_ex`（`src-self/back/pack/yjit.yac:72-86`）只写 hdr + TEXT + LINK，
     **没有 rela 段**；`YJIT_REL_*` 常量定义 6 处、使用 0 处（死常量）。
   - 影像按 `JIT_VADDR`（2^33）**绝对寻址烘焙**（glob、abs64 patch 都指向
     `JIT_VADDR + 偏移`），装载到别处必须整体重定位。

2. **一个进程只能有一张影像**
   - `yac_jit_run`（`src-self/rt/runtime.yac`）固定 `mmap` 16MiB @ 2^33 一次，
     基址存宿主 `G+112`；后续 load 只是往同一映射里拷，第二个 blob 没有自己的
     符号/重定位空间。
   - `jit_load_yjit`（`src-self/back/jit.yac:382-412`）语义是**替换当前会话**
     （export 表塞进 `jit_live_box`），不是"装载一个库并填另一张符号表"。

> LINK 段（`JIT_IMAGE.md` §4.4）的 import/export 表本身是完整的、可复用，
> 阻塞只在"重定位"与"多影像布局"这两处格式/运行时层面。

## 3. 重启方案

### 3.1 `.yjit` 加 rela 段（把 `YJIT_REL_*` 用起来）

- pack 侧：emit 阶段已收集的绝对 imm64 位置可参照 ELF 的
  `elf_note_abs64` / `elf_relocs_get`（`back/pack/elf.yac:82-89`）。为 `.yjit`
  新增同类的 `jit_relocs`（每个 reloc = TEXT 内位置 + 原值，8B/项），写进
  hdr 之后的 rela 段；hdr 增加 `rela_off / rela_len` 字段。
- 语义：`value += NEW_BASE - OLD_BASE`，OLD_BASE = `JIT_VADDR`。

### 3.2 运行时支持多影像

- 方案 A（一段式，先做）：单一 16MiB 映射 @2^33 不改，但把"当前影像"改成
  **影像表**：`G+112` 由单基址改成 `[nImages, base, per-image 元信息…]`；
  `jit_load_yjit` 追加新影像到未用区并做 rela，把包 export 登记到一张
  **会话级符号表**；guest fcall 到包导出改为先查该符号表。
- 方案 B（多段式）：`yac_jit_run` 支持多张 16MiB 段（基址递增），各段独立
  rela；成本高，先不选。
- C import 侧：`cimport_jit_bind` 已是 `dlsym(RTLD_DEFAULT)`（`emit.yac`），
  对任意 `.so` 天然可用，影像侧无需改。

### 3.3 语义

- REPL `:dump`/`:load` 继续走单影像（保持向后兼容）；`--link yjit` 走多影像
  装载。
- 包影像自带的 `yac_*`：多影像下每张都带一份 runtime/GC → 与 §12.6 embed 相同
  的跨影像 GC 域问题。**int-only 边界内成立**；真值传递需"共享 runtime"设计
  （同 §12.6 结论），超出本文。

## 4. 验收（最小 int 版）

1. 构造 3 函数假包 → `--format yjit` 产 `pkg.yjit`；guest 影像 + `pkg.yjit`
   在同一进程共存（两段 rela 后均能运行，互不踩）。
2. guest `import pkg; 调 f(int)` 经内部 ABI 直调得到正确结果（int-only）。
3. `.yjit` 内 C import（如 `dlsym`）在包影像侧 bind 成功。
4. REPL `:dump`/`:load` 单影像用例不回归（现有 `tests/run.yac` yjit dump/load
   PASS 保持）。
5. 无 rela 段影像在 `--link yjit` 下明确报错（格式版本检查）。

## 5. 改动面（估计）

| 区域 | 内容 |
|---|---|
| `pack/yjit.yac` | rela 段写/读、hdr 字段、版本号 |
| `rt/runtime.yac` | `yac_jit_run` 影像表/多段映射 |
| `back/jit.yac` | `jit_load_yjit` 装载+符号登记语义 |
| `emit` 各 arch | 复用现有 abs64 patch 收集出 rela（x86 先行）|
| `tests` | §4 验收用例（link 套件加 `yjit` 组或独立 runner）|
