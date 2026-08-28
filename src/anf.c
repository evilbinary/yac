#include "anf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bignum.h"

/* Lexical scope entry: a name lives at (frame, slot) where `frame` is the
 * frame id at the time it was bound; a reference from the current frame
 * resolves with depth = curframe - frame. */
typedef struct Scope {
    const char *name;
    int frame;
    int slot;
    struct Scope *prev;
} Scope;

typedef struct {
    Arena *a;
    int fresh;
    char *error;
    int curframe;       /* current frame id (0 = top level) */
    int *frameslots;    /* slot counters per frame id */
    int capframes;
} NrmCtx;

static Scope *scope_push(NrmCtx *c, Scope *s, const char *name, int frame, int slot) {
    Scope *n = (Scope *)arena_alloc(c->a, sizeof(Scope));
    n->name = name;
    n->frame = frame;
    n->slot = slot;
    n->prev = s;
    return n;
}

static bool in_scope(NrmCtx *c, Scope *s, const char *name) {
    (void)c;
    for (Scope *p = s; p; p = p->prev) {
        if (strcmp(p->name, name) == 0) return true;
    }
    return false;
}

/* slot of `name` when it is bound in the current frame; -1 if absent */
static int current_slot(NrmCtx *c, Scope *s, const char *name) {
    for (Scope *p = s; p; p = p->prev) {
        if (strcmp(p->name, name) == 0) {
            if (p->frame == c->curframe) return p->slot;
            return -2;
        }
    }
    return -1;
}

static char *fresh(NrmCtx *c) {
    char buf[32];
    snprintf(buf, sizeof(buf), "#t%d", c->fresh++);
    return arena_strdup(c->a, buf);
}

static void frame_enter(NrmCtx *c) {
    c->curframe++;
    if (c->curframe >= c->capframes) {
        int ncap = c->capframes ? c->capframes * 2 : 16;
        c->frameslots = (int *)realloc(c->frameslots, (size_t)ncap * sizeof(int));
        c->capframes = ncap;
    }
    c->frameslots[c->curframe] = 0;
}

static void frame_exit(NrmCtx *c) {
    c->curframe--;
}

static int alloc_slot(NrmCtx *c) {
    return c->frameslots[c->curframe]++;
}

/* create a fresh temp var, allocate its slot in the current frame, and push
 * it into the scope */
static char *fresh_bind(NrmCtx *c, Scope **scope) {
    char *name = fresh(c);
    int slot = alloc_slot(c);
    *scope = scope_push(c, *scope, name, c->curframe, slot);
    return name;
}

static void nerr(NrmCtx *c, const char *fmt, ...) {
    if (c->error) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    c->error = arena_strdup(c->a, buf);
}

/* ---- IR constructors ---- */

Atom atom_var(const char *name) {
    Atom at;
    at.kind = AT_VAR;
    at.u.var.name = name;
    at.u.var.depth = 0;
    at.u.var.slot = -1;
    return at;
}

Atom atom_var_ds(const char *name, int depth, int slot) {
    Atom at;
    at.kind = AT_VAR;
    at.u.var.name = name;
    at.u.var.depth = depth;
    at.u.var.slot = slot;
    return at;
}

Atom atom_lit(Value v) {
    Atom at;
    at.kind = AT_LIT;
    at.u.lit = v;
    return at;
}

Atom atom_lam(char **params, int nparams, int nslots, Anf *body) {
    Atom at;
    at.kind = AT_LAM;
    at.u.lam.params = params;
    at.u.lam.nparams = nparams;
    at.u.lam.nslots = nslots;
    at.u.lam.body = body;
    return at;
}

static Anf *anf_node(Arena *a, AnfKind kind, int line) {
    Anf *n = (Anf *)arena_alloc(a, sizeof(Anf));
    memset(n, 0, sizeof(Anf));
    n->kind = kind;
    n->line = line;
    return n;
}

Anf *anf_ret(Arena *a, Atom atom) {
    Anf *n = anf_node(a, N_RETURN, 0);
    n->u.ret = atom;
    return n;
}

