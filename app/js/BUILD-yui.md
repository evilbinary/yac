# 构建 yui 桥（libyui_yac.so）与 js.exe

`domtest.js` / `browser.yac` 走 `pkg/gui/yui.yac` 的桥，运行时 `dlopen app/native/libyui_yac.so`。
下面这套命令在本机（MSYS2 mingw64，无 libasan）实测可用。

## 一次性准备

```bash
export PATH=/mingw64/bin:$PATH          # pkg-config / gcc / ya 不在默认 PATH
```

## 1. 重建 yui 静态库（改了 yui 的 src/*.h 就必须做）

```bash
cd /e/workspace/yui
CPATH="E:/soft/msys2/mingw64/include;E:/soft/msys2/mingw64/include/freetype2;E:/soft/msys2/mingw64/include/SDL2" \
  ya -b yui
```

`ya` 不会自己加 freetype 头路径（`ft2build.h`），靠 `CPATH` 补。

## 2. 重建 .so

```bash
cd /e/workspace/yac
make -C app so YUI_ROOT=/e/workspace/yui ASAN=
```

- `YUI_ROOT` 必须显式给：Makefile 里默认的 `../yui` 是相对 `app/`，会算成 `yac/yui`（不存在）。
- `ASAN=` 必须给：本机 mingw64 没有 `libasan`，链接会报 `cannot find -lasan`。
- 链接已加 `-Wl,--export-all-symbols` 和 `-lfreetype`，见 `app/Makefile`。

## 3. 重建 js.exe（改了 pkg/*.yac 时必须做）

```bash
rm -f app/js/js.exe && make js        # 不 rm 会复用到旧的嵌入包
```

## 4. 验证

```bash
app/js/js.exe --compile app/js/domtest.js -o build/domtest
PATH="/mingw64/bin:$PATH" ./build/domtest; echo "rc=$?"     # 期望 rc=0
```

## 两个坑（都踩过，症状很像"崩在 yui 初始化"）

1. **`.so` 只导出 cJSON 符号**：`libcjson.a` 里的 `CJSON_PUBLIC` 带 `__declspec(dllexport)`，
   一个显式 dllexport 就会关掉 ld 默认的整体导出 → `GetProcAddress("yui_yac_init")` 返回 0 →
   `ccall(0, …)` → `rip=0` 的 SIGSEGV，且**一行输出都没有**。`app/Makefile` 里已加
   `-Wl,--export-all-symbols` 解决；`pkg/gui/yui.yac` 也加了缺符号保护（打印 `yui: symbol missing ...`）。

2. **`.so` 与 `libyui.a` 的 `Layer` 结构体错位**：`ytype.h` 变过（如 09-21 的 focus 提交给 Layer 加了
   3 个字段，`sizeof(Layer)` 1256 → 1264），而 `libyui.a` 还是旧的，于是 `yui_boot.c`（现编，新布局）
   写进去、`.a`（旧布局）读出来 → `root->sub` 变成野指针（`font = 0xbaadf00d`）→ `load_all_fonts` 崩。
   判断方法：看 `.a` 里 `parse_layer_from_json` 的 `malloc` 尺寸是否为 `sizeof(Layer)`。
   **结论：改 `yui/src/ytype.h` 后必须重跑第 1 步。**
