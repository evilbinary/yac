# GC：堆结构、根集合与回收机制

本文描述 yac 的两套垃圾回收器，重点在**原生镜像**（AOT 与 JIT blob）里
`src-self/rt/runtime.yac` 实现的那一套；最后附 C 解释器（`src/gc.c`）的差异。

> 结论先行：两套都是**非移动（non-moving）的标记-清除（mark-sweep）**，
> 单线程、无写屏障、无分代、无弱引用/终结器。原生 GC 只在 `yac_alloc`
> 里触发（以及显式 `gc_collect()`）。地址稳定是硬约束：镜像里烘焙了
> `gvar` 单元、跨镜像 patch 单元、JIT 代码指针，对象一旦移动这些都会失效。

---

## 1. 值表示与 kind

原生镜像里一个值就是一个 64 位字（tagged）：

| 值 | 位模式 | 说明 |
|---|---|---|
| int `n` | `n << 1` | 偶数；`0` 同时是 `false`/`unit` |
| `true` | `2` | 也满足“偶数 = 立即数” |
| `nil` | `1` | 唯一的奇数立即数；列表空表 `/l/` 就是它 |
| 堆指针 | `addr \| 1` | `addr` 8 字节对齐，所以指针 `& 7 == 1` |
| f64 | 堆对象 | kind 7，IEEE 位模式在对象里（不会出现在立即数里） |

所以 GC 判断“可能是堆指针”的快速条件就是 `(v & 7) == 1`（保守扫描用的
也是这一条）。注意偶数覆盖 int/`false`/`unit`，而 `nil` 是唯一的奇数非指针，
必须先单独排除。

`kind(x)` 原语（不是 GC 内部读的那个字段）语义：

1. 偶数 → `0`（int，含 false/unit）；
2. `x == 1` → `1`（nil）；
3. 否则读原始对象 `[p + 16]`；若等于 2/3/4/5/7 就原样返回；
4. 其余一律返回 `6`。

第 4 条很关键：**闭包没有 kind 字段**，`[p+16]` 是烘焙进去的函数入口地址，
`kind` 原语把这类对象（以及显式 kind=6 的续延）都报告成 6。GC 内部不看
`kind()` 原语，而是直接读 `[p+16]` 做分派，所以闭包会落到默认分支
（按闭包布局扫描）——详见 §4.3。

---

## 2. 全局页 G 与控制页

`$gbase` 取到全局页基址 G。G 是一块镜像数据区（大小由 emit 决定），前
112 字节是 GC 状态；`G+32` 指向另一块 4KiB 的**控制页**（`yac_ctrl` 惰性
`mmap`，不是 GC 对象）。

### 2.1 G 页布局（qword，x86_64/arm64/riscv64 相同）

| 偏移 | 内容 | GC 根？ |
|---|---|---|
| `+0` | `gc_all` 链表头（所有活着的 / 未回收的对象 body） | 遍历对象图 |
| `+8` | `argc` | 否 |
| `+16` | `argv`（指向 OS 栈上的指针数组） | 否 |
| `+24` | `stack_hi`：`_start` 进入时的 rbp（栈的顶端） | 栈扫描上界 |
| `+32` | 控制页指针（见 §2.2） | 否 |
| `+40` | 自上次 GC 以来累计分配字节（raw，触发阈值比较用） | 否 |
| `+48` | 最小 arena 地址（`yac_amap` 维护；历史遗留，位图已不用它） | 否 |
| `+56` | 标记工作栈指针（mmap 块；`+0` cap、`+8` top、`+16` 起是槽） | 否 |
| `+64` | 对象起始位图基址（mmap，可跨轮复用） | 否 |
| `+72` | 本轮位图覆盖的地址上界（`heap_max`） | 否 |
| `+80` | 本轮位图覆盖的地址下界（`heap_min`） | 否 |
| `+88` | 位图 mmap 的字节数（复用判断用） | 否 |
| `+96` | temp bump arena 的当前游标 | 否 |
| `+104` | temp bump arena 的结束地址 | 否 |
| `+112` | JIT 会话映射基址（0 = 纯 AOT，无会话）；见 §5.3 | 否 |
| `+120` | 剖析器单元格列表（`glob 3`） | **是** |
| `+128` | 剖析器 enter/leave 忙标志（`glob 4`） | 否 |
| `+136..+215` | 10 个宿主函数槽（`@name` 绑定） | 否 |
| `+216..+439` | 28 个 cimport 地址表项 | 否 |
| `+440` | 顶层值表 `[ [name, value] ... ]`（cons 链） | **是** |
| `+448` | 顶层函数表 `[ [name, entry] ... ]` | **是** |
| `+456...` | 外部符号槽，之后是跨镜像 patch 单元 | 否 |