Anf *anf_let(Arena *a, const char *name, int slot, Atom atom, Anf *body) {
    Anf *n = anf_node(a, N_LET, 0);
    n->u.let.name = name;
    n->u.let.slot = slot;
    n->u.let.atom = atom;
    n->u.let.body = body;
    return n;
}

Anf *anf_let_call(Arena *a, const char *name, int slot, Atom head, Atom *args, int nargs, Anf *body) {
    Anf *n = anf_node(a, N_LET_CALL, 0);
    n->u.call.name = name;
    n->u.call.slot = slot;
    n->u.call.head = head;
    n->u.call.args = args;
    n->u.call.nargs = nargs;
    n->u.call.body = body;
    return n;
}

Anf *anf_if(Arena *a, Atom cond, Anf *then, Anf *els) {
    Anf *n = anf_node(a, N_IF, 0);
    n->u.if_.cond = cond;
    n->u.if_.then = then;
    n->u.if_.els = els;
    return n;
}

Anf *anf_tail_call(Arena *a, Atom head, Atom *args, int nargs) {
    Anf *n = anf_node(a, N_TAIL_CALL, 0);
    n->u.tailcall.head = head;
    n->u.tailcall.args = args;
    n->u.tailcall.nargs = nargs;
    return n;
}

Anf *anf_let_callcc(Arena *a, const char *name, int slot, Atom atom, Anf *body) {
    Anf *n = anf_node(a, N_LET_CALLCC, 0);
    n->u.callcc.name = name;
    n->u.callcc.slot = slot;
    n->u.callcc.atom = atom;
    n->u.callcc.body = body;
    return n;
}

Anf *anf_tail_throw(Arena *a, Atom k, Atom v) {
    Anf *n = anf_node(a, N_TAIL_THROW, 0);
    n->u.tailthrow.k = k;
    n->u.tailthrow.v = v;
    return n;
}

/* ---- atomize: is the AST node atomic? ---- */

static bool atomize(const Ast *e, Atom *out, NrmCtx *c, Scope *scope);

static Anf *norm(const Ast *e, const char *vname, Anf *k, NrmCtx *c, Scope *scope);
static Anf *norm_tail(const Ast *e, const char *vname, Anf *k, NrmCtx *c, Scope *scope);

static int vslot(NrmCtx *c, Scope *s, const char *vname) {
    int sl = current_slot(c, s, vname);
    return sl < 0 ? 0 : sl;
}

/* -- tail position: the value of `e` is the answer, routed through
 *    (vname, k) only for non-jump results. A tail call/prim discards
 *    (vname, k): the value flows back through the caller's frame. -- */

