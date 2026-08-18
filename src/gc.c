#include "gc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eval_anf.h"

/* ---- helpers ---- */

static void oom(void) {
    fprintf(stderr, "out of memory\n");
    exit(1);
}

static void runaway(const Gc *g) {
    fprintf(stderr, "runaway program: %zu live objects exceed limit %zu\n",
            g->total_objs, g->max_objs);
    exit(1);
}

/* ---- marking ---- */

static void mark_obj(Gc *g, GObj *o);

static void mark_value(Gc *g, Value v) {
    if (v.tag == V_FUN || v.tag == V_CONT) mark_obj(g, (GObj *)v.u.clo);
}

static void mark_obj(Gc *g, GObj *o) {
    if (!o || o->mark) return;
    o->mark = 1;
    switch (o->kind) {
    case G_STR:
        break; /* string data is arena-resident (literals only) */
    case G_BIND: {
        Binding *b = (Binding *)o;
        mark_value(g, b->value);
        mark_obj(g, (GObj *)b->prev);
        break;
    }
    case G_CLO: {
        Closure *c = (Closure *)o;
        mark_obj(g, (GObj *)c->env);
        break;
    }
    case G_FRAME: {
        Frame *f = (Frame *)o;
        mark_obj(g, (GObj *)f->prev);
        mark_obj(g, (GObj *)f->env);
        break;
    }
    case G_VALARR: {
        ValArr *va = (ValArr *)o;
        for (int i = 0; i < va->n; i++) mark_value(g, va->data[i]);
        break;
    }
    }
}

static void mark_roots(Gc *g) {
    mark_obj(g, (GObj *)g->envroot);
    mark_obj(g, g->frameroot);
    for (int i = 0; i < g->nroots; i++) mark_obj(g, g->roots[i]);
}

/* ---- allocation ---- */

static void *gc_alloc_raw(Gc *g, GKind kind, size_t size) {
    if (g->enabled && g->allocated > g->threshold) gc_collect(g);
    size_t total = size;
    total = (total + 15) & ~(size_t)15;
    GObj *o = (GObj *)malloc(total);
    if (!o) oom();
    o->next = g->all;
    o->size = total;
    o->kind = kind;
    o->mark = 0;
    g->all = o;
    g->total_objs++;
    g->allocated += total;
    if (g->max_objs && g->total_objs > g->max_objs) runaway(g);
    return o;
}

void *gc_alloc(Gc *g, GKind kind, size_t size) {
    return gc_alloc_raw(g, kind, size);
}

Binding *gc_new_binding(Gc *g, const char *name, Value v, Binding *prev) {
    Binding *b = (Binding *)gc_alloc_raw(g, G_BIND, sizeof(Binding));
    b->name = name;
    b->value = v;
    b->prev = prev;
    return b;
}

Closure *gc_new_closure(Gc *g) {
    return (Closure *)gc_alloc_raw(g, G_CLO, sizeof(Closure));
}

ValArr *gc_new_valarr(Gc *g, int n) {
    ValArr *va = (ValArr *)gc_alloc_raw(g, G_VALARR,
                                        sizeof(ValArr) + (size_t)n * sizeof(Value));
    va->n = n;
    return va;
}

/* ---- roots ---- */

void gc_push_root(Gc *g, GObj *o) {
    if (g->nroots >= g->caproots) {
        g->caproots = g->caproots ? g->caproots * 2 : 16;
        g->roots = (GObj **)realloc(g->roots, (size_t)g->caproots * sizeof(GObj *));
        if (!g->roots) oom();
    }
    g->roots[g->nroots++] = o;
}

void gc_pop_root(Gc *g) {
    if (g->nroots > 0) g->nroots--;
}

void gc_push_value(Gc *g, Value v) {
    if (v.tag == V_FUN || v.tag == V_CONT) gc_push_root(g, (GObj *)v.u.clo);
}

void gc_set_env(Gc *g, Binding *env) {
    g->envroot = env;
}

void gc_set_frame(Gc *g, GObj *frame) {
    g->frameroot = frame;
}

/* ---- collection ---- */

static void free_obj(GObj *o) {
    /* all payloads are contiguous with the header; string data is inline */
    free(o);
}

void gc_collect(Gc *g) {
    if (!g->enabled) {
        g->allocated = 0;
        return;
    }
    mark_roots(g);

    /* sweep: rebuild the all-list keeping only marked objects */
    GObj **p = &g->all;
    GObj *o = g->all;
    size_t live = 0, nobjs = 0;
    while (o) {
        GObj *next = o->next;
        if (o->mark) {
            o->mark = 0;
            *p = o;
            p = &o->next;
            live += o->size;
            nobjs++;
        } else {
            free_obj(o);
        }
        o = next;
    }
    *p = NULL;
    g->live = live;
    g->live_objs = nobjs;
    g->total_objs = nobjs;
    g->allocated = 0;
    if (g->max_objs && nobjs > g->max_objs) runaway(g);
}

void gc_init(Gc *g, size_t threshold) {
    memset(g, 0, sizeof(*g));
    g->threshold = threshold > 0 ? threshold : 1024 * 1024;
    g->enabled = true;
    g->envroot = NULL;
    g->roots = NULL;
    g->nroots = g->caproots = 0;
}

void gc_free(Gc *g) {
    GObj *o = g->all;
    while (o) {
        GObj *next = o->next;
        free(o);
        o = next;
    }
    g->all = NULL;
    free(g->roots);
    g->roots = NULL;
    g->nroots = g->caproots = 0;
}