> 不要往 `G+48/56/64` 放普通全局变量（`src-self/back/profile.yac` 顶部注释
> 也重申过这一点）。

### 2.2 控制页（`G+32` → ctrl）

| 偏移 | 内容 |
|---|---|
| `+0..+80` | 11 个尺寸类 bin 头：64, 128, ..., 65536（`+0 + 8*log2(size/64)`） |
| `+88` | 48 字节类 bin 头（cons、box、最小闭包等） |
| `+96` | temp arena 链表头 |
| `+104` | perm bump 游标 |
| `+112` | perm bump 结束 |
| `+120` | perm arena 链表头 |
| `+128` | perm 模式标志（`intern` 期间为 1） |
| `+136` | 当前 temp bump arena |
| `+144` | 当前 perm bump arena |
| `+152` | 下一次 GC 的字节阈值（0 = 还没收集过） |
| `+160` | 待清理（flags=2）的 arena 链表头 |

临时区的 `cur/end` 在 **G+96/G+104**（不是控制页），由 `yac_alloc` 直接读写。

---

## 3. 对象、arena 与分配器

### 3.1 对象头与 arena

每个从 `yac_alloc` 返回的 body 前面固定有 16 字节头：

```text
        ┌──────────── 16 字节对象头 ────────────┐
body-16 │ arena 指针（指向所属 arena）           │
body-8  │ size：body 的“尺寸类”字节数           │
body+0  │ next：gc_all 链表的下一个对象          │
body+8  │ mark：标记位（0/1）                    │
body+16 │ kind / 闭包入口 / 续延标记             │
body+24 │ 负载……                                 │
        └────────────────────────────────────────┘
```

arena 自身也是 32 字节头 + 对象区：

| 偏移 | 内容 |
|---|---|
| `+0` | next（temp/perm arena 链表） |
| `+8` | mmap 字节数 |
| `+16` | live：本 arena 当前活对象数（GC 看到的 + 从 bin 复用的） |
| `+24` | flags：0=temp，1=perm，2=temp 已死（`+16` 被复用为 dead 链指针） |
| `+32` | 第一个对象头（`arena+32` 起：`[arena][size][body]`…） |

`yac_alloc(n)` 返回 **body**；body 地址因此总是 8 字节对齐（64 位）。

### 3.2 尺寸类与 bin

`yac_aclass(n)` 把请求规整到：

- `n < 48` → `48`（最小 body 要能放下 cons 的 `cdr@32`，所以不能是 32）；
- `48 < n <= 64KiB` → 不小于 `n` 的 2 的幂（64, 128, ..., 65536）；
- `n > 64KiB` → 原样（大对象，单独 mmap）。

分配路径（`rt_alloc_ins`）：

1. 请求先按 `max(n,48)` 计入 `G+40`；若 `G+40 >= threshold`，先 `yac_gc`，

   然后 `G+40 = 0`。
2. `size = aclass(n)`，实际从 arena 切出 `align8(size + 16)`（对象头 16）。
3. **小对象**（`size < 64KiB`）先查对应 bin；非空则摘下节点、

   `arena.live++`、`memset(body, 0, size)` 后复用。