static Anf *norm_tail(const Ast *e, const char *vname, Anf *k, NrmCtx *c, Scope *scope) {
    Atom atom;
    if (atomize(e, &atom, c, scope)) return anf_let(c->a, vname, vslot(c, scope, vname), atom, k);

    switch (e->kind) {
    case A_APP: {
        int nargs = e->u.app.nargs;
        if (nargs == 0) return norm_tail(e->u.app.fn, vname, k, c, scope);
        Atom hat;
        bool h_atom = atomize(e->u.app.fn, &hat, c, scope);
        char *headf = h_atom ? NULL : fresh_bind(c, &scope);
        int hslot = h_atom ? -1 : current_slot(c, scope, headf);
        Atom *args = (Atom *)arena_alloc(c->a, (size_t)nargs * sizeof(Atom));
        Anf *acc = anf_tail_call(c->a, h_atom ? hat : atom_var_ds(headf, 0, hslot), args, nargs);
        for (int i = nargs - 1; i >= 0; i--) {
            Atom a;
            if (atomize(e->u.app.args[i], &a, c, scope)) {
                args[i] = a;
            } else {
                char *af = fresh_bind(c, &scope);
                int aslot = current_slot(c, scope, af);
                args[i] = atom_var_ds(af, 0, aslot);
                acc = norm(e->u.app.args[i], af, acc, c, scope);
            }
        }
        if (!h_atom) return norm(e->u.app.fn, headf, acc, c, scope);
        return acc;
    }
    case A_IF: {
        Atom cond;
        if (atomize(e->u.if_.cond, &cond, c, scope)) {
            Anf *t = norm_tail(e->u.if_.then, vname, k, c, scope);
            Anf *f = norm_tail(e->u.if_.els, vname, k, c, scope);
            return anf_if(c->a, cond, t, f);
        }
        char *cf = fresh_bind(c, &scope);
        int cslot = current_slot(c, scope, cf);
        Anf *t = norm_tail(e->u.if_.then, vname, k, c, scope);
        Anf *f = norm_tail(e->u.if_.els, vname, k, c, scope);
        Anf *iff = anf_if(c->a, atom_var_ds(cf, 0, cslot), t, f);
        return norm(e->u.if_.cond, cf, iff, c, scope);
    }
    case A_LET: {
        const char *name = e->u.let.name;
        int nslot = alloc_slot(c);
        Scope *s2 = scope_push(c, scope, name, c->curframe, nslot);
        if (e->u.let.bound->kind == A_FUN) {
            const Ast *fn = e->u.let.bound;
            frame_enter(c);
            /* params first (runtime binds args to slots 0..nparams-1) */
            Scope *s3 = s2;
            for (int i = 0; i < fn->u.fun.nparams; i++) {
                int ps = alloc_slot(c);
                s3 = scope_push(c, s3, fn->u.fun.params[i], c->curframe, ps);
            }
            char *ret = fresh_bind(c, &s3);
            int retslot = current_slot(c, s3, ret);
            Anf *fbody = norm_tail(fn->u.fun.body, ret, anf_ret(c->a, atom_var_ds(ret, 0, retslot)), c, s3);
            int lam_nslots = c->frameslots[c->curframe];
            frame_exit(c);
            Anf *body = norm_tail(e->u.let.body, vname, k, c, s2);
            return anf_let(c->a, name, nslot, atom_lam(fn->u.fun.params, fn->u.fun.nparams, lam_nslots, fbody), body);
        }
        Atom batom;
        if (atomize(e->u.let.bound, &batom, c, s2)) {
            Anf *body = norm_tail(e->u.let.body, vname, k, c, s2);
            return anf_let(c->a, name, nslot, batom, body);
        }
        char *bf = fresh_bind(c, &s2);
        int bslot = current_slot(c, s2, bf);
        Anf *body = norm_tail(e->u.let.body, vname, k, c, s2);
        Anf *bind = anf_let(c->a, name, nslot, atom_var_ds(bf, 0, bslot), body);
        return norm(e->u.let.bound, bf, bind, c, s2);
    }
    case A_BINOP: {
        char *lf = fresh_bind(c, &scope);
        char *rf = fresh_bind(c, &scope);
        int lslot = current_slot(c, scope, lf);
        int rslot = current_slot(c, scope, rf);
        Atom *args = (Atom *)arena_alloc(c->a, 2 * sizeof(Atom));
        args[0] = atom_var_ds(lf, 0, lslot);
        args[1] = atom_var_ds(rf, 0, rslot);
        Anf *acc = anf_tail_call(c->a, atom_var(binop_prim_name(e->u.bin.op)), args, 2);
        Anf *r = norm(e->u.bin.rhs, rf, acc, c, scope);
        return norm(e->u.bin.lhs, lf, r, c, scope);
    }
    case A_NOT: {
        char *of = fresh_bind(c, &scope);
        int oslot = current_slot(c, scope, of);
        Atom *args = (Atom *)arena_alloc(c->a, sizeof(Atom));
        args[0] = atom_var_ds(of, 0, oslot);
        Anf *acc = anf_tail_call(c->a, atom_var("not"), args, 1);
        return norm(e->u.operand, of, acc, c, scope);
    }
    case A_PRINT: {
        char *of = fresh_bind(c, &scope);
        int oslot = current_slot(c, scope, of);
        if (e->u.print.nl) {
            char *nf = fresh_bind(c, &scope);
            int nslot = current_slot(c, scope, nf);
            Atom *args = (Atom *)arena_alloc(c->a, 2 * sizeof(Atom));
            args[0] = atom_var_ds(of, 0, oslot);
            args[1] = atom_var_ds(nf, 0, nslot);
            Anf *acc = anf_tail_call(c->a, atom_var("print"), args, 2);
            Anf *npart = norm(e->u.print.nl, nf, acc, c, scope);
            return norm(e->u.print.val, of, npart, c, scope);
        }
        Atom *args = (Atom *)arena_alloc(c->a, sizeof(Atom));
        args[0] = atom_var_ds(of, 0, oslot);
        Anf *acc = anf_tail_call(c->a, atom_var("print"), args, 1);
        return norm(e->u.print.val, of, acc, c, scope);
    }
    case A_CALLCC: {
        char *of = fresh_bind(c, &scope);
        int oslot = current_slot(c, scope, of);
        Anf *cc = anf_let_callcc(c->a, vname, vslot(c, scope, vname), atom_var_ds(of, 0, oslot), k);
        return norm(e->u.operand, of, cc, c, scope);
    }
    case A_THROW: {
        char *kf = fresh_bind(c, &scope);
        char *vf = fresh_bind(c, &scope);
        int kslot = current_slot(c, scope, kf);
        int vslot_ = current_slot(c, scope, vf);
        Anf *th = anf_tail_throw(c->a, atom_var_ds(kf, 0, kslot), atom_var_ds(vf, 0, vslot_));
        Anf *vpart = norm(e->u.thr.v, vf, th, c, scope);
        return norm(e->u.thr.k, kf, vpart, c, scope);
    }
    default:
        return NULL;
    }
}

