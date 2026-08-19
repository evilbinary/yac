#include "cps.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Arena *a;
    int fresh;
    char *error;
    int frames[64]; /* per-lambda-nesting-level slot counters */
    int depth;      /* current lambda nesting level */
} CpsCtx;

static char *cfresh(CpsCtx *c) {
    char buf[32];
    snprintf(buf, sizeof(buf), "#k%d", c->fresh++);
    return arena_strdup(c->a, buf);
}

static void cerror(CpsCtx *c, const char *fmt, ...) {
    if (c->error) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    c->error = arena_strdup(c->a, buf);
}

/* ---- CVal constructors ---- */

static CVal cv_var_ds(const char *name, int depth, int slot) {
    CVal v;
    v.kind = CV_VAR;
    v.u.var.name = name;
    v.u.var.depth = depth;
    v.u.var.slot = slot;
    return v;
}

static CVal cv_var(const char *name) {
    return cv_var_ds(name, 0, -1);
}

static CVal cv_lit(Value lit) {
    CVal v;
    v.kind = CV_LIT;
    v.u.lit = lit;
    return v;
}

static CVal cv_fun(char **params, int nparams, int nslots, int kslot, CExp *body) {
    CVal v;
    v.kind = CV_FUN;
    v.u.fun.params = params;
    v.u.fun.nparams = nparams;
    v.u.fun.nslots = nslots;
    v.u.fun.kslot = kslot;
    v.u.fun.body = body;
    return v;
}

static CVal cv_cont(const char *param, int rslot, CExp *body) {
    CVal v;
    v.kind = CV_CONT;
    v.u.cont.param = param;
    v.u.cont.rslot = rslot;
    v.u.cont.body = body;
    return v;
}

/* ---- CExp constructors ---- */

static CExp *ce_node(Arena *a, CExpKind kind, int line) {
    CExp *n = (CExp *)arena_alloc(a, sizeof(CExp));
    memset(n, 0, sizeof(CExp));
    n->kind = kind;
    n->line = line;
    return n;
}

static CExp *ce_let(Arena *a, const char *name, int slot, CVal val, CExp *body) {
    CExp *n = ce_node(a, CE_LET, 0);
    n->u.let.name = name;
    n->u.let.slot = slot;
    n->u.let.val = val;
    n->u.let.body = body;
    return n;
}

static CExp *ce_call(Arena *a, CVal head, CVal *args, int nargs, int line) {
    CExp *n = ce_node(a, CE_CALL, line);
    n->u.call.head = head;
    n->u.call.args = args;
    n->u.call.nargs = nargs;
    return n;
}

static CExp *ce_throw(Arena *a, CVal k, CVal v, int line) {
    CExp *n = ce_node(a, CE_THROW, line);
    n->u.throw_.k = k;
    n->u.throw_.v = v;
    return n;
}

static CExp *ce_if(Arena *a, CVal cond, CExp *then, CExp *els) {
    CExp *n = ce_node(a, CE_IF, 0);
    n->u.if_.cond = cond;
    n->u.if_.then = then;
    n->u.if_.els = els;
    return n;
}

static CExp *ce_halt(Arena *a, CVal v) {
    CExp *n = ce_node(a, CE_HALT, 0);
    n->u.halt.v = v;
    return n;
}

/* ---- conversion ---- */

static CVal conv_atom(const Atom *atom, CpsCtx *c);
static CExp *conv_ce(const Anf *node, CVal kcont, CpsCtx *c);

/* The continuation for a lambda: a fresh variable bound in the closure, kept
 * in a slot after the ANF locals. kcont flows through tail calls untouched,
 * giving TCO in CPS. */