4. bin 空则从 temp bump arena 切；空间不够就 `yac_amap` 一块新的 2MiB

   arena 并链接到 ctrl+96。
5. **大对象**（`size >= 64KiB`）总是 `mmap` 一块

   `round4KiB(size + 48)` 的专用 arena，里面只有一个对象（live=1）。
6. perm 模式（`yac_perm_on/off`，只有 `intern` 使用）只用 perm bump，

   不走 bin，切出的对象**永远不会被清除**。
7. 最后把 body 链到 `G+0` 的 `gc_all` 头，`mark=0`，返回 body。

注意 `G+40` 统计的是**请求字节**（至少 48，未按尺寸类取整，也不含 16 字节
对象头和 arena 头）；而 GC 后算阈值的 `live` 是各对象 `body-8` 的**尺寸类**
之和。两者口径略有差异，所以 RSS 会明显高于日志里的数字。

### 3.3 触发条件与自适应阈值

`yac_alloc` 里的判断：

```text
threshold = ctrl ? ctrl[152] : 0
if threshold == 0: threshold = 256MiB + 1        # 第一次收集之前
if G[40] >= threshold: yac_gc(); G[40] = 0
```

收集结束时（`rt_gc_collect_ins` 尾部）：

```text
ctrl[152] = max(live_bytes, 1GiB + 1)
```

即：首轮门槛 256MiB，之后至少 1GiB。`threshold` 是“自上次收集以来新分配
的字节数”，不是堆总量；堆会稳定在 `live + 已分配垃圾 <= 2*live` 附近
（大 `live` 时），或用 1GiB 下限换取更少的标记次数。改这个值就是直接改
时间/内存曲线，`runtime.yac` 的注释里记录了 256MiB/512MiB/1GiB 的实测。

显式入口：

- `gc_collect()` / `yac_gc()`：无条件收集（`gc_collect` 是 LIR 内建原语，
`yac_gc` 是内核过程，两者都进 `rt_gc_collect_ins`）；
- 编译器在阶段边界主动调用（见 §7）。

---

## 4. 标记（yac_gc_mark）

`yac_gc_mark(v)` 是导出的内核过程：把**已 tagged** 的 `v` 压入工作栈，
然后迭代地做深度优先标记。它不递归，因此单行 96KB、几千层嵌套的 JS AST
也不会爆 C 栈。

### 4.1 工作栈

| 位置 | 内容 |
|---|---|
| `G+56` | 工作栈块指针（首次使用时 `mmap` 2MiB+16，惰性初始化） |
| 块 `+0` | 容量（字节） |
| 块 `+8` | 当前 top（字节） |
| 块 `+16` 起 | 8 字节槽，存放 tagged 值 |

容量不够时 `mmap` 一块两倍大的、`memcpy`、`munmap` 旧块。工作栈不随 GC
释放，跨轮复用。

### 4.2 单个候选值的校验

从栈里弹出一个 `v` 后：

1. `v & 1 == 0` → 立即数（int/false/unit），跳过；
2. `v == 1` → nil，跳过；
3. `p = v & ~1`；若 `p & 7 != 0` → 不是对齐指针，跳过；
4. 优先用**对象起始位图**判断 `p` 是否正好是某个对象的 body 起点：

   要求 `heap_min <= p < heap_max` 且对应 bit 为 1；否则跳过。
    位图不存在时（分配失败）退回线性扫描 `gc_all` 查找 `p`；
5. 若 `p->mark != 0` → 已标记，跳过；
6. 置 `p->mark = 1`，按 kind 压入子对象。

位图是**“对象起点图”**，不是标记图（标记仍在对象 `+8`）。它每轮重建：
`gc_collect` 先遍历 `gc_all` 求 body 的 min/max 并清 mark，再按
`1 bit / 8 字节` 建位图、把所有对象起点位置 1。这样内部指针 / 指到对象
中间的伪指针会被可靠拒绝。位图 mmap 按 `G+88` 记录的容量复用，不够才重建。

