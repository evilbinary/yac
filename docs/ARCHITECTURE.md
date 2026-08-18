# 架构与开发

## 目录结构

```
src/
  main.c          CLI 驱动
  lexer.c/h       词法
  parser.c/h      语法 → AST
  ast.c/h         AST 节点与 dump
  anf.c/h         AST → ANF 归一化（原子子表达式直接使用，减少冗余 let）
  cps.c/h         ANF → CPS，cps_simplify（--opt 常量折叠/eta 归约）
  uncps.c/h       CPS → ANF（受限，可失败）
  value.c/h       Value/Closure/Prim 与原语实现
  env.c/h         环境
  eval_anf.c/h    ANF 解释器（帧栈 + 蹦床，拒绝 callcc）
  eval_cps.c/h    CPS 解释器（纯蹦床，支持 callcc/throw）
  gc.c/h          mark-sweep GC（运行时对象 + 值根栈）
  gcobj.h         GC 对象头
  arena.c/h       区域分配（IR 与字符串字面量）
tests/
  run_tests.sh    回归测试
  prop.sh         属性测试：随机程序比对 ANF/CPS/--opt（make prop）
tools/
  genyac.c        随机 Yac 程序生成器（确定性、可终止）
docs/
  DESIGN.md       语言与 IR 的完整设计
```

## 管线

```
源程序 → lexer → parser → AST → ANF → CPS
                              │        │
                              ▼        ▼
                          eval_anf  eval_cps
                              └──┬───┘
                          （--both 比对）
```

每层都可以独立 dump（`--dump-anf` / `--dump-cps` / `--dump-uncps`）。

## CLI 完整参考

```
usage: yac [options] file.yac
  --cps            run via the CPS interpreter
  --dump-anf       print the ANF and exit
  --dump-cps       print ANF->CPS and exit
  --both           run both interpreters and compare
  --uncps          run ANF->CPS->ANF (un-CPS round trip) and execute
  --dump-uncps     print the un-CPS'd ANF and exit
  --opt            simplify the CPS IR (constant folding, eta reduction)
  --ast            dump the parsed AST and exit
  --no-gc          disable garbage collection (arena-style growth)
  --limit-nodes N  abort when live objects exceed N (0 = unlimited)
```

调试钩子（环境变量）：

- `YAC_GC_THRESHOLD=N` — 覆盖 GC 触发阈值（字节），用于压测 GC。
- `YAC_GC_DBG=1` — 每次回收打印存活对象数。

## 测试

### 回归测试（`make test`，29 项）

- 普通程序在 ANF 与 CPS 下结果一致（golden + parity）
- 10⁷ 次尾调用不爆栈（ANF / CPS / un-CPS 各一遍）
- `callcc` 四例：逃逸、递归提前退出、捕获后使用、函数内逃逸
- un-CPS 往返一致；callcc 被 un-CPS 正确拒绝
- GC：循环分配垃圾时内存有界；`--no-gc` 触发 `--limit-nodes` 保护

### 属性测试（`make prop`，默认 300 随机 + 60 callcc，可 `prop.sh N M`）

- `tools/genyac.c [--callcc] [seed]` 生成确定性、可终止的随机程序
- 普通程序在 ANF、CPS、`--opt --cps` 下 stdout / stderr / 退出码完全一致
- callcc 程序只在 CPS 机器下断言正常终止（rc ≤ 1）

## 性能基线

10⁷ 次尾递归（`tests/tco.yac`）：

| 模式 | 耗时 |
|---|---|
| ANF | 13.0s |
| CPS | 19.3s |
| CPS `--opt` | 19.0s |
| un-CPS | 13.7s |

## 里程碑

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M1 | lexer、parser、AST→ANF、ANF 解释器 | ✅ |
| M2 | ANF→CPS、CPS 解释器、`callcc`/`throw` | ✅ |
| M3 | mark-sweep GC、un-CPS、`--dump-*` | ✅ |
| M4 | 属性测试、CPS 化简（`--opt`）、文档 | ✅ |
| M4 | 扁平环境快照（性能优化） | ⏳ |