static CVal conv_lambda(const Atom *lam, CpsCtx *c) {
    char *k = cfresh(c);
    char **params = (char **)arena_alloc(c->a,
        (size_t)(lam->u.lam.nparams + 1) * sizeof(char *));
    for (int i = 0; i < lam->u.lam.nparams; i++) params[i] = lam->u.lam.params[i];
    params[lam->u.lam.nparams] = k;
    c->depth++;
    c->frames[c->depth] = lam->u.lam.nslots + 1; /* +1 for the k slot */
    int kslot = lam->u.lam.nslots;
    CExp *body = conv_ce(lam->u.lam.body, cv_var_ds(k, 0, kslot), c);
    int nslots = c->frames[c->depth];
    c->depth--;
    return cv_fun(params, lam->u.lam.nparams + 1, nslots, kslot, body);
}

static CVal conv_atom(const Atom *atom, CpsCtx *c) {
    switch (atom->kind) {
    case AT_VAR:
        if (atom->u.var.slot < 0)
            return cv_var_ds(atom->u.var.name, 0, -1); /* primitive */
        return cv_var_ds(atom->u.var.name, atom->u.var.depth, atom->u.var.slot);
    case AT_LIT:
        return cv_lit(atom->u.lit);
    case AT_LAM:
        return conv_lambda(atom, c);
    }
    cerror(c, "internal: bad atom in CPS conversion");
    return cv_var("?");
}

/* Build args = [cv(a0)..cv(a_{n-1}), continuation]. */
static CVal *conv_args(const Atom *args, int nargs, CVal cont,
                       CpsCtx *c) {
    CVal *out = (CVal *)arena_alloc(c->a, (size_t)(nargs + 1) * sizeof(CVal));
    for (int i = 0; i < nargs; i++) out[i] = conv_atom(&args[i], c);
    out[nargs] = cont;
    return out;
}

static CExp *conv_ce(const Anf *node, CVal kcont, CpsCtx *c) {
    switch (node->kind) {
    case N_LET:
        return ce_let(c->a, node->u.let.name, node->u.let.slot,
                      conv_atom(&node->u.let.atom, c),
                      conv_ce(node->u.let.body, kcont, c));

    case N_LET_CALL: {
        /* let name = call(head, args) in body
         *  == call head args (kappa(r). let name = r in conv(body, kcont))
         * The continuation's param r lives in a slot (rslot) of the host
         * frame; applying the continuation writes the value there and keeps
         * the current frame. */
        char *r = cfresh(c);
        int rslot = c->frames[c->depth]++;
        CExp *cont_body = ce_let(c->a, node->u.call.name, node->u.call.slot,
                                 cv_var_ds(r, 0, rslot),
                                 conv_ce(node->u.call.body, kcont, c));
        CVal cont = cv_cont(r, rslot, cont_body);
        CVal *args = conv_args(node->u.call.args, node->u.call.nargs, cont, c);
        return ce_call(c->a, conv_atom(&node->u.call.head, c), args,
                       node->u.call.nargs + 1, node->line);
    }

    case N_IF:
        return ce_if(c->a, conv_atom(&node->u.if_.cond, c),
                     conv_ce(node->u.if_.then, kcont, c),
                     conv_ce(node->u.if_.els, kcont, c));

    case N_TAIL_CALL: {
        /* tail call: continuation is the ambient one, passed straight through */
        CVal *args = conv_args(node->u.tailcall.args, node->u.tailcall.nargs,
                               kcont, c);
        return ce_call(c->a, conv_atom(&node->u.tailcall.head, c), args,
                       node->u.tailcall.nargs + 1, node->line);
    }

    case N_RETURN: {
        /* apply the ambient continuation to the value */
        CVal *args = (CVal *)arena_alloc(c->a, sizeof(CVal));
        args[0] = conv_atom(&node->u.ret, c);
        return ce_call(c->a, kcont, args, 1, node->line);
    }

    case N_LET_CALLCC: {
        /* let name = callcc(atom) in body
         *  == call atom k cont
         *  where  k    = kappa(v). throw kcont v
         *         cont = kappa(r). let name = r in conv(body, kcont) */
        char *ck = cfresh(c);
        char *cr = cfresh(c);
        int kslot = c->frames[c->depth]++;
        int cslot = c->frames[c->depth]++;
        CExp *capture = ce_throw(c->a, kcont, cv_var_ds(ck, 0, kslot), node->line);
        CVal k = cv_cont(ck, kslot, capture);
        CExp *rest = ce_let(c->a, node->u.callcc.name, node->u.callcc.slot,
                            cv_var_ds(cr, 0, cslot),
                            conv_ce(node->u.callcc.body, kcont, c));
        CVal cont = cv_cont(cr, cslot, rest);
        CVal *args = (CVal *)arena_alloc(c->a, 2 * sizeof(CVal));
        args[0] = k;
        args[1] = cont;
        return ce_call(c->a, conv_atom(&node->u.callcc.atom, c), args, 2,
                       node->line);
    }

    case N_TAIL_THROW:
        return ce_throw(c->a, conv_atom(&node->u.tailthrow.k, c),
                        conv_atom(&node->u.tailthrow.v, c), node->line);
    }
    cerror(c, "internal: bad ANF node in CPS conversion");
    return NULL;
}