/* -- non-tail position: eval `e`, bind result to vname, then run k -- */

static Anf *norm(const Ast *e, const char *vname, Anf *k, NrmCtx *c, Scope *scope) {
    Atom atom;
    if (atomize(e, &atom, c, scope)) return anf_let(c->a, vname, vslot(c, scope, vname), atom, k);

    switch (e->kind) {
    case A_APP: {
        int nargs = e->u.app.nargs;
        if (nargs == 0) return norm(e->u.app.fn, vname, k, c, scope);
        Atom hat;
        bool h_atom = atomize(e->u.app.fn, &hat, c, scope);
        char *headf = h_atom ? NULL : fresh_bind(c, &scope);
        int hslot = h_atom ? -1 : current_slot(c, scope, headf);
        Atom *args = (Atom *)arena_alloc(c->a, (size_t)nargs * sizeof(Atom));
        Anf *acc = anf_let_call(c->a, vname, vslot(c, scope, vname), h_atom ? hat : atom_var_ds(headf, 0, hslot), args, nargs, k);
        for (int i = nargs - 1; i >= 0; i--) {
            Atom a;
            if (atomize(e->u.app.args[i], &a, c, scope)) {
                args[i] = a;
            } else {
                char *af = fresh_bind(c, &scope);
                int aslot = current_slot(c, scope, af);
                args[i] = atom_var_ds(af, 0, aslot);
                acc = norm(e->u.app.args[i], af, acc, c, scope);
            }
        }
        if (!h_atom) return norm(e->u.app.fn, headf, acc, c, scope);
        return acc;
    }
    case A_IF: {
        Atom cond;
        if (atomize(e->u.if_.cond, &cond, c, scope)) {
            Anf *t = norm(e->u.if_.then, vname, k, c, scope);
            Anf *f = norm(e->u.if_.els, vname, k, c, scope);
            return anf_if(c->a, cond, t, f);
        }
        char *cf = fresh_bind(c, &scope);
        int cslot = current_slot(c, scope, cf);
        Anf *t = norm(e->u.if_.then, vname, k, c, scope);
        Anf *f = norm(e->u.if_.els, vname, k, c, scope);
        Anf *iff = anf_if(c->a, atom_var_ds(cf, 0, cslot), t, f);
        return norm(e->u.if_.cond, cf, iff, c, scope);
    }
    case A_LET: {
        const char *name = e->u.let.name;
        int nslot = alloc_slot(c);
        Scope *s2 = scope_push(c, scope, name, c->curframe, nslot);
        if (e->u.let.bound->kind == A_FUN) {
            const Ast *fn = e->u.let.bound;
            frame_enter(c);
            /* params first (runtime binds args to slots 0..nparams-1) */
            Scope *s3 = s2;
            for (int i = 0; i < fn->u.fun.nparams; i++) {
                int ps = alloc_slot(c);
                s3 = scope_push(c, s3, fn->u.fun.params[i], c->curframe, ps);
            }
            char *ret = fresh_bind(c, &s3);
            int retslot = current_slot(c, s3, ret);
            Anf *fbody = norm_tail(fn->u.fun.body, ret, anf_ret(c->a, atom_var_ds(ret, 0, retslot)), c, s3);
            int lam_nslots = c->frameslots[c->curframe];
            frame_exit(c);
            Anf *body = norm(e->u.let.body, vname, k, c, s2);
            return anf_let(c->a, name, nslot, atom_lam(fn->u.fun.params, fn->u.fun.nparams, lam_nslots, fbody), body);
        }
        Atom batom;
        if (atomize(e->u.let.bound, &batom, c, s2)) {
            Anf *body = norm(e->u.let.body, vname, k, c, s2);
            return anf_let(c->a, name, nslot, batom, body);
        }
        char *bf = fresh_bind(c, &s2);
        int bslot = current_slot(c, s2, bf);
        Anf *body = norm(e->u.let.body, vname, k, c, s2);
        Anf *bind = anf_let(c->a, name, nslot, atom_var_ds(bf, 0, bslot), body);
        return norm(e->u.let.bound, bf, bind, c, s2);
    }
    case A_BINOP: {
        char *lf = fresh_bind(c, &scope);
        char *rf = fresh_bind(c, &scope);
        int lslot = current_slot(c, scope, lf);
        int rslot = current_slot(c, scope, rf);
        Atom *args = (Atom *)arena_alloc(c->a, 2 * sizeof(Atom));
        args[0] = atom_var_ds(lf, 0, lslot);
        args[1] = atom_var_ds(rf, 0, rslot);
        Anf *call = anf_let_call(c->a, vname, vslot(c, scope, vname), atom_var(binop_prim_name(e->u.bin.op)), args, 2, k);
        Anf *r = norm(e->u.bin.rhs, rf, call, c, scope);
        return norm(e->u.bin.lhs, lf, r, c, scope);
    }
    case A_NOT: {
        char *of = fresh_bind(c, &scope);
        int oslot = current_slot(c, scope, of);
        Atom *args = (Atom *)arena_alloc(c->a, sizeof(Atom));
        args[0] = atom_var_ds(of, 0, oslot);
        Anf *call = anf_let_call(c->a, vname, vslot(c, scope, vname), atom_var("not"), args, 1, k);
        return norm(e->u.operand, of, call, c, scope);
    }
    case A_PRINT: {
        char *of = fresh_bind(c, &scope);
        int oslot = current_slot(c, scope, of);
        if (e->u.print.nl) {
            char *nf = fresh_bind(c, &scope);
            int nslot = current_slot(c, scope, nf);
            Atom *args = (Atom *)arena_alloc(c->a, 2 * sizeof(Atom));
            args[0] = atom_var_ds(of, 0, oslot);
            args[1] = atom_var_ds(nf, 0, nslot);
            Anf *call = anf_let_call(c->a, vname, vslot(c, scope, vname), atom_var("print"), args, 2, k);
            Anf *npart = norm(e->u.print.nl, nf, call, c, scope);
            return norm(e->u.print.val, of, npart, c, scope);
        }
        Atom *args = (Atom *)arena_alloc(c->a, sizeof(Atom));
        args[0] = atom_var_ds(of, 0, oslot);
        Anf *call = anf_let_call(c->a, vname, vslot(c, scope, vname), atom_var("print"), args, 1, k);
        return norm(e->u.print.val, of, call, c, scope);
    }
    case A_CALLCC: {
        char *of = fresh_bind(c, &scope);
        int oslot = current_slot(c, scope, of);
        Anf *cc = anf_let_callcc(c->a, vname, vslot(c, scope, vname), atom_var_ds(of, 0, oslot), k);
        return norm(e->u.operand, of, cc, c, scope);
    }
    case A_THROW: {
        char *kf = fresh_bind(c, &scope);
        char *vf = fresh_bind(c, &scope);
        int kslot = current_slot(c, scope, kf);
        int vslot_ = current_slot(c, scope, vf);
        Anf *th = anf_tail_throw(c->a, atom_var_ds(kf, 0, kslot), atom_var_ds(vf, 0, vslot_));
        Anf *vpart = norm(e->u.thr.v, vf, th, c, scope);
        return norm(e->u.thr.k, kf, vpart, c, scope);
    }
    default:
        return NULL;
    }
}

