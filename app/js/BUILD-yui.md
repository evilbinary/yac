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

## 字体/资源路径在哪配

`app/js/yac-app.json`（yui 只认这两个键，缺省值是 `app/assets` + `Roboto-Regular.ttf`）：

```json
"assets": "../yui/app/assets",      // 资源根（字体、图片都从这下面找）
"font":   "Roboto-Regular.ttf",     // 相对 assets 的文件名
"style":  { "fontSize": 16 }        // 字号，默认 16
```

实际打开的是 `<assets>/<font>` = `../yui/app/assets/Roboto-Regular.ttf`（cwd 为仓库根）。
`YUI_ASSETS=<dir>` 环境变量优先级更高，可用于临时换资源目录，不必改 JSON。

判定字体是否真的加载成功（yui 的 `printf` 重定向到文件会丢，不能只看日志）：

```bash
PATH=/mingw64/bin:$PATH gdb -batch -ex 'set breakpoint pending on' \
  -ex 'b backend_apply_font_options' -ex run -ex 'bt 3' --args ./build/domtest
# 命中即成功，参数里能看到实际打开的路径与 pt
```

## yac 侧 API 与 JS 绑定对齐

`pkg/gui/yui.yac` 的名字/语义对齐 `yui/docs/yui-js-api.md` 与 `yui/lib/jsmodule-quickjs`，
底层都走 `app/native/yui_yac.c` 里**按 id 寻址**的导出（库里的 `yui_set_text` /
`yui_set_visible` / `yui_update` 收的是 `Layer*`，yac 拿不到，所以这些包装在胶水里）：

| JS API (`YUI.*`) | C 导出 | yac |
|---|---|---|
| `setText(id, text)` | `yui_yac_set_text` | `yui_set_text` |
| `getText(id)` | `yui_yac_get_text` | `yui_get_text`（返回**宿主机 `char*`**，非 yac 字符串） |
| `setBgColor(id, hex)` | `yui_dom_set_bg` | `yui_set_bg` |
| `hide/show(id)` | `yui_yac_set_visible(id, 0/1)` | `yui_hide` / `yui_show` |
| `renderFromJson(id, json)` | `yui_dom_create` | `yui_render_from_json` |
| `update(json)` | `yui_yac_update_json` | `yui_update` |
| `inspect.enable/disable()` | `yui_yac_inspect` | `yui_inspect_enable/disable` |
| `inspect.setShowBounds/Info()` | `yui_yac_inspect_bounds/info` | 同名 |
| `resize(w, h)` | `yui_yac_resize` | `yui_resize` |
| tick / init | `yui_yac_tick` / `yui_yac_init` | `yui_tick` / `yui_init` |

注意：

- 没有 `should_quit` 导出，退出信号就是 `yui_tick` 的**返回值**（`backend_should_quit`），
  `yui_should_quit` 恒返回 0，避免被误读成"该退出了"。
- `yui_sym2` 会**惰性打开** `app/native/libyui_yac.so`，所以只调 `yui_init` 的程序也能用；
  换路径仍可用 `yui_bridge_open(path)`。
- ccall 只能把数字/C 字符串传进去，返回值是字符串的一律回来是 `void*`。

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