bool anf_to_cps(const Anf *anf, int anf_top_nslots, Arena *a, CExp **out,
                int *top_nslots, char **errmsg) {
    CpsCtx c = {a, 0, NULL, {0}, 0};
    c.frames[0] = anf_top_nslots;
    char *top = cfresh(&c);      /* name bound to the halt continuation */
    int top_slot = c.frames[0]++;
    char *p = cfresh(&c);        /* halt continuation's parameter */
    int pslot = c.frames[0]++;
    CVal halt = cv_cont(p, pslot, ce_halt(a, cv_var_ds(p, 0, pslot)));
    CExp *body = conv_ce(anf, cv_var_ds(top, 0, top_slot), &c);
    if (c.error) {
        *out = NULL;
        if (top_nslots) *top_nslots = 0;
        if (errmsg) *errmsg = c.error;
        return false;
    }
    if (top_nslots) *top_nslots = c.frames[0];
    *out = ce_let(a, top, top_slot, halt, body);
    return true;
}

/* ---- simplification (--opt) ---- */

typedef struct CE { const char *name; Value v; const struct CE *next; } CE;

typedef struct { Arena *a; } OptCtx;

static const CE *ce_push(OptCtx *c, const CE *prev, const char *name, Value v) {
    CE *e = (CE *)arena_alloc(c->a, sizeof(CE));
    e->name = name;
    e->v = v;
    e->next = prev;
    return e;
}

static bool ce_find(const CE *ce, const char *name, Value *out) {
    for (; ce; ce = ce->next)
        if (strcmp(ce->name, name) == 0) { *out = ce->v; return true; }
    return false;
}

/* const env with the given parameter names removed (shadowing) */
static const CE *ce_without(OptCtx *c, const CE *ce, char *const *params, int n) {
    const CE *out = NULL;
    for (; ce; ce = ce->next) {
        int shadowed = 0;
        for (int i = 0; i < n; i++)
            if (strcmp(ce->name, params[i]) == 0) { shadowed = 1; break; }
        if (!shadowed) out = ce_push(c, out, ce->name, ce->v);
    }
    return out;
}

static CExp *opt(const CExp *node, const CE *ce, OptCtx *c);