static bool atomize(const Ast *e, Atom *out, NrmCtx *c, Scope *scope) {
    switch (e->kind) {
    case A_INT:
        *out = atom_lit(v_int(e->u.ival));
        return true;
    case A_BIG: {
        Bignum *bg = bignum_from_dec_arena(c->a, e->u.sval);
        *out = bg ? atom_lit(v_big(bg)) : atom_lit(v_int(0));
        return true;
    }
    case A_FLOAT:
        *out = atom_lit(v_float(e->u.fval));
        return true;
    case A_BOOL:
        *out = atom_lit(v_bool(e->u.bval));
        return true;
    case A_UNIT:
        *out = atom_lit(v_unit());
        return true;
    case A_STR:
        *out = atom_lit(v_str(c->a, e->u.sval));
        return true;
    case A_LIST:
        /* non-empty lists are desugared into cons calls by the parser; only
         * the empty literal reaches normalization */
        if (e->u.list.n == 0) {
            *out = atom_lit(v_list_arena(c->a, NULL, 0));
            return true;
        }
        nerr(c, "%d:%d: internal: non-empty list literal reached normalization",
             e->line, e->col);
        return false;
    case A_VAR: {
        if (!in_scope(c, scope, e->u.name) && !prim_lookup(e->u.name)) {
            nerr(c, "%d:%d: unbound variable '%s'", e->line, e->col, e->u.name);
            return false;
        }
        for (Scope *p = scope; p; p = p->prev) {
            if (strcmp(p->name, e->u.name) == 0) {
                *out = atom_var_ds(e->u.name, c->curframe - p->frame, p->slot);
                return true;
            }
        }
        /* primitive name: resolved at run time by name, not a frame slot */
        *out = atom_var_ds(e->u.name, 0, -1);
        return true;
    }
    case A_FUN: {
        frame_enter(c);
        Scope *s2 = scope;
        /* params first (runtime binds args to slots 0..nparams-1) */
        for (int i = 0; i < e->u.fun.nparams; i++) {
            int ps = alloc_slot(c);
            s2 = scope_push(c, s2, e->u.fun.params[i], c->curframe, ps);
        }
        char *ret = fresh_bind(c, &s2);
        int retslot = current_slot(c, s2, ret);
        Anf *body = norm_tail(e->u.fun.body, ret, anf_ret(c->a, atom_var_ds(ret, 0, retslot)), c, s2);
        int nslots = c->frameslots[c->curframe];
        frame_exit(c);
        *out = atom_lam(e->u.fun.params, e->u.fun.nparams, nslots, body);
        return true;
    }
    default:
        return false;
    }
}