### 4.3 kind 分派与子对象

GC 直接读 `p+16`（不是 `kind()` 原语）：

| `[p+16]` | 类型 | 扫描的字段 |
|---|---|---|
| 2 | cons / box | `car@24`、`cdr@32`（两者都必须压栈；box 的 cdr 是 nil） |
| 3 | STR | 叶子 |
| 4 | BYTES | `dataptr@40`（指向一个 kind=3 的 STR blob） |
| 5 | listbuf | `vec@64`（kind=3 blob），再扫描 `len@24` 个元素（元素值在 `vec+32` 起） |
| 6 | 续延（mkcont） | 叶子（只存 fp/sp/恢复标签，不持有堆指针） |
| 7 | FLOAT | 叶子（IEEE 位模式） |
| 其它 | **闭包** | `nenv@24` 个捕获槽：`[p+32 .. p+32+8*nenv)` |

闭包没有 kind 字段，`[p+16]` 是烘焙的函数入口地址；它不等于 2/3/4/5/6/7，
于是落到默认分支。这也是为什么 `kind(闭包) == 6`（原语把未知种类映射成 6），
但 GC 不会把闭包当 kind 6 的叶子——两处判断依据不同，改代码时不要合并。

### 4.4 listbuf 的标记（2026-11 改动）

listbuf 的 `vec` 是独立分配的 kind=3 blob：

```text
listbuf(body):
  +0 next      +8 mark      +16 kind=5
  +24 len（raw）            +32 head（旧 cons 链，已不用）
  +40 tail（同上，已不用）   +48 cursor i（已不用）
  +56 cell（已不用）
  +64 vec（指向 STR blob）   +72 cap（raw）
vec(STR blob):
  +0 next +8 mark +16 kind=3
  +24 len（未使用）
  +32 起：len 个 tagged 元素值
```

标记流程：

1. `vec == nil` 直接返回（空缓冲）；
2. 先把 `vec` 本身压栈（保证 blob 被标记、不会被清除）；
3. 令 `count = listbuf.len@24`、`base = vec + 32`，复用闭包字段扫描循环

   （`mk_cl`）把每个元素压栈。

**改 listbuf 表示时必须与 GC 同步**：旧实现 `list_push` 会为每个元素额外
分配一个 cons cell 并把 cell 指针存进 `vec`，GC 标记 `head@32` 的 cons 链；
新实现 `vec` 直接存元素值，GC 必须扫描 `vec` 的元素。两者改一半会出现
“元素没被标记 → 被提前回收 → 随机崩溃”。

当前 list 原语对 listbuf 的支持面：

| 原语 | listbuf 行为 |
|---|---|
| `len` | 读 `len@24` |
| `nth` | 直接 `vec[i]`（越界/已清除为 0） |
| `list_set` | 直接写 `vec[i]`（编译器用它把已处理的 AST/token 置 0 释放引用） |
| `list_push` | 追加元素值、必要时扩容 |
| `list_rev` | 正向读 `vec`，建成反转的 cons 链返回 |
| `print` | 按 `0..len-1` 打印 |
| `drop` | 从 `i` 起重建 cons 链（历史行为） |
| `tail` / `append` / `foldl` / `map` | **不支持 listbuf**（改造前也不支持；不要依赖） |

---

## 5. 根集合（yac_gc_collect 的前半段）

`rt_gc_collect_ins` 顺序：清 mark/求 min-max → 建/复用位图 → 标记根 →
（`yac_gc_mark` 内部迭代）→ 清除 → 选新阈值。

根分为五组。

### 5.1 全局根

- `G+120` 剖析器单元格列表；
- `G+440` 顶层值表；
- `G+448` 顶层函数入口表。

三者都是普通堆对象（cons 链或 listbuf），值本身若为堆指针（`&7 == 1`）
就交给 `yac_gc_mark`；G+440 里的 `[name, value]` 对会把 name（intern 的
perm 字符串）和 value 一起保活。