static CVal opt_cval(CVal v, const char *letname, const CE *ce, OptCtx *c) {
    switch (v.kind) {
    case CV_FUN: {
        /* eta-reduce: lambda(x*, k). call f x* k  ==>  f */
        const char *target = NULL;
        int np = v.u.fun.nparams;
        const CExp *b = v.u.fun.body;
        if (np >= 1 && b && b->kind == CE_CALL && b->u.call.nargs == np &&
            b->u.call.head.kind == CV_VAR) {
            const char *f = b->u.call.head.u.var.name;
            const char *k = v.u.fun.params[np - 1];
            int ok = (letname == NULL || strcmp(f, letname) != 0) &&
                     strcmp(f, k) != 0;
            for (int i = 0; ok && i < np; i++)
                if (strcmp(f, v.u.fun.params[i]) == 0) ok = 0;
            for (int i = 0; ok && i < np; i++) {
                CVal a = b->u.call.args[i];
                if (a.kind != CV_VAR || strcmp(a.u.var.name, v.u.fun.params[i]) != 0) ok = 0;
            }
            if (ok) target = f;
        }
        if (target) {
            /* move the reference out of the lambda frame (depth - 1) */
            int df = b->u.call.head.u.var.depth;
            int sf = b->u.call.head.u.var.slot;
            return cv_var_ds(target, df - 1, sf);
        }
        const CE *inner = ce_without(c, ce, v.u.fun.params, np);
        v.u.fun.body = opt(v.u.fun.body, inner, c);
        return v;
    }
    case CV_CONT: {
        /* eta-reduce: kappa(x). call k x  ==>  k
         * (continuations add no frame, so k's address is unchanged) */
        const char *target = NULL;
        const CExp *b = v.u.cont.body;
        if (b && b->kind == CE_CALL && b->u.call.nargs == 1 &&
            b->u.call.head.kind == CV_VAR) {
            const char *k = b->u.call.head.u.var.name;
            CVal a0 = b->u.call.args[0];
            if (a0.kind == CV_VAR && strcmp(a0.u.var.name, v.u.cont.param) == 0 &&
                strcmp(k, v.u.cont.param) != 0 &&
                (letname == NULL || strcmp(k, letname) != 0)) {
                target = k;
            }
        }
        if (target) return b->u.call.head;
        const CE *inner = ce_without(c, ce, (char *[]){ (char *)v.u.cont.param }, 1);
        v.u.cont.body = opt(v.u.cont.body, inner, c);
        return v;
    }
    default:
        return v;
    }
}

/* Fold  call <pure prim> a* cont  ==>  call cont (result)  when every
 * argument a is a literal (or a known constant via the const env). */
static CExp *fold_call(CExp *call, const CE *ce, OptCtx *c) {
    CVal *args = call->u.call.args;
    int n = call->u.call.nargs;
    if (n < 1) return call;
    const Prim *p = NULL;
    if (call->u.call.head.kind == CV_PRIM) {
        p = prim_lookup(call->u.call.head.u.prim.name);
    } else if (call->u.call.head.kind == CV_VAR) {
        /* the converter emits primitives as variables; resolve by name */
        p = prim_lookup(call->u.call.head.u.var.name);
    }
    if (!p || !p->pure || p->needs_gc) return call; /* needs_gc: cannot fold at compile time */
    int na = n - 1;
    if (p->arity >= 0 && p->arity != na) return call;
    if (na > 8) return call;
    Value lits[8];
    for (int i = 0; i < na; i++) {
        if (args[i].kind == CV_LIT) lits[i] = args[i].u.lit;
        else if (args[i].kind == CV_VAR && ce_find(ce, args[i].u.var.name, &lits[i])) { /* ok */ }
        else return call;
    }
    PrimCtx pctx = {false, "", NULL, NULL, NULL};
    Value r = p->fn(lits, na, &pctx);
    if (pctx.errored) return call; /* e.g. division by zero: keep it dynamic */
    CVal *nargs = (CVal *)arena_alloc(c->a, sizeof(CVal));
    nargs[0] = cv_lit(r);
    return ce_call(c->a, args[n - 1], nargs, 1, call->line);
}

