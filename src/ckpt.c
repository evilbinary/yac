#include "ckpt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtio.h"

/* ---- reachable-object collection ---- */

typedef struct { Frame *ptr; long id; } FRef;
typedef struct { Closure *ptr; long id; } CRef;
typedef struct { CFrame *ptr; long id; } RRef;

typedef struct {
    FRef *frames;
    int nframes, capframes;
    CRef *clos;
    int nclos, capclos;
    RRef *cframes;
    int ncframes, capcframes;
} Ctx;

static long frame_id(Ctx *c, Frame *f) {
    for (int i = 0; i < c->nframes; i++)
        if (c->frames[i].ptr == f) return c->frames[i].id;
    return -1;
}

static long clo_id(Ctx *c, Closure *k) {
    for (int i = 0; i < c->nclos; i++)
        if (c->clos[i].ptr == k) return c->clos[i].id;
    return -1;
}

static long cframe_id(Ctx *c, CFrame *f) {
    for (int i = 0; i < c->ncframes; i++)
        if (c->cframes[i].ptr == f) return c->cframes[i].id;
    return -1;
}

static bool add_frame(Ctx *c, Frame *f) {
    if (!f || frame_id(c, f) >= 0) return false;
    FRef r = {f, c->nframes};
    if (c->nframes >= c->capframes) {
        c->capframes = c->capframes ? c->capframes * 2 : 8;
        c->frames = (FRef *)realloc(c->frames, (size_t)c->capframes * sizeof(FRef));
    }
    c->frames[c->nframes++] = r;
    return true;
}

static bool add_clo(Ctx *c, Closure *k) {
    if (!k || clo_id(c, k) >= 0) return false;
    CRef r = {k, c->nclos};
    if (c->nclos >= c->capclos) {
        c->capclos = c->capclos ? c->capclos * 2 : 8;
        c->clos = (CRef *)realloc(c->clos, (size_t)c->capclos * sizeof(CRef));
    }
    c->clos[c->nclos++] = r;
    return true;
}

static bool add_cframe(Ctx *c, CFrame *f) {
    if (!f || cframe_id(c, f) >= 0) return false;
    RRef r = {f, c->ncframes};
    if (c->ncframes >= c->capcframes) {
        c->capcframes = c->capcframes ? c->capcframes * 2 : 8;
        c->cframes = (RRef *)realloc(c->cframes, (size_t)c->capcframes * sizeof(RRef));
    }
    c->cframes[c->ncframes++] = r;
    return true;
}

static void collect(Ctx *c, const AnfState *st) {
    add_frame(c, st->env);
    for (CFrame *f = st->cframe; f; f = f->prev) {
        add_cframe(c, f);
        add_frame(c, f->env);
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < c->nframes; i++) {
            Frame *f = c->frames[i].ptr;
            if (add_frame(c, f->parent)) changed = true;
            for (int s = 0; s < f->nslots; s++)
                if (f->slots[s].tag == V_FUN || f->slots[s].tag == V_CONT)
                    if (add_clo(c, f->slots[s].u.clo)) changed = true;
        }
        for (int i = 0; i < c->nclos; i++)
            if (add_frame(c, c->clos[i].ptr->frame)) changed = true;
    }
}

/* ---- preorder node walk (matches anf_write's order) ---- */

typedef void (*NodeCb)(const Anf *n, void *ud);

static void walk_nodes(const Anf *n, NodeCb cb, void *ud);
static void walk_atom(const Atom *a, NodeCb cb, void *ud) {
    if (a->kind == AT_LAM) walk_nodes(a->u.lam.body, cb, ud);
}

static void walk_nodes(const Anf *n, NodeCb cb, void *ud) {
    if (!n) return;
    cb(n, ud);
    switch (n->kind) {
    case N_LET:
        walk_atom(&n->u.let.atom, cb, ud);
        walk_nodes(n->u.let.body, cb, ud);
        break;
    case N_LET_CALL:
        walk_atom(&n->u.call.head, cb, ud);
        for (int i = 0; i < n->u.call.nargs; i++) walk_atom(&n->u.call.args[i], cb, ud);
        walk_nodes(n->u.call.body, cb, ud);
        break;
    case N_IF:
        walk_atom(&n->u.if_.cond, cb, ud);
        walk_nodes(n->u.if_.then, cb, ud);
        walk_nodes(n->u.if_.els, cb, ud);
        break;
    case N_TAIL_CALL:
        walk_atom(&n->u.tailcall.head, cb, ud);
        for (int i = 0; i < n->u.tailcall.nargs; i++) walk_atom(&n->u.tailcall.args[i], cb, ud);
        break;
    case N_RETURN:
        walk_atom(&n->u.ret, cb, ud);
        break;
    case N_LET_CALLCC:
        walk_atom(&n->u.callcc.atom, cb, ud);
        walk_nodes(n->u.callcc.body, cb, ud);
        break;
    case N_TAIL_THROW:
        walk_atom(&n->u.tailthrow.k, cb, ud);
        walk_atom(&n->u.tailthrow.v, cb, ud);
        break;
    }
}

