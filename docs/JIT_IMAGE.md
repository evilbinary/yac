# JIT Image（Yac 内存映像）

状态：**部分实现。** 里程碑 1 + REPL append：`--repl` 每行只编译该行，机器码追加到同一 16MiB 映射；`let` 槽在映像 12MiB 处。尚未：独立 RODATA/DATA 映射、reloc 表延后、W^X。

磁盘 ELF / PE / Mach-O 仍由 `pack_*` 产出；本格式 **不是** ELF，逻辑上与 ELF 段角色同构，便于以后 dump 再 pack。

相关：`docs/DESIGN.md` §2.1 机器码路径；`docs/SELFHOST.md` 原生 `--repl` / `0x200000000`。

---

## 1. 目标

| 目标 | 做法 |
|------|------|
| 会话在映像里 | REPL 不拼接源码、不改 AST；新行只追加代码并 `call` 新入口 |
| 可重定位 | 绝对引用只出现在 reloc 表；加载 / dump 时由链接器回填 |
| 可增长 | 代码、数据、链接元数据分 **独立 VM 段**，互不顶开 |
| 符号可达 | 后续模块按名引用已加载的 proc / 全局槽；`ccall` 走导入槽 |
| 可 dump | 同一套段 + reloc + 导入名 → `.yjit`，或再 `pack` 成 ELF/PE |
| W^X | 代码段按页 RW 写入后改 RX；数据段一直 RW。v1 可暂缓，先独立映射 |

非目标（v1）：通用 Segment/Section 表、DWARF unwind、把客堆打进映像、真 ELF 头 mmap。

---

## 2. 两种布局

**文件**（`.yjit` / dump）：节按顺序紧挨着，`.bss` 可以 filesz=0。  
**运行时**：每段一块预留好的映射，中间留空洞；header 可以拷到代码段前 64 字节，或单独一页（推荐单独一页，避免改 header 时碰 RX）。

```
运行时 VA（示意，基址仍 JIT_VADDR = 0x200000000）

  HDR     1 页     RW     目录；eval_off / *_end 会改
  TEXT    预留 N   RX*    机器码 + PLT；*写入中的页暂 RW
  RODATA  预留 N   R      strlit、smap、函数起始表
  DATA    预留 N   RW     GOT、GC 全局、let 槽
  LINK    预留 N   RW     export / import 名索引 / rela / 字符串
                          （也可不映射，全放宿主；dump 时再写出）

客 GC 堆仍在独立 arena，不进本映像（快照另议）。
```

线性「hdr + text + rodata + data + import + …」**不能**当活 REPL 映像：`.text` 一涨，后面全得 memmove。

建议预留（可调）：TEXT 16MiB，RODATA/DATA/LINK 各 4MiB。超出再 `mremap` / 额外映射（v2）。

---

## 3. Header v1（64 字节，小端）

偏移从 HDR 映射起点算。Magic：ASCII `YJIT`（`0x54494A59` 若按 u32 小端读则是 `'Y'|'J'<<8|'I'<<16|'T'<<24`）。

| off | 类型 | 名 | 含义 |
|-----|------|----|------|
| 0 | u32 | magic | `'YJIT'` |
| 4 | u16 | version | `1` |
| 6 | u16 | arch | `1` x86_64，`2` arm64，`3` riscv64 |
| 8 | u16 | abi | `0` SysV，`1` Win64（与 `emit_abi_win` 一致） |
| 10 | u16 | flags | bit0 = 已 bind 导入（活映像）；dump 前清 GOT 并清此位 |
| 12 | u32 | hdr_size | `64` |
| 16 | u64 | base | 加载基址。JIT 默认 `0x200000000` |
| 24 | u32 | entry | **TEXT 内**字节偏移：当前 `_eval`（REPL）或 `_start`（exe dump） |
| 28 | u32 | reserved0 | 0 |
| 32 | u32 | text_off | TEXT 在文件中的偏移（运行时：相对 TEXT 映射起点为 0） |
| 36 | u32 | text_size | 已提交代码字节 |
| 40 | u32 | ro_off / ro_size | RODATA 文件偏移与已提交大小 |
| 48 | u32 | data_off / data_size | DATA 同上 |
| 56 | u32 | link_off / link_size | LINK 同上（文件）；无 LINK 段则为 0 |