static CExp *opt(const CExp *node, const CE *ce, OptCtx *c) {
    switch (node->kind) {
    case CE_LET: {
        CVal v = opt_cval(node->u.let.val, node->u.let.name, ce, c);
        const CE *ce2 = ce;
        if (v.kind == CV_LIT) ce2 = ce_push(c, ce, node->u.let.name, v.u.lit);
        CExp *body = opt(node->u.let.body, ce2, c);
        return ce_let(c->a, node->u.let.name, node->u.let.slot, v, body);
    }
    case CE_CALL: {
        CVal head = opt_cval(node->u.call.head, NULL, ce, c);
        CVal *args = (CVal *)arena_alloc(c->a, (size_t)node->u.call.nargs * sizeof(CVal));
        for (int i = 0; i < node->u.call.nargs; i++)
            args[i] = opt_cval(node->u.call.args[i], NULL, ce, c);
        CExp *nc = ce_call(c->a, head, args, node->u.call.nargs, node->line);
        return fold_call(nc, ce, c);
    }
    case CE_THROW: {
        CVal k = opt_cval(node->u.throw_.k, NULL, ce, c);
        CVal v = opt_cval(node->u.throw_.v, NULL, ce, c);
        return ce_throw(c->a, k, v, node->line);
    }
    case CE_IF: {
        CVal cond = opt_cval(node->u.if_.cond, NULL, ce, c);
        CExp *t = opt(node->u.if_.then, ce, c);
        CExp *e = opt(node->u.if_.els, ce, c);
        return ce_if(c->a, cond, t, e);
    }
    case CE_HALT: {
        CVal v = opt_cval(node->u.halt.v, NULL, ce, c);
        return ce_halt(c->a, v);
    }
    }
    return NULL; /* unreachable */
}

CExp *cps_simplify(const CExp *prog, Arena *a) {
    OptCtx c = {a};
    return opt(prog, NULL, &c);
}

/* ---- dump ---- */

static void print_cval(const CVal *v, int depth) {
    for (int i = 0; i < depth; i++) printf("  ");
    switch (v->kind) {
    case CV_VAR: printf("var %s(%d,%d)\n", v->u.var.name, v->u.var.depth, v->u.var.slot); break;
    case CV_PRIM: printf("prim %s\n", v->u.prim.name); break;
    case CV_LIT: {
        char *s = value_to_string(NULL, v->u.lit);
        printf("lit %s\n", s ? s : "<str>");
        break;
    }
    case CV_FUN: {
        printf("fun %d (", v->u.fun.nslots);
        for (int i = 0; i < v->u.fun.nparams; i++) {
            printf("%s%s", i ? ", " : "", v->u.fun.params[i]);
        }
        printf(")\n");
        cps_dump(v->u.fun.body, depth + 1);
        break;
    }
    case CV_CONT: {
        printf("cont (%s, slot %d)\n", v->u.cont.param, v->u.cont.rslot);
        cps_dump(v->u.cont.body, depth + 1);
        break;
    }
    }
}

void cps_dump(const CExp *node, int depth) {
    if (!node) return;
    for (int i = 0; i < depth; i++) printf("  ");
    switch (node->kind) {
    case CE_LET:
        printf("let %s(%d) =\n", node->u.let.name, node->u.let.slot);
        print_cval(&node->u.let.val, depth + 1);
        cps_dump(node->u.let.body, depth + 1);
        break;
    case CE_CALL:
        printf("call\n");
        print_cval(&node->u.call.head, depth + 1);
        for (int i = 0; i < node->u.call.nargs; i++) print_cval(&node->u.call.args[i], depth + 1);
        break;
    case CE_THROW:
        printf("throw\n");
        print_cval(&node->u.throw_.k, depth + 1);
        print_cval(&node->u.throw_.v, depth + 1);
        break;
    case CE_IF:
        printf("if\n");
        print_cval(&node->u.if_.cond, depth + 1);
        printf("then:\n");
        cps_dump(node->u.if_.then, depth + 1);
        printf("else:\n");
        cps_dump(node->u.if_.els, depth + 1);
        break;
    case CE_HALT:
        printf("halt\n");
        print_cval(&node->u.halt.v, depth + 1);
        break;
    }
}