### 5.2 帧根：rbp 链 + 栈映射（smap）

原生代码的每个普通过程帧：

```text
[rbp+8]   返回地址（进入调用者的代码）
[rbp+0]   保存的调用者 rbp
[rbp-8]   slot 0（对象 ABI 下保存 r13 = self）
[rbp-16]  slot 1
...
[rbp-8*(k+1)] slot k
```

编译期，发射器在**每条调用指令**处记录一条映射：
`[返回地址的绝对地址, 调用者的局部槽数]`，写进镜像里的 smap 表
（`smap_note` / `smap_emit_table`，链接期用 patch tag 12 烘焙成
`codebase + offset`）。`$local` 帧和 raw 过程记录 `nslots = 0`。

收集时从当前 `rbp` 开始：

1. `frame = [current_fp]`（跳过 GC 自己的帧，从调用者的帧开始）；
2. 当 `frame != 0` 且 `frame < stack_hi`：
   - `ret = [frame + 8]`，在 smap 表里线性查找 `ret`；
   - 命中记录 `nslots` 后，**这个帧的调用者**的 rbp 是 `[frame]`，
   从 `[caller_rbp - 8]` 起标记 `nslots` 个 qword；
   - `frame = [frame]`，继续向上。
3. `stack_hi` 为 0 时（JIT 镜像不跑 `_start`），先沿 rbp 链找到链顶并

   写回 `G+24`，再开始扫描。

槽里的值只有 `&7 == 1` 才标记（槽可能存 int/nil/未初始化）。

### 5.3 JIT 会话槽根

REPL/JIT 会话把每行的顶层 `let` 值放在固定映射区：

- 映射基址 `JIT_VADDR = 2^33`，16MiB RWX；
- **槽区基址 = JIT_VADDR + 12MiB = 8602517504**；
- `yac_jslot_set(id, v)` 写槽，并在 `基址 - 8 = 8602517496` 维护
高水位结束地址（只增不减，重定义旧槽也不会缩小范围）；
- `G+112` 在第一次 `yac_jit_run` 时写入映射基址；纯 AOT 进程为 0，跳过。

GC 在 `G+112 != 0` 时扫描 `[槽区基址, 高水位)` 的每个 8 字节槽，值满足
`&7 == 1` 就标记。没有这组根，REPL 里 `let ff = compile(...)` 的 `ff`
会在下一次编译的 GC 中被回收。

### 5.4 保守栈扫描

做完精确的帧/smap 走查后，GC **总是**再从当前 `$sp` 到 `stack_hi` 保守扫描
整个栈区（含 GC 自己、`$proc`/`$local` 帧、被调函数的临时帧）：

- 逐 8 字节读；
- 仅当 `(v & 7) == 1` 时尝试标记（8 字节对齐的 tagged 指针）；
- 与位图校验配合，普通整数/浮点立即数不会被误认。

保守扫描是“不漏根”的兜底，代价是栈上的陈旧值可能让垃圾多活一轮。

### 5.5 寄存器不是根

GC 只认**内存里的根**。LIR 的求值模型保证：任何可能触发分配的调用点
（`fcall`/`yac_alloc` 等）之前，活值都已经落在帧槽里（槽号就是寄存器分配
的结果），所以寄存器不需要扫描；被调用者保存寄存器由帧槽/栈扫描覆盖。

---

## 6. 清除（sweep）与资源回收

`sweep` 遍历 `gc_all`，维护 `prev`：

- `[p+8] != 0`（活）：`live += [p-8]`（尺寸类字节），保留；
- `[p+8] == 0`（死）：调用 `yac_free_dead(p)`，再从链表摘除。

`yac_free_dead(p)`：

