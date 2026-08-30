# Yac GUI apps (YUI)

Yac talks to YUI through `ccall` / `cload`. UI is JSON; button `onClick` is
`yac_click`. Clicks are polled as integers (`yui_click_is("plus")`) because
`ccall` cannot return a Yac string.

`native/libyui_yac.so` is PIC glue plus the static libs in
`/media/evil/d/dev/yui/build/pc/None/None`. Those `.a` files must be built
with `-fPIC` (YUI `add_flags()` on pc). Link order is a DAG, not a cycle:
`libyui.a` → `libyaml2json.a` → `libyaml.a` / `libcjson.a`, plus `libtsm.a`
and `libsocket.a`.

## Layout

| path | role |
| --- | --- |
| `native/yui.yac` | Yac bindings. Game: `spawn`/`box`/`vel`/`pos`/`get`/`hit`/`cull`/`burst` |
| `native/yui_yac.c` | C glue: init, tick, set_text, click_is |
| `native/libyui_yac.so` | shared lib (built by make) |
| `counter/` | increment / decrement / reset |
| `lantern/` | WASD moth: catch gold embers, dodge ash |

## Build

From the Yac repo root:

```
make -C app
```

`libyui.a` from `ya` is ASan-instrumented, so the `.so` is too. Preload ASan
before the Yac binary:

```
export LD_PRELOAD=$(gcc -print-file-name=libasan.so)
cd /path/to/yac
./app/counter/counter
./app/hello/hello
./app/lantern/lantern
```

**Lantern Moth** uses YUI’s game entities (color rects, particles, input). Gold bits fall — catch them; gray ash subtracts a point. YUI draws the game **under** the UI tree, so HUD roots/labels must use `bgColor: "transparent"` (same as `yui/app/game/demo.json`); an opaque stage View will hide the scene. JSON-only edits do not need a rebuild.

JSON path and `.so` path are optional argv:

```
./app/counter/counter app/counter/ui.json app/native/libyui_yac.so
```

Headless / smoke (quit after N frames):

```
YUI_HEADLESS=1 YUI_AUTO_FRAMES=30 ./app/counter/counter
```

Run from the Yac repo root so `app/counter/ui.json` and `app/native/libyui_yac.so` resolve.

Fonts: `ui.json` uses DejaVu at
`/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf`.
