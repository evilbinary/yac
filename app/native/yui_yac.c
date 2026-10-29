/* Yac ↔ YUI glue. Register click handler before yui_init; poll clicks as ints. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yui_boot.h"
#include "event.h"
#include <SDL2/SDL.h>
#include "layer.h"
#include "backend.h"
#include "game/game.h"
#include "layer_update.h"
#include "layer_lifecycle.h"
#include "layout.h"
#include "render.h"
#include "theme_manager.h"
#include "log.h"

static char g_click[YUI_LAYER_ID_MAX];
static int g_have_click;
static int g_auto_left = -1;

static void* yac_on_click(void* data) {
    Layer* layer = (Layer*)data;
    if (!layer || layer->id[0] == '\0') {
        return NULL;
    }
    strncpy(g_click, layer->id, sizeof(g_click) - 1);
    g_click[sizeof(g_click) - 1] = '\0';
    g_have_click = 1;
    if (strcmp(layer->id, "quit") == 0) {
        backend_request_quit(0);
    }
    return NULL;
}

int yui_yac_init(const char* json_path, const char* assets_dir) {
    /* We live in a shared library, not the SDL_main entry point. */
    SDL_SetMainReady();
    const char* assets = assets_dir;
    const char* af;
    g_have_click = 0;
    g_click[0] = '\0';
    if (getenv("YUI_HEADLESS") != NULL) {
        backend_set_headless(1);
    }
    af = getenv("YUI_AUTO_FRAMES");
    g_auto_left = (af && af[0]) ? atoi(af) : -1;
    register_event_handler("yac_click", yac_on_click);
    if (assets && assets[0] == '\0') {
        assets = NULL;
    }
    return yui_init(json_path, assets);
}

static void yui_yac_rgb(int rgb, Color* c) {
    c->r = (unsigned char)((rgb >> 16) & 255);
    c->g = (unsigned char)((rgb >> 8) & 255);
    c->b = (unsigned char)(rgb & 255);
    c->a = 255;
}

int yui_yac_game_boot(const char* scene_path) {
    srand((unsigned)backend_get_ticks());
    game_init();
    game_set_enabled(1);
    game_set_paused(0);
    if (!scene_path || scene_path[0] == '\0') {
        return 0;
    }
    if (!game_load_scene_json(scene_path)) {
        fprintf(stderr, "yui: game scene failed: %s\n", scene_path);
        return -1;
    }
    return 0;
}

int yui_yac_game_key(const char* name) {
    return name ? game_input_down(name) : 0;
}

int yui_yac_game_vel(const char* id, int vx, int vy) {
    GameEntity* e = game_find(id);
    if (!e) {
        return -1;
    }
    e->vx = (float)vx;
    e->vy = (float)vy;
    return 0;
}

int yui_yac_game_get(const char* id, int which) {
    GameEntity* e = game_find(id);
    if (!e) {
        return 0;
    }
    if (which == 1) {
        return (int)e->y;
    }
    if (which == 2) {
        return e->alive;
    }
    return (int)e->x;
}

int yui_yac_game_pos(const char* id, int x, int y) {
    GameEntity* e = game_find(id);
    if (!e) {
        return -1;
    }
    e->x = (float)x;
    e->y = (float)y;
    return 0;
}

int yui_yac_game_spawn(const char* id, const char* tag, int x, int y, int rgb) {
    static int seq;
    char auto_id[GAME_ID_LEN];
    const char* use = id;
    GameEntity* e;
    Color c;
    if (!tag || tag[0] == '\0') {
        return -1;
    }
    if (!use || use[0] == '\0') {
        snprintf(auto_id, sizeof(auto_id), "%s_%d", tag, seq++);
        use = auto_id;
    }
    e = game_spawn(use);
    if (!e) {
        return -1;
    }
    strncpy(e->tag, tag, GAME_ID_LEN - 1);
    e->tag[GAME_ID_LEN - 1] = '\0';
    e->x = (float)x;
    e->y = (float)y;
    e->z = 5.0f;
    e->w = 16.0f;
    e->h = 16.0f;
    e->cw = 16.0f;
    e->ch = 16.0f;
    yui_yac_rgb(rgb, &c);
    e->color = c;
    return 0;
}