1. 读 `arena = [p-16]`；为 0 直接返回（防御）；
2. `arena.live--`；
3. `arena.flags == 1`（perm）→ 返回（perm 对象永不释放）；
4. `flags == 0`（temp）且 `live == 0`：
   - 置 `flags = 2`，把 arena 链到 `ctrl+160`（dead 链），等收集末尾统一
    `munmap`；`arena+16` 被复用为 dead 链指针；
5. 否则（arena 仍活）：
   - 若 `size = [p-8] < 64KiB`：把 body 压回对应尺寸类 bin（复用即
    `arena.live++`）；
   - 若 `>= 64KiB`：大对象必然独占 arena（live 已到 0），不进 bin。

收集末尾调用 `yac_purge_dead`：

1. 扫 12 个 bin，摘掉所有 arena `flags == 2` 的节点（这些节点只可能在本轮

   清除中产生）；
2. 遍历 `ctrl+160` 的 dead arena：从 temp arena 链表摘除、若它是当前

   bump arena 就清空 `G+96/G+104`、然后 `munmap(arena, arena.mmap_size)`。

**语义要点**：

- 对象地址永不移动（非移动 GC）；
- 死的小对象先进 freelist，不立刻还给 OS；
- 一个 arena 只要还有活对象，它里面的空闲槽就留在 bin 里；整个 arena
死光才 `munmap`；
- 大对象独立 arena，死亡即整块释放；
- perm（intern）对象不回收；
- 不清除活对象的 mark（下一轮 `gc_collect` 开头统一清零）。

---

## 7. 编译器如何配合 GC

原生 GC 没有“槽活跃性分析”，一个值只要还留在任何帧槽里就是根。`backend.yac`
因此把每个阶段放进**独立帧**，并在阶段边界显式 `gc_collect()`：

```text
ast_ready:  lex -> parse -> (dump) -> ...
anf_of:     parse 结果 -> ANF，结束后丢弃
lir_of:     gc_collect() 后再跑 LIR
lower_of:   gc_collect() 后再跑 lower
pack_of:    gc_collect() 后再跑 pack
compile_env_of: 最后再 gc_collect()
```

另外两处手动“释放引用”：

- `parser.yac` 的 `clear_toks`：顶层 form 处理完就 `list_set(toks, k, 0)`；
- `lir.yac` 的 `lir_all`：一个 ANF item 降低完就 `list_set(items, i, 0)`。

`list_set` 只是把槽写成 0；对 listbuf 来说 0 是“tagged int 0”，保守扫描
不会把它当指针，因此引用确实断了（旧 cons 表示下 cell 仍挂在 head 链上，
不会释放——这是 2026-11 连续化改造的直接动机之一）。

`--dump-obj` / `dump_now` 会在 `gc_collect()` 之后打印存活堆构成
（`yac_heap_stats`）：per-kind 数量 + 字节数，字节是 `body-8` 的尺寸类，
不是 RSS。

---

## 8. 观测与调试

| 手段 | 作用 |
|---|---|
| `gc_collect()` / `yac_gc()` | 立即触发一轮收集 |
| `yac_heap_stats()` / `--dump-obj` | 收集后按 kind 打印存活对象数与字节数 |
| `--dump-obj` + `dump_now(tag)` | 编译器在 lex/parse/anf/lir/lower/pack 后各打一次 |
| `YAC_GC_THRESHOLD` | **仅 C 解释器**：初始/最小阈值（默认 8MiB） |
| `YAC_GC_DBG=1` | **仅 C 解释器**：每轮打印 live/objs/roots/next |
| `--limit-nodes N` | **仅 C 解释器**：活对象数超限直接报错退出 |
| `time_ms()` / `prof` 日志 | 配合 `--dump-obj` 观察 GC 对阶段耗时的影响 |

注意原生镜像的阈值是运行期自适应（§3.3），没有环境变量入口；要改只能改
`rt_alloc_ins` / `rt_gc_collect_ins` 里的常量。

---

## 9. C 解释器 GC（`src/gc.c`）

`./yac` 直接解释/求值时用另一套 mark-sweep：

