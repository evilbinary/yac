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
    else if (v.tag == V_LIST) mark_obj(g, (GObj *)v.u.l);
    else if (v.tag == V_BIG) mark_obj(g, (GObj *)v.u.big);
    else if (v.tag == V_BYTES) mark_obj(g, (GObj *)v.u.bytes);
}

static void mark_obj(Gc *g, GObj *o) {
    if (!o || o->mark) return;
    o->mark = 1;
    switch (o->kind) {
    case G_STR:
        break; /* string data is arena-resident (literals only) */
    case G_ENVFRAME: {
        Frame *f = (Frame *)o;
        mark_obj(g, (GObj *)f->parent);
        for (int i = 0; i < f->nslots; i++) mark_value(g, f->slots[i]);
        break;
    }
    case G_CLO: {
        Closure *c = (Closure *)o;
        mark_obj(g, (GObj *)c->frame);
        break;
    }
    case G_FRAME: {
        CFrame *f = (CFrame *)o;
        mark_obj(g, (GObj *)f->prev);
        mark_obj(g, (GObj *)f->env);
        break;
    }
    case G_VALARR: {
        ValArr *va = (ValArr *)o;
        for (int i = 0; i < va->n; i++) mark_value(g, va->data[i]);
        break;
    }
    case G_LIST: {
        List *l = (List *)o;
        for (int i = 0; i < l->len; i++) mark_value(g, l->items[i]);
        break;
    }
    case G_BIGNUM:
        break;
    case G_BYTES:
        break; /* payload is a separate raw block; no child pointers */
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

Frame *gc_new_frame(Gc *g, int nslots) {
    Frame *f = (Frame *)gc_alloc_raw(g, G_ENVFRAME,
                                     sizeof(Frame) + (size_t)nslots * sizeof(Value));
    f->parent = NULL;
    f->nslots = nslots;
    /* zero the slots so an unfilled recursive binding / early mark is safe */
    memset(f->slots, 0, (size_t)nslots * sizeof(Value));
    return f;
}

Closure *gc_new_closure(Gc *g) {
    return (Closure *)gc_alloc_raw(g, G_CLO, sizeof(Closure));
}

ValArr *gc_new_valarr(Gc *g, int n) {
    ValArr *va = (ValArr *)gc_alloc_raw(g, G_VALARR,
                                        sizeof(ValArr) + (size_t)n * sizeof(Value));
    va->n = n;
    /* zero the slots: a GC may run while the machine is still filling them
     * (arg evaluation can allocate), and uninitialized Values must not be
     * mistaken for pointers during mark. */
    memset(va->data, 0, (size_t)n * sizeof(Value));
    return va;
}

List *gc_new_list(Gc *g, int n) {
    List *l = (List *)gc_alloc_raw(g, G_LIST, sizeof(List));
    l->len = n;
    l->cap = n;
    if (n == 0) {
        l->items = NULL;
    } else {
        l->items = (Value *)malloc((size_t)n * sizeof(Value));
        if (!l->items) oom();
        memset(l->items, 0, (size_t)n * sizeof(Value));
    }
    return l;
}

Bignum *gc_new_bignum(Gc *g, int ndigits) {
    Bignum *b = (Bignum *)gc_alloc_raw(g, G_BIGNUM,
                                       sizeof(Bignum) + (size_t)ndigits * sizeof(uint32_t));
    b->sign = 0;
    b->ndigits = ndigits;
    memset(b->digits, 0, (size_t)ndigits * sizeof(uint32_t));
    return b;
}

Bytes *gc_new_bytes(Gc *g) {
    Bytes *b = (Bytes *)gc_alloc_raw(g, G_BYTES, sizeof(Bytes));
    b->len = 0;
    b->cap = 0;
    b->data = NULL;
    return b;
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
    GObj *o = (v.tag == V_FUN || v.tag == V_CONT) ? (GObj *)v.u.clo
              : (v.tag == V_LIST) ? (GObj *)v.u.l
              : (v.tag == V_BIG) ? (GObj *)v.u.big
              : (v.tag == V_BYTES) ? (GObj *)v.u.bytes : NULL;
    gc_push_root(g, o); /* always push (NULL for immediates) to keep balance */
}

void gc_set_env(Gc *g, Frame *env) {
    g->envroot = env;
}

void gc_set_frame(Gc *g, GObj *frame) {
    g->frameroot = frame;
}

/* ---- collection ---- */

static void free_obj(GObj *o) {
    if (o->kind == G_BYTES) free(((Bytes *)o)->data); /* separate malloc block */
    if (o->kind == G_LIST) free(((List *)o)->items);
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
    if (getenv("YAC_GC_DBG"))
        fprintf(stderr, "gc: live=%zu objs=%zu roots=%d\n", live, nobjs, g->nroots);
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