int yui_yac_game_box(const char* id, int w, int h) {
    GameEntity* e = game_find(id);
    if (!e) {
        return -1;
    }
    e->w = w > 0 ? (float)w : e->w;
    e->h = h > 0 ? (float)h : e->h;
    e->cw = e->w;
    e->ch = e->h;
    return 0;
}

int yui_yac_game_hit(const char* id, const char* tag) {
    GameEntity* p = game_find(id);
    GameEntity* hits[32];
    int n;
    int i;
    if (!p || !tag) {
        return 0;
    }
    n = game_find_all_by_tag(tag, hits, 32);
    for (i = 0; i < n; i++) {
        if (hits[i] && game_entities_overlap(p, hits[i])) {
            game_destroy(hits[i]);
            return 1;
        }
    }
    return 0;
}

int yui_yac_game_cull(const char* tag, int y_max) {
    GameEntity* hits[64];
    int n;
    int i;
    int gone = 0;
    if (!tag || tag[0] == '\0') {
        return 0;
    }
    n = game_find_all_by_tag(tag, hits, 64);
    for (i = 0; i < n; i++) {
        if (hits[i] && hits[i]->alive && (int)hits[i]->y > y_max) {
            game_destroy(hits[i]);
            gone++;
        }
    }
    return gone;
}

int yui_yac_game_burst(int x, int y, int rgb) {
    Color c;
    yui_yac_rgb(rgb, &c);
    return game_spawn_particles((float)x, (float)y, 14, c, 90.0f, 0.45f);
}

int yui_yac_rand(int n) {
    if (n <= 0) {
        return 0;
    }
    return rand() % n;
}

int yui_yac_tick(void) {
    yui_tick();
    if (g_auto_left >= 0) {
        if (g_auto_left == 0) {
            backend_request_quit(0);
        } else {
            g_auto_left--;
        }
    }
    backend_delay(16);
    return backend_should_quit();
}

int yui_yac_set_text(const char* id, const char* text) {
    Layer* root = yui_get_root();
    Layer* layer;
    if (!root || !id) {
        return -1;
    }
    layer = find_layer_by_id(root, id);
    if (!layer) {
        return -1;
    }
    layer_set_text(layer, text ? text : "");
    return 0;
}

/* ---- YUI.* parity with yui/docs/yui-js-api.md ---------------------------
 * The mquickjs/QuickJS bindings expose YUI.setText/getText/setBgColor/hide/
 * show/renderFromJson/update/resize/inspect.*; those engines resolve an id to
 * a Layer inside the binding. A yac program reaches the same surface through
 * pkg/gui/yui.yac, so the id-addressed entry points live here -- the library
 * functions (yui_set_text, yui_set_visible, yui_update) take a Layer*. */
extern int yui_inspect_mode_enabled;
extern int yui_inspect_show_bounds;
extern int yui_inspect_show_info;

static Layer* yac_layer(const char* id) {
    Layer* root = yui_get_root();
    if (!root || !id || !id[0]) {
        return NULL;
    }
    return find_layer_by_id(root, id);
}

const char* yui_yac_get_text(const char* id) {
    Layer* layer = yac_layer(id);
    const char* text;
    if (!layer) {
        return "";
    }
    text = layer_get_text(layer);
    return text ? text : "";
}

int yui_yac_set_visible(const char* id, int visible) {
    Layer* layer = yac_layer(id);
    if (!layer) {
        return -1;
    }
    layer_set_visible(layer, visible != 0);
    return 0;
}

int yui_yac_update_json(const char* json) {
    Layer* root = yui_get_root();
    if (!root || !json) {
        return -1;
    }
    return yui_update(root, json);
}

int yui_yac_resize(int width, int height) {
    yui_resize(width, height);
    return 0;
}