- 对象头 `GObj`：`next`、`size`、`kind:4`、`mark:1`，`malloc` 分配、
16 字节对齐；种类 `G_STR / G_ENVFRAME / G_CLO / G_FRAME / G_VALARR / G_LIST / G_BIGNUM / G_BYTES`，与原生 kind 不是同一套编号；
- 子对象：
  - `G_ENVFRAME`：`parent` + 槽数组（`Value[]`）；
  - `G_CLO`：`frame`；
  - `G_FRAME`（ANF 续延）：`prev`、`env`；
  - `G_VALARR`：`n` 个 `Value`；
  - `G_LIST`：`items[]`（独立 malloc 块）；
  - `G_BYTES`：`data`（独立 malloc 块）；
  - `G_STR`：字面量在 arena，不回收；`G_BIGNUM` 无子指针；
- 根：`g->envroot`（当前环境帧）、`g->frameroot`（ANF 机器帧链）、
`g->roots[]` 显式根栈（`gc_push_root` / `gc_push_value`，按值类型解出
GC 指针）；
- 分配：`gc_alloc_raw` 先判断 `allocated > threshold` 再收，然后 `malloc`，
链入 `all`；`allocated` 累加对齐后的总大小；类型化分配器在新槽上做
`memset`，避免“边填边 GC”时读到未初始化指针；
- 标记：递归 `mark_obj`，用对象头 `mark` 去重；
- 清除：重建 `all` 链，只保留 marked，`free` 未标记对象（以及
`List.items` / `Bytes.data` 这些独立块），统计 live/live_objs/total_objs；
- 阈值：`next = max(min_threshold, 2*live)`；若本轮 live 相对上一轮
`live*4 >= old_live*3`（几乎没降），改用 `4*live`，避免稳定大堆每 MB
重标；`min_threshold` 来自 `YAC_GC_THRESHOLD`（默认 8MiB）；
- 关闭：`gc_free` 释放全部对象与根栈。

C 解释器不做保守栈扫描，靠调用点显式 `gc_push_value`/`gc_push_root` 保根；
漏推根就是悬垂指针。原生镜像的“帧+smap+保守扫描”是后来为自举编译器加的，
两者不要混用。

---

## 10. 已知限制与改 GC 的检查清单

限制：

- 非移动、无压缩 → 长期运行会碎片化（靠尺寸类 bin 缓解）；
- 无写屏障 / 无分代 → 每次都是全堆标记；大堆上的停顿与 live 成正比；
- 保守栈扫描会保留少量已死对象（陈旧槽值）；
- `$proc`/`$local` 帧没有精确槽映射，只能靠保守扫描；
- perm 对象与 dead arena 的 bin 节点会常驻到 arena 死亡/进程结束；
- `G+40` 与 `live` 统计的是 body 字节，不含 16 字节对象头和 arena 头，
所以 RSS 会明显高于日志里的数字。

新增/修改一种堆对象时，至少同步这些地方：

1. `runtime.yac` 顶部的布局注释；
2. `rt_gc_mark_ins` 的 kind 分派（子对象在哪里、是否有子指针）；
3. 该对象的分配器（尺寸、字段初始化、`gc_all` 链接由 `yac_alloc` 统一做）；
4. 任何直接遍历该对象的原语（`len/nth/list_set/list_push/print/...`）；
5. `--dump-obj` 的 kind 名称表（`rt_heap_stats_ins`）；
6. `docs/LIR.md` 的 `kind` 语义与 `docs/FLAT_ABI.md` 的 G 区偏移说明

   （若动了 G/ctrl 布局）。

改 **listbuf** 时额外记住：`vec` 是独立对象，GC 必须先标记 `vec` 本身，
再按 `len@24` 扫描 `vec+32` 起的元素；`vec` 的容量槽（`+72`）不参与扫描，
`len` 之外的槽可以保留垃圾值（不会被标记，也不应被读取）。