typedef struct {
    const Anf *target;
    long found;
    long cur;
} IndexWalk;

static void index_cb(const Anf *n, void *ud) {
    IndexWalk *w = (IndexWalk *)ud;
    if (w->found >= 0) return;
    if (n == w->target) {
        w->found = w->cur;
        return;
    }
    w->cur++;
}

typedef struct {
    long want;
    const Anf *found;
    long cur;
} FindWalk;

static void find_cb(const Anf *n, void *ud) {
    FindWalk *f = (FindWalk *)ud;
    if (f->found) return;
    if (f->cur == f->want) {
        f->found = n;
        return;
    }
    f->cur++;
}

/* ---- writer ---- */

static void put_name(FILE *f, const char *s) {
    fputc('"', f);
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

static void write_value(FILE *f, Ctx *c, Value v) {
    switch (v.tag) {
    case V_INT: fprintf(f, "i %lld", (long long)v.u.i); break;
    case V_FLOAT: fprintf(f, "f %.17g", v.u.f); break;
    case V_BOOL: fprintf(f, "b %d", v.u.b ? 1 : 0); break;
    case V_UNIT: fputs("u", f); break;
    case V_STR: fputs("s ", f); put_name(f, v.u.s->data); break;
    case V_FUN:
    case V_CONT: fprintf(f, "c %ld", clo_id(c, v.u.clo)); break;
    case V_PRIM: fputs("p ", f); put_name(f, v.u.prim->name); break;
    }
}

int ckpt_dump(const AnfState *st, long step, const char *path) {
    Ctx c = {0};
    collect(&c, st);
    IndexWalk iw = {st->node, -1, 0};
    walk_nodes(st->root, index_cb, &iw);
    if (iw.found < 0) {
        free(c.frames);
        free(c.clos);
        free(c.cframes);
        return 1;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        free(c.frames);
        free(c.clos);
        free(c.cframes);
        return 1;
    }
    fprintf(f, "(ckpt 1 %ld %ld\n", iw.found, step);

    fputs("(frames\n", f);
    for (int i = 0; i < c.nframes; i++) {
        Frame *fr = c.frames[i].ptr;
        fprintf(f, "(f %ld %ld %d", c.frames[i].id, frame_id(&c, fr->parent), fr->nslots);
        for (int s = 0; s < fr->nslots; s++) {
            fputc(' ', f);
            write_value(f, &c, fr->slots[s]);
        }
        fputs(")\n", f);
    }
    fputs(")\n", f);

    fputs("(cframes\n", f);
    for (int i = 0; i < c.ncframes; i++) {
        CFrame *cf = c.cframes[i].ptr;
        IndexWalk rw = {cf->rest, -1, 0};
        walk_nodes(st->root, index_cb, &rw);
        fprintf(f, "(c %ld %ld %d %ld %ld)\n", c.cframes[i].id,
                cframe_id(&c, cf->prev), cf->slot, rw.found, frame_id(&c, cf->env));
    }
    fputs(")\n", f);

    fputs("(clos\n", f);
    for (int i = 0; i < c.nclos; i++) {
        Closure *k = c.clos[i].ptr;
        IndexWalk bw = {k->body, -1, 0};
        walk_nodes(st->root, index_cb, &bw);
        fprintf(f, "(g %ld %ld {", c.clos[i].id, bw.found);
        for (int p = 0; p < k->nparams; p++) {
            fputc(' ', f);
            put_name(f, k->params[p]);
        }
        fprintf(f, " } %d %d %ld)\n", k->nslots, k->kslot,
                frame_id(&c, k->frame));
    }
    fputs(")\n", f);

    fputs("(rt 0 ", f);
    anf_write(st->root, f);
    fputs("))\n", f);

    fclose(f);
    free(c.frames);
    free(c.clos);
    free(c.cframes);
    return 0;
}

/* ---- reader ---- */

typedef struct { Value v; long cref; bool ref; } PV;

typedef struct {
    long id, parent;
    int nslots;
    PV *vals; /* cref only meaningful when ref */
} TFrame;

typedef struct { long id, prev; int slot; long nodeidx; long envf; } TCFrame;

typedef struct { long id, bodyidx; long fid; int np, nslots, kslot; char **params; } TClo;

static bool rd_value(Rd *r, Value *out, long *cref, bool *ref) {
    char *tag = rd_word(r);
    if (strcmp(tag, "i") == 0) { *out = v_int(strtoll(rd_word(r), NULL, 10)); *ref = false; return true; }
    if (strcmp(tag, "f") == 0) { *out = v_float(strtod(rd_word(r), NULL)); *ref = false; return true; }
    if (strcmp(tag, "b") == 0) { *out = v_bool(atoi(rd_word(r)) != 0); *ref = false; return true; }
    if (strcmp(tag, "u") == 0) { *out = v_unit(); *ref = false; return true; }
    if (strcmp(tag, "s") == 0) { *out = v_str(rd_arena(r), rd_name(r)); *ref = false; return true; }
    if (strcmp(tag, "c") == 0) { *cref = strtol(rd_word(r), NULL, 10); *ref = true; return true; }
    if (strcmp(tag, "p") == 0) {
        const Prim *p = prim_lookup(rd_name(r));
        *out = p ? v_prim(p) : VALUE_NULL;
        *ref = false;
        return true;
    }
    char b[256];
    snprintf(b, sizeof(b), "bad checkpoint value '%s'", tag);
    rd_set_error(r, b);
    return false;
}

bool ckpt_resume(const char *path, Arena *a, Gc *gc, const Anf **root,
                 const Anf **node, Frame **env, CFrame **cframe, long *step,
                 char **errmsg) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errmsg) *errmsg = "cannot open checkpoint file";
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = '\0';
    fclose(f);

    Rd *r = rd_open(buf, (size_t)sz, a);
    bool ok = false;
    long nodeidx = -1, saved_step = 0;
    TFrame *tfs = NULL;
    int ntf = 0, ctf = 0;
    TCFrame *tcs = NULL;
    int ntc = 0, ctc = 0;
    TClo *tks = NULL;
    int ntk = 0, ctk = 0;

    rd_expect(r, '(');
    char *kw = rd_word(r);
    if (rd_error(r) || strcmp(kw, "ckpt") != 0) {
        if (errmsg) *errmsg = (char *)rd_error(r);
        goto done;
    }
    rd_word(r); /* version */
    nodeidx = strtol(rd_word(r), NULL, 10);
    saved_step = strtol(rd_word(r), NULL, 10);

    /* frames */
    rd_expect(r, '(');
    kw = rd_word(r);
    if (strcmp(kw, "frames") != 0) goto done;
    while (rd_peek(r) == '(') {
        rd_expect(r, '(');
        char *tag = rd_word(r);
        if (strcmp(tag, "f") != 0) goto done;
        if (ntf >= ctf) {
            ctf = ctf ? ctf * 2 : 8;
            tfs = (TFrame *)realloc(tfs, (size_t)ctf * sizeof(TFrame));
        }
        TFrame *t = &tfs[ntf++];
        t->id = strtol(rd_word(r), NULL, 10);
        t->parent = strtol(rd_word(r), NULL, 10);
        t->nslots = atoi(rd_word(r));
        t->vals = (PV *)calloc((size_t)t->nslots, sizeof(PV));
        for (int i = 0; i < t->nslots; i++) rd_value(r, &t->vals[i].v, &t->vals[i].cref, &t->vals[i].ref);
        rd_expect(r, ')');
    }
    rd_expect(r, ')');

    /* cframes */
    rd_expect(r, '(');
    kw = rd_word(r);
    if (strcmp(kw, "cframes") != 0) goto done;
    while (rd_peek(r) == '(') {
        rd_expect(r, '(');
        char *tag = rd_word(r);
        if (strcmp(tag, "c") != 0) goto done;
        if (ntc >= ctc) {
            ctc = ctc ? ctc * 2 : 8;
            tcs = (TCFrame *)realloc(tcs, (size_t)ctc * sizeof(TCFrame));
        }
        TCFrame *t = &tcs[ntc++];
        t->id = strtol(rd_word(r), NULL, 10);
        t->prev = strtol(rd_word(r), NULL, 10);
        t->slot = atoi(rd_word(r));
        t->nodeidx = strtol(rd_word(r), NULL, 10);
        t->envf = strtol(rd_word(r), NULL, 10);
        rd_expect(r, ')');
    }
    rd_expect(r, ')');

    /* closures */
    rd_expect(r, '(');
    kw = rd_word(r);
    if (strcmp(kw, "clos") != 0) goto done;
    while (rd_peek(r) == '(') {
        rd_expect(r, '(');
        char *tag = rd_word(r);
        if (strcmp(tag, "g") != 0) goto done;
        if (ntk >= ctk) {
            ctk = ctk ? ctk * 2 : 8;
            tks = (TClo *)realloc(tks, (size_t)ctk * sizeof(TClo));
        }
        TClo *t = &tks[ntk++];
        t->id = strtol(rd_word(r), NULL, 10);
        t->bodyidx = strtol(rd_word(r), NULL, 10);
        rd_expect(r, '{');
        char **params = NULL;
        int np = 0, cap = 0;
        while (rd_peek(r) == '"') {
            if (np >= cap) {
                cap = cap ? cap * 2 : 4;
                char **npa = (char **)arena_alloc(rd_arena(r), (size_t)cap * sizeof(char *));
                memcpy(npa, params, (size_t)np * sizeof(char *));
                params = npa;
            }
            params[np++] = arena_strdup(rd_arena(r), rd_name(r));
        }
        rd_expect(r, '}');
        t->params = params;
        t->np = np;
        t->nslots = atoi(rd_word(r));
        t->kslot = atoi(rd_word(r));
        t->fid = strtol(rd_word(r), NULL, 10);
        rd_expect(r, ')');
    }
    rd_expect(r, ')');

    /* the IR tree */
    Anf *tree = NULL;
    if (!anf_read(r, &tree, NULL)) goto done;

    /* resolve the current node by index */
    FindWalk fw = {nodeidx, NULL, 0};
    walk_nodes(tree, find_cb, &fw);
    if (!fw.found) {
        if (errmsg) *errmsg = "checkpoint: node index out of range";
        goto done;
    }

    /* allocate frames */
    Frame **farr = (Frame **)calloc((size_t)ntf, sizeof(Frame *));
    for (int i = 0; i < ntf; i++) farr[i] = gc_new_frame(gc, tfs[i].nslots);
    /* allocate closures */
    Closure **karr = (Closure **)calloc((size_t)ntk, sizeof(Closure *));
    for (int i = 0; i < ntk; i++) karr[i] = gc_new_closure(gc);
    /* allocate continuation frames */
    CFrame **carr = (CFrame **)calloc((size_t)ntc, sizeof(CFrame *));
    for (int i = 0; i < ntc; i++) carr[i] = (CFrame *)gc_alloc(gc, G_FRAME, sizeof(CFrame));

    /* fix frame parents and fill slots */
    for (int i = 0; i < ntf; i++) {
        Frame *fr = farr[i];
        fr->parent = tfs[i].parent >= 0 ? farr[tfs[i].parent] : NULL;
        for (int s = 0; s < tfs[i].nslots; s++) {
            if (tfs[i].vals[s].ref) fr->slots[s] = v_fun(karr[tfs[i].vals[s].cref]);
            else fr->slots[s] = tfs[i].vals[s].v;
        }
        free(tfs[i].vals);
    }

    /* fix closures */
    for (int i = 0; i < ntk; i++) {
        Closure *k = karr[i];
        FindWalk bw = {tks[i].bodyidx, NULL, 0};
        walk_nodes(tree, find_cb, &bw);
        k->body = (void *)bw.found;
        k->params = tks[i].params;
        k->nparams = tks[i].np;
        k->nslots = tks[i].nslots;
        k->kslot = tks[i].kslot;
        k->frame = tks[i].fid >= 0 ? farr[tks[i].fid] : NULL;
        k->cont_name = NULL;
        k->rslot = -1;
    }

    /* fix continuation frames */
    CFrame *cftop = NULL;
    for (int i = 0; i < ntc; i++) {
        CFrame *cf = carr[i];
        FindWalk nw = {tcs[i].nodeidx, NULL, 0};
        walk_nodes(tree, find_cb, &nw);
        cf->prev = tcs[i].prev >= 0 ? carr[tcs[i].prev] : NULL;
        cf->slot = tcs[i].slot;
        cf->rest = (const Anf *)nw.found;
        cf->env = farr[tcs[i].envf];
        if (tcs[i].id == 0) cftop = cf;
    }
    /* cframes were written in stack order, so id 0 is the current top */
    if (ntc > 0 && !cftop) cftop = carr[0];

    *root = tree;
    *node = fw.found;
    *env = farr[0];
    *cframe = cftop;
    *step = saved_step;
    gc_set_env(gc, farr[0]);
    gc_set_frame(gc, (GObj *)cftop);
    ok = true;

    free(farr);
    free(karr);
    free(carr);

done:
    if (!ok && errmsg && !*errmsg) *errmsg = (char *)rd_error(r);
    rd_close(r);
    free(buf);
    free(tfs);
    free(tcs);
    free(tks);
    return ok;
}