int yui_yac_inspect(int enabled) {
    yui_inspect_mode_enabled = enabled != 0;
    return yui_inspect_mode_enabled;
}

int yui_yac_inspect_bounds(int show) {
    yui_inspect_show_bounds = show != 0;
    return yui_inspect_show_bounds;
}

int yui_yac_inspect_info(int show) {
    yui_inspect_show_info = show != 0;
    return yui_inspect_show_info;
}

/* YUI.inspect.setLayer(layerId, enabled): per-layer flag (Layer.inspect_enabled). */
int yui_yac_inspect_layer(const char* id, int enabled) {
    Layer* layer = yac_layer(id);
    if (!layer) {
        return -1;
    }
    layer->inspect_enabled = enabled != 0;
    return layer->inspect_enabled;
}

/* YUI.log(msg): yui_log is variadic, so pass a format and the message. */
int yui_yac_log(const char* msg) {
    yui_log(YUI_LOG_INFO, "yac", "%s", msg ? msg : "");
    return 0;
}

/* YUI.themeLoad / themeSetCurrent / themeUnload / themeApplyToTree. */
int yui_yac_theme_load(const char* path) {
    if (!path || !path[0]) {
        return -1;
    }
    return theme_manager_load_theme(path) ? 0 : -1;
}

int yui_yac_theme_current(const char* name) {
    if (!name) {
        return -1;
    }
    return theme_manager_set_current(name);
}

int yui_yac_theme_unload(const char* name) {
    if (!name) {
        return -1;
    }
    theme_manager_unload_theme(name);
    return 0;
}

int yui_yac_theme_apply(void) {
    Layer* root = yui_get_root();
    if (!root) {
        return -1;
    }
    theme_manager_apply_to_tree(root);
    return 0;
}

int yui_yac_click_is(const char* id) {
    if (!id || !g_have_click) {
        return 0;
    }
    if (strcmp(g_click, id) != 0) {
        return 0;
    }
    g_have_click = 0;
    g_click[0] = '\0';
    return 1;
}

void yui_yac_shutdown(void) {
    game_shutdown();
    yui_shutdown();
    g_have_click = 0;
    g_click[0] = '\0';
}

/* ---- DOM bridge: dynamic layers addressed by id (yui_dom_*) --------------
 * The browser.yac DOM stubs create real yui layers so a translated web app
 * renders in a yui window. Ids are minted by the caller (browser.yac keeps a
 * counter); the JSON fragment carries the same id. */
int yui_dom_create(const char* parent_id, const char* json) {
    Layer* root = yui_get_root();
    Layer* parent;
    Layer* nl;
    if (!root || !parent_id || !json) {
        return -1;
    }
    parent = parent_id[0] == '\0' ? root : find_layer_by_id(root, parent_id);
    if (!parent) {
        return -2;
    }
    nl = parse_layer_from_string(json, parent);
    if (!nl) {
        return -3;
    }
    nl->visible = IN_VISIBLE;
    {
        Layer** nc = realloc(parent->children, sizeof(Layer*) * (parent->child_count + 1));
        if (!nc) {
            layer_lifecycle_before_destroy(nl);
            destroy_layer(nl);
            return -4;
        }
        parent->children = nc;
        parent->children[parent->child_count] = nl;
        parent->child_count++;
    }
    layout_layer(parent);
    load_all_fonts(nl);
    theme_manager_apply_to_tree(nl);
    return 0;
}

int yui_dom_remove(const char* parent_id, const char* child_id) {
    Layer* root = yui_get_root();
    Layer* parent;
    if (!root || !parent_id) {
        return -1;
    }
    parent = find_layer_by_id(root, parent_id);
    if (!parent) {
        return -2;
    }
    return yui_remove_child(parent, child_id);
}

int yui_dom_set_bg(const char* id, const char* hex) {
    Layer* root = yui_get_root();
    Layer* l;
    if (!root || !id) {
        return -1;
    }
    l = find_layer_by_id(root, id);
    if (!l) {
        return -2;
    }
    yui_set_bg_color(l, hex ? hex : "#000000");
    return 0;
}