static bool ast_to_anf_impl(const Ast *prog, Arena *a, Anf **out, int *top_nslots,
                            char **errmsg, const char *const *pre_names, int pre_n) {
    NrmCtx c = {a, 0, NULL, 0, NULL, 0};
    c.capframes = 16;
    c.frameslots = (int *)malloc((size_t)c.capframes * sizeof(int));
    c.frameslots[0] = pre_n;
    Scope *scope = NULL;
    for (int i = 0; i < pre_n; i++) scope = scope_push(&c, scope, pre_names[i], 0, i);
    char *result = fresh_bind(&c, &scope);
    int rslot = current_slot(&c, scope, result);
    Anf *anf = norm_tail(prog, result, anf_ret(a, atom_var_ds(result, 0, rslot)), &c, scope);
    if (c.error) {
        *out = NULL;
        if (top_nslots) *top_nslots = 0;
        if (errmsg) *errmsg = c.error;
        return false;
    }
    if (top_nslots) *top_nslots = c.frameslots[0];
    *out = anf;
    return true;
}

bool ast_to_anf(const Ast *prog, Arena *a, Anf **out, int *top_nslots,
                char **errmsg) {
    return ast_to_anf_impl(prog, a, out, top_nslots, errmsg, NULL, 0);
}

bool ast_to_anf_prelude(const Ast *prog, Arena *a, Anf **out, int *top_nslots,
                        char **errmsg, const char *const *pre_names, int pre_n) {
    return ast_to_anf_impl(prog, a, out, top_nslots, errmsg, pre_names, pre_n);
}

