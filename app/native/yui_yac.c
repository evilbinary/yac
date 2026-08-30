/* Yac ↔ YUI glue. Register click handler before yui_init; poll clicks as ints. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yui_boot.h"
#include "event.h"
#include "layer.h"
#include "backend.h"
#include "game/game.h"

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

int yui_yac_game_drop(const char* tag, int x, int rgb) {
    static int seq;
    char id[GAME_ID_LEN];
    GameEntity* e;
    Color c;
    if (!tag) {
        return -1;
    }
    snprintf(id, sizeof(id), "%s_%d", tag, seq++);
    e = game_spawn(id);
    if (!e) {
        return -1;
    }
    strncpy(e->tag, tag, GAME_ID_LEN - 1);
    e->x = (float)x;
    e->y = -18.0f;
    e->z = 5.0f;
    e->w = 14.0f;
    e->h = 14.0f;
    e->vy = (float)(55 + rand() % 50);
    yui_yac_rgb(rgb, &c);
    e->color = c;
    return 0;
}

int yui_yac_game_eat(const char* player_id, const char* tag) {
    GameEntity* p = game_find(player_id);
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

int yui_yac_game_fall(int y_max) {
    int n = 0;
    GameEntity** all = game_entities(&n);
    int i;
    for (i = 0; i < n; i++) {
        GameEntity* e = all[i];
        if (!e || !e->alive) {
            continue;
        }
        if (strcmp(e->id, "moth") == 0 || strcmp(e->id, "sky") == 0) {
            continue;
        }
        if (strcmp(e->tag, "lantern") == 0 || strcmp(e->tag, "bg") == 0) {
            continue;
        }
        if ((int)e->y > y_max) {
            game_destroy(e);
        }
    }
    return 0;
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
