#include "cps.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Arena *a;
    int fresh;
    char *error;
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

static CVal cv_var(const char *name) {
    CVal v;
    v.kind = CV_VAR;
    v.u.var.name = name;
    return v;
}

static CVal cv_lit(Value lit) {
    CVal v;
    v.kind = CV_LIT;
    v.u.lit = lit;
    return v;
}

static CVal cv_fun(char **params, int nparams, CExp *body) {
    CVal v;
    v.kind = CV_FUN;
    v.u.fun.params = params;
    v.u.fun.nparams = nparams;
    v.u.fun.body = body;
    return v;
}

static CVal cv_cont(const char *param, CExp *body) {
    CVal v;
    v.kind = CV_CONT;
    v.u.cont.param = param;
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

static CExp *ce_let(Arena *a, const char *name, CVal val, CExp *body) {
    CExp *n = ce_node(a, CE_LET, 0);
    n->u.let.name = name;
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

/* The continuation for a lambda: a fresh variable bound in the closure.
 * kcont flows through tail calls untouched, giving TCO in CPS. */
static CVal conv_lambda(const Atom *lam, CpsCtx *c) {
    char *k = cfresh(c);
    char **params = (char **)arena_alloc(c->a,
        (size_t)(lam->u.lam.nparams + 1) * sizeof(char *));
    for (int i = 0; i < lam->u.lam.nparams; i++) params[i] = lam->u.lam.params[i];
    params[lam->u.lam.nparams] = k;
    CExp *body = conv_ce(lam->u.lam.body, cv_var(k), c);
    return cv_fun(params, lam->u.lam.nparams + 1, body);
}

static CVal conv_atom(const Atom *atom, CpsCtx *c) {
    switch (atom->kind) {
    case AT_VAR:
        return cv_var(atom->u.var.name);
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
        return ce_let(c->a, node->u.let.name,
                      conv_atom(&node->u.let.atom, c),
                      conv_ce(node->u.let.body, kcont, c));

    case N_LET_CALL: {
        /* let name = call(head, args) in body
         *  == call head args (kappa(r). let name = r in conv(body, kcont)) */
        char *r = cfresh(c);
        CExp *cont_body = ce_let(c->a, node->u.call.name, cv_var(r),
                                 conv_ce(node->u.call.body, kcont, c));
        CVal cont = cv_cont(r, cont_body);
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
        CExp *capture = ce_throw(c->a, kcont, cv_var(ck), node->line);
        CVal k = cv_cont(ck, capture);
        CExp *rest = ce_let(c->a, node->u.callcc.name, cv_var(cr),
                            conv_ce(node->u.callcc.body, kcont, c));
        CVal cont = cv_cont(cr, rest);
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

bool anf_to_cps(const Anf *anf, Arena *a, CExp **out, char **errmsg) {
    CpsCtx c = {a, 0, NULL};
    char *top = cfresh(&c);      /* name bound to the halt continuation */
    char *p = cfresh(&c);        /* halt continuation's parameter */
    CVal halt = cv_cont(p, ce_halt(a, cv_var(p)));
    CExp *body = conv_ce(anf, cv_var(top), &c);
    if (c.error) {
        *out = NULL;
        if (errmsg) *errmsg = c.error;
        return false;
    }
    *out = ce_let(a, top, halt, body);
    return true;
}

/* ---- dump ---- */

static void print_cval(const CVal *v, int depth) {
    for (int i = 0; i < depth; i++) printf("  ");
    switch (v->kind) {
    case CV_VAR: printf("var %s\n", v->u.var.name); break;
    case CV_PRIM: printf("prim %s\n", v->u.prim.name); break;
    case CV_LIT: {
        char *s = value_to_string(NULL, v->u.lit);
        printf("lit %s\n", s ? s : "<str>");
        break;
    }
    case CV_FUN: {
        printf("fun (");
        for (int i = 0; i < v->u.fun.nparams; i++) {
            printf("%s%s", i ? ", " : "", v->u.fun.params[i]);
        }
        printf(")\n");
        cps_dump(v->u.fun.body, depth + 1);
        break;
    }
    case CV_CONT: {
        printf("cont (%s)\n", v->u.cont.param);
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
        printf("let %s =\n", node->u.let.name);
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