运行时另有 **不进这 64 字节、只在宿主** 的游标：`text_cap`、`ro_cap`、`data_cap`（映射容量）。`*_size` 是已用。

文件里若不用独立 HDR 页：这 64 字节放在文件最前，`text_off = 64`（或对齐到 16）。运行时 TEXT 映射 **不含** header，以免 RX 与改 `entry` 冲突。

---

## 4. 段内容（与现行 emit 对齐）

现行 `emit_program_*` 顺序：procs → strlit → smap 表 → 112 字节全局 → cplt/GOT → 宿主 stub。拆开如下。

### 4.1 TEXT

机器码。PLT stub 算 TEXT：`jmp [rip+got_slot]`（x86）等，目标是 DATA 里的 GOT。

函数在 TEXT 内的偏移由 export 给出。冷启动把 `runtime_funs` 编进本段一次，之后 REPL 行只 append。

### 4.2 RODATA

- **strlit 池**：与现在 `strlit_bytes` 相同；指针 reloc 为 `ABS64 = RODATA.vm + off`，tagged 时 `| 1`（现行 patch tag 11）。
- **smap**：现行 `[n]{ ret_va, nslots }`。`ret_va` 用 reloc `ABS64` 指向 TEXT（tag 12），不要在 emit 时写死 `JIT_VADDR`。
- **funstarts（可选但建议 v1 就有）**：`u32 n`；然后 `n` 个 `{ u32 text_off, u32 size, u32 name_idx }`。调试与「函数起始表」用这个，不上 DWARF。

### 4.3 DATA

固定前缀 **112 字节**，布局不变（`emit_resolve_patch` tag 3–8）：

```
+0   gc_head
+8   argc
+16  argv
+24  stack_hi
+32  gc_free
+40  alloc_bump
+48  heap_lo
+56  prof_st      ; glob which=3，不 shl
+64  gc_bm        ; glob which=4
+72  gc_hi
+80  gc_bmlo
+88  gc_bmsz
+96  arena_cur
+104 arena_end
```

其后：

1. **GOT**：`u64 nimport` 个槽（可与现在一样前面留 3 个 qword 对齐 `emit_cplt`：`got+24+i*8`）。活映像 bind 后写函数指针；dump 时必须清零。
2. **会话全局**：每个顶层 `let` 一个 `u64` 槽（tagged 值）。export 类型 `DATA`。初始化代码只在该 let 首次出现的那一行 TEXT 里跑一次。

不设独立 `.bss`：未初始化就是 DATA 里尚未写入的容量（文件 dump 时 DATA filesz 可截到最后一个非零，vmsize 仍为 `data_size`）。

### 4.4 LINK（文件必有；运行时可选）

| 块 | 内容 |
|----|------|
| import | `nimport` 个 `{ u32 name_idx, u32 flags, u32 got_off }`。`got_off` 相对 DATA。flags：`0` = `dlsym(RTLD_DEFAULT)`（现行 `cimport_jit_bind`） |
| export | `nexport` 个 `{ u32 name_idx, u8 kind, u8 seg, u16 pad, u32 off }`。kind：`1` FUNC（off 相对 TEXT），`2` DATA（off 相对 DATA）。seg 冗余可作校验 |
| rela | 见 §5 |
| strtab | `u32 size` + UTF-8 字节，NUL 分隔。`name_idx` 为串起始偏移 |

Header v1 没有这些表的内部偏移。约定 LINK 段布局：

```
+0   u32 nimport, nexport, nrela, str_off
+16  import[]
     export[]
     rela[]
     strtab
```

`str_off` 相对 LINK 起点。宿主若自己持有 hashmap，活映像可以不填 LINK，dump 时从宿主重建。

---

## 5. Reloc

每条 16 字节：

| off | 类型 | 名 |
|-----|------|----|
| 0 | u32 | offset | 相对 **所在段** 的写入位置 |
| 4 | u8 | seg | `1` TEXT `2` RODATA `3` DATA |
| 5 | u8 | type | 见下 |
| 6 | u16 | pad | 0 |
| 8 | u32 | sym | export 下标，或 `0xffffffff` 表示段内 addend-only（strlit / smap） |
| 12 | u32 | addend | 无符号 32 位够用；需要高位时 type 用 ABS64 且 addend 为段内 off |

**type**（与 `emit.yac` patch tag 对齐，便于从现有 `patches` 直接吐）：

| type | 现行 tag | 语义 |
|------|----------|------|
| 1 | 1 | x86 REL32：`*(i32*) = target - (site+4)`，target = TEXT + export.off |
| 2 | 2 | ABS32：closure fnptr 低 32 位 = TEXT + off（若仍用 32 位指针场） |
| 3 | 3–8 | ABS64：`*(u64*) = DATA.vm + addend`（addend = 0,8,16,… 对应 glob 槽） |
| 4 | 11 | ABS64：`*(u64*) = (RODATA.vm + addend) \| 1` 字符串 |
| 5 | 12 | ABS64：`*(u64*) = TEXT.vm + addend`（smap 返回地址等） |
| 6 | 15 | REL32 到 PLT stub：target = TEXT + plt_off(import) |
| 7 | 9 | arm64/riscv 尾跳 / BL / JAL（实现时按 arch 再拆 7a/7b） |
| 8 | 16–19 | PE stub REL32（仅 Win 宿主 JIT）；dump 成 PE 时改走导入表，不写死 stub VA |

加载：对每条 rela 按 **当前** `base + seg.vmoff` 回填。  
emit 不再在编译器里对 `JIT_VADDR` 调 `emit_resolve_loop`（模块模式）；AOT `yc -o` 仍可 resolve + pack，与今日相同。

活映像 append 一行：只对 **本行新字节** 的 rela 回填，旧 TEXT 不动。

---

## 6. 链接与调用

宿主会话对象（可后移进 HDR 页）：

```
jit_sess = {
  hdr_ptr, text, ro, data, link,   ; 各映射基址与 cap/size
  exports,                         ; name intern → {kind, off}
  imports,                         ; 与 GOT 平行
}
```

### 6.1 冷启动

1. 映射四段（及 HDR）。
2. 把 runtime（+ 默认 `rt.num` 若需要）emit 成 **未 resolve** 的模块字节，copy 进 TEXT/RODATA/DATA，写 LINK/宿主 export。
3. 回填 rela；`cimport`：`dlsym` → GOT。
4. 不调用 `_eval`（或调用一个只初始化 GC 全局的入口）。

### 6.2 追加（REPL 一行 / 增量模块）

1. 只 parse/ANF/LIR/emit **这一行**（未进映像的 `import` 包在此 `lir_extend`）。
2. 新 proc 的 `fcall` 对已有名生成 type 1/6 rela，不把 `print` 再编一遍。
3. `let a`：DATA 上新槽，export `a`；本行 TEXT 写初始化，下次只 load 槽。
4. `bytes_extend` 到 `text_size`；更新 funstarts / smap（RODATA append 或宿主侧重建 smap 表——v1 允许 smap 只覆盖最新代码，GC 扫栈用合并表）。
5. `hdr.entry = 新 _eval 的 TEXT off`。
6. `icall(text + entry)`，返回值按现行 `_eval` 打印约定。

禁止：`mmap` 同一 `JIT_VADDR` 覆盖；禁止把历史源码拼起来重 JIT。

### 6.3 `jit_run` 拆分

| 原语 | 作用 |
|------|------|
| `jit_map` | 冷启动映射；进程内一次 |
| `jit_load(mod)` | 拷贝模块、rela、bind 新 import、登记 export |
| `jit_call(off)` | `icall(text+off)`，不 memcpy 整映像 |
| `jit_dump(path)` | 见 §8 |

现行 `yac_jit_run(bytes, tagged_off)` 保留给测试；`--repl` 改走 sess。

---

## 7. W^X

v1 可继续整段 RWX（与现在一致），但映射已按段拆开，改权限是本地的。

v1.1：

- TEXT 新页：`PROT_READ|PROT_WRITE` 写入 → `mprotect` `PROT_READ|PROT_EXEC`。
- 只 mprotect **本行用到的页**，不要每次整段 TEXT。
- DATA/HDR/LINK：`PROT_READ|PROT_WRITE`，永不 EXEC。
- RODATA：写完 `PROT_READ`；append 时短暂 RW。

Windows：`VirtualProtect` 对应；或 RW 视图 + RX 视图同一物理页（后做）。

---

## 8. Dump

### 8.1 `.yjit`

按文件顺序写：

```
[64B hdr][TEXT][RODATA][DATA filesz][LINK]
```

`hdr.base` 可写 0（可重定位）或写当前 `JIT_VADDR`。GOT 全 0，`flags.bit0 = 0`。rela 必须完整，否则只能在原基址 mmap。

### 8.2 ELF / PE / Mach-O

不把 `.yjit` 当 ELF 解析。流程：

1. 取出 TEXT（及需要的 RODATA 常量）。
2. DATA 取 **模板**（GC 全局清零 + GOT 清零），不要把会话 tagged 指针写进可执行文件（除非做堆快照）。
3. `cimport_names` 从 import 表恢复。
4. 按目标 `mk_target` 的 `vaddr + text_off` 对 rela 回填（或交给现有 emit resolve 等价物）。
5. `backend_pack(t, code)` —— 与 `yc -o` 同一 pack。

进程入口：生成或选择 `_start`（栈 argc/argv、退出 syscall），不要用 REPL 的 `_eval`。需要「从某函数开始跑」时，`_start` 里 `fcall` 该 export。

Mach-O 同理走 `pack_macho_*`。

---

## 9. 与 AOT 的关系

| | AOT `yc -o` | JIT sess |
|--|-------------|----------|
| emit | 可仍 resolve + pack（今日路径） | 吐未 resolve 模块 + rela |
| 容器 | ELF/PE/Mach-O | 本格式（内存 + 可选 `.yjit`） |
| 入口 | `_start` | `_eval` 每行更新 |
| 全局 | 映像 DATA | 同 DATA，跨行存活 |

不必强迫 AOT 改用 `.yjit`；两路径共享「段角色 + reloc 类型号」。

---

## 10. 实现顺序

代码落点（预计）：

- `src-self/back/pack/yjit.yac`（或 `jitimg.yac`）：hdr 读写、rela 应用、`.yjit` 写出
- `src-self/rt/runtime.yac`：`jit_map` / `jit_load` / `jit_call`；`rt_jit_run_ins` 改为走 sess 或旁路
- `src-self/back/emit/emit.yac`：模块模式下跳过 `emit_resolve_loop`，把 `patches` 编成 rela
- `src-self/back/backend.yac`：`compile_jit` / REPL 用 sess.append；去掉源码累加
- `src-self/yc.yac`：`repl_loop` 每行只编译该行

建议里程碑：

1. **map-once + call**（已做）：16MiB 一次 mmap；`.yjit` 扁平 TEXT dump。仍整份 emit+resolve+memcpy。
2. **未 resolve + rela 加载**：一份模块 load 进固定基址，行为与今日 `jit_run` 相同。
3. **append**（已做雏形）：REPL 第二行起 skip 已 emit 的 proc，把新 blob 拷到 `text_end`，`globbase` 沿用首份映像。
4. **REPL**（已做雏形）：不再拼接源码；顶层 `let` 进 `JIT_LET_BASE`（映射内 12MiB 处）；imap/malias 跨行保留。
5. **dump `.yjit`**，再接 pack ELF/PE。
6. W^X、funstarts、RODATA 只读。

---

## 11. v2（明确不做）

- Mach-O 式 Segment Table + Section Table 通用数组
- `.bss` 独立节、DWARF / compact unwind
- 客堆并入 DATA 的进程快照
- 把活映射做成可被 `readelf` 解析的 ELF
- 每行 `mprotect` 整段 TEXT

需要第三方加载器或多节类型时，在 64 字节 header 后加 `sectab_off`（version=2），v1 加载器忽略。

---

## 12. 现行行为对照（改之前）

```
emit_program(prog, JIT_VADDR, text_off=0)
  → 绝对地址已写进机器码
cimport_jit_bind(code)          → dlsym 填 GOT
mmap(FIXED, 0x200000000, RWX)
memcpy(整份)
icall(_eval)
下一行：再 mmap 同一地址 → 旧代码与 DATA 消失
REPL：str_cat(acc, line) 重编全部源码
```

目标：一次 map，append 机器码，符号在 DATA/export，历史副作用不重跑。