/* ---- dump ---- */

static void print_atom(const Atom *a, int depth) {
    for (int i = 0; i < depth; i++) printf("  ");
    switch (a->kind) {
    case AT_VAR:
        printf("var %s(%d,%d)\n", a->u.var.name, a->u.var.depth, a->u.var.slot);
        break;
    case AT_LIT: {
        char *s = value_to_string(NULL, a->u.lit);
        printf("lit %s\n", s ? s : "<str>");
        break;
    }
    case AT_LAM: {
        printf("lam %d (", a->u.lam.nslots);
        for (int i = 0; i < a->u.lam.nparams; i++) printf("%s%s", i ? ", " : "", a->u.lam.params[i]);
        printf(")\n");
        anf_dump(a->u.lam.body, depth + 1);
        break;
    }
    }
}

void anf_dump(const Anf *node, int depth) {
    if (!node) return;
    for (int i = 0; i < depth; i++) printf("  ");
    switch (node->kind) {
    case N_LET:
        printf("let %s(%d) =\n", node->u.let.name, node->u.let.slot);
        print_atom(&node->u.let.atom, depth + 1);
        anf_dump(node->u.let.body, depth + 1);
        break;
    case N_LET_CALL:
        printf("let %s(%d) = call\n", node->u.call.name, node->u.call.slot);
        print_atom(&node->u.call.head, depth + 1);
        for (int i = 0; i < node->u.call.nargs; i++) print_atom(&node->u.call.args[i], depth + 1);
        anf_dump(node->u.call.body, depth + 1);
        break;
    case N_IF:
        printf("if\n");
        print_atom(&node->u.if_.cond, depth + 1);
        printf("then:\n");
        anf_dump(node->u.if_.then, depth + 1);
        printf("else:\n");
        anf_dump(node->u.if_.els, depth + 1);
        break;
    case N_TAIL_CALL:
        printf("tail-call\n");
        print_atom(&node->u.tailcall.head, depth + 1);
        for (int i = 0; i < node->u.tailcall.nargs; i++) print_atom(&node->u.tailcall.args[i], depth + 1);
        break;
    case N_RETURN:
        printf("return\n");
        print_atom(&node->u.ret, depth + 1);
        break;
    case N_LET_CALLCC:
        printf("let %s(%d) = callcc\n", node->u.callcc.name, node->u.callcc.slot);
        print_atom(&node->u.callcc.atom, depth + 1);
        anf_dump(node->u.callcc.body, depth + 1);
        break;
    case N_TAIL_THROW:
        printf("throw\n");
        print_atom(&node->u.tailthrow.k, depth + 1);
        print_atom(&node->u.tailthrow.v, depth + 1);
        break;
    }
}