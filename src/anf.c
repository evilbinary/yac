#include "anf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Scope {
    const char *name;
    struct Scope *prev;
} Scope;

typedef struct {
    Arena *a;
    int fresh;
    char *error;
} NrmCtx;

static Scope *scope_push(NrmCtx *c, Scope *s, const char *name) {
    Scope *n = (Scope *)arena_alloc(c->a, sizeof(Scope));
    n->name = name;
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

static char *fresh(NrmCtx *c) {
    char buf[32];
    snprintf(buf, sizeof(buf), "#t%d", c->fresh++);
    return arena_strdup(c->a, buf);
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
    return at;
}

Atom atom_lit(Value v) {
    Atom at;
    at.kind = AT_LIT;
    at.u.lit = v;
    return at;
}

Atom atom_lam(char **params, int nparams, Anf *body) {
    Atom at;
    at.kind = AT_LAM;
    at.u.lam.params = params;
    at.u.lam.nparams = nparams;
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

Anf *anf_let(Arena *a, const char *name, Atom atom, Anf *body) {
    Anf *n = anf_node(a, N_LET, 0);
    n->u.let.name = name;
    n->u.let.atom = atom;
    n->u.let.body = body;
    return n;
}

Anf *anf_let_call(Arena *a, const char *name, Atom head, Atom *args, int nargs, Anf *body) {
    Anf *n = anf_node(a, N_LET_CALL, 0);
    n->u.call.name = name;
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

Anf *anf_let_callcc(Arena *a, const char *name, Atom atom, Anf *body) {
    Anf *n = anf_node(a, N_LET_CALLCC, 0);
    n->u.callcc.name = name;
    n->u.callcc.atom = atom;
    n->u.callcc.body = body;
    return n;
}

static Anf *anf_tail_throw(Arena *a, Atom k, Atom v) {
    Anf *n = anf_node(a, N_TAIL_THROW, 0);
    n->u.tailthrow.k = k;
    n->u.tailthrow.v = v;
    return n;
}

/* ---- atomize: is the AST node atomic? ---- */

static bool atomize(const Ast *e, Atom *out, NrmCtx *c, Scope *scope);

static Anf *norm(const Ast *e, const char *vname, Anf *k, NrmCtx *c, Scope *scope);
static Anf *norm_tail(const Ast *e, const char *vname, Anf *k, NrmCtx *c, Scope *scope);

/* -- tail position: the value of `e` is the answer, routed through
 *    (vname, k) only for non-jump results. A tail call/prim discards
 *    (vname, k): the value flows back through the caller's frame. -- */

static Anf *norm_tail(const Ast *e, const char *vname, Anf *k, NrmCtx *c, Scope *scope) {
    Atom atom;
    if (atomize(e, &atom, c, scope)) return anf_let(c->a, vname, atom, k);

    switch (e->kind) {
    case A_APP: {
        int nargs = e->u.app.nargs;
        if (nargs == 0) return norm_tail(e->u.app.fn, vname, k, c, scope);
        char *headf = fresh(c);
        char **argf = (char **)arena_alloc(c->a, (size_t)nargs * sizeof(char *));
        Atom *args = (Atom *)arena_alloc(c->a, (size_t)nargs * sizeof(Atom));
        for (int i = 0; i < nargs; i++) {
            argf[i] = fresh(c);
            args[i] = atom_var(argf[i]);
        }
        Anf *acc = anf_tail_call(c->a, atom_var(headf), args, nargs);
        for (int i = nargs - 1; i >= 0; i--) {
            acc = norm(e->u.app.args[i], argf[i], acc, c, scope);
        }
        return norm(e->u.app.fn, headf, acc, c, scope);
    }
    case A_IF: {
        Atom cond;
        if (atomize(e->u.if_.cond, &cond, c, scope)) {
            Anf *t = norm_tail(e->u.if_.then, vname, k, c, scope);
            Anf *f = norm_tail(e->u.if_.els, vname, k, c, scope);
            return anf_if(c->a, cond, t, f);
        }
        char *cf = fresh(c);
        Anf *t = norm_tail(e->u.if_.then, vname, k, c, scope);
        Anf *f = norm_tail(e->u.if_.els, vname, k, c, scope);
        Anf *iff = anf_if(c->a, atom_var(cf), t, f);
        return norm(e->u.if_.cond, cf, iff, c, scope);
    }
    case A_LET: {
        const char *name = e->u.let.name;
        Scope *s2 = scope_push(c, scope, name);
        if (e->u.let.bound->kind == A_FUN) {
            const Ast *fn = e->u.let.bound;
            char *ret = fresh(c);
            Scope *s3 = s2;
            for (int i = 0; i < fn->u.fun.nparams; i++) s3 = scope_push(c, s3, fn->u.fun.params[i]);
            Anf *fbody = norm_tail(fn->u.fun.body, ret, anf_ret(c->a, atom_var(ret)), c, s3);
            Anf *body = norm_tail(e->u.let.body, vname, k, c, s2);
            return anf_let(c->a, name, atom_lam(fn->u.fun.params, fn->u.fun.nparams, fbody), body);
        }
        Atom batom;
        if (atomize(e->u.let.bound, &batom, c, s2)) {
            Anf *body = norm_tail(e->u.let.body, vname, k, c, s2);
            return anf_let(c->a, name, batom, body);
        }
        char *bf = fresh(c);
        Anf *body = norm_tail(e->u.let.body, vname, k, c, s2);
        Anf *bind = anf_let(c->a, name, atom_var(bf), body);
        return norm(e->u.let.bound, bf, bind, c, scope);
    }
    case A_BINOP: {
        char *lf = fresh(c);
        char *rf = fresh(c);
        Atom *args = (Atom *)arena_alloc(c->a, 2 * sizeof(Atom));
        args[0] = atom_var(lf);
        args[1] = atom_var(rf);
        Anf *acc = anf_tail_call(c->a, atom_var(binop_prim_name(e->u.bin.op)), args, 2);
        Anf *r = norm(e->u.bin.rhs, rf, acc, c, scope);
        return norm(e->u.bin.lhs, lf, r, c, scope);
    }
    case A_NOT: {
        char *of = fresh(c);
        Atom *args = (Atom *)arena_alloc(c->a, sizeof(Atom));
        args[0] = atom_var(of);
        Anf *acc = anf_tail_call(c->a, atom_var("not"), args, 1);
        return norm(e->u.operand, of, acc, c, scope);
    }
    case A_PRINT: {
        char *of = fresh(c);
        Atom *args = (Atom *)arena_alloc(c->a, sizeof(Atom));
        args[0] = atom_var(of);
        Anf *acc = anf_tail_call(c->a, atom_var("print"), args, 1);
        return norm(e->u.operand, of, acc, c, scope);
    }
    case A_CALLCC: {
        char *of = fresh(c);
        Anf *cc = anf_let_callcc(c->a, vname, atom_var(of), k);
        return norm(e->u.operand, of, cc, c, scope);
    }
    case A_THROW: {
        char *kf = fresh(c);
        char *vf = fresh(c);
        Anf *th = anf_tail_throw(c->a, atom_var(kf), atom_var(vf));
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
    if (atomize(e, &atom, c, scope)) return anf_let(c->a, vname, atom, k);

    switch (e->kind) {
    case A_APP: {
        int nargs = e->u.app.nargs;
        if (nargs == 0) return norm(e->u.app.fn, vname, k, c, scope);
        char *headf = fresh(c);
        char **argf = (char **)arena_alloc(c->a, (size_t)nargs * sizeof(char *));
        Atom *args = (Atom *)arena_alloc(c->a, (size_t)nargs * sizeof(Atom));
        for (int i = 0; i < nargs; i++) {
            argf[i] = fresh(c);
            args[i] = atom_var(argf[i]);
        }
        Anf *call = anf_let_call(c->a, vname, atom_var(headf), args, nargs, k);
        Anf *acc = call;
        for (int i = nargs - 1; i >= 0; i--) {
            acc = norm(e->u.app.args[i], argf[i], acc, c, scope);
        }
        return norm(e->u.app.fn, headf, acc, c, scope);
    }
    case A_IF: {
        Atom cond;
        if (atomize(e->u.if_.cond, &cond, c, scope)) {
            Anf *t = norm(e->u.if_.then, vname, k, c, scope);
            Anf *f = norm(e->u.if_.els, vname, k, c, scope);
            return anf_if(c->a, cond, t, f);
        }
        char *cf = fresh(c);
        Anf *t = norm(e->u.if_.then, vname, k, c, scope);
        Anf *f = norm(e->u.if_.els, vname, k, c, scope);
        Anf *iff = anf_if(c->a, atom_var(cf), t, f);
        return norm(e->u.if_.cond, cf, iff, c, scope);
    }
    case A_LET: {
        const char *name = e->u.let.name;
        Scope *s2 = scope_push(c, scope, name);
        if (e->u.let.bound->kind == A_FUN) {
            const Ast *fn = e->u.let.bound;
            char *ret = fresh(c);
            Scope *s3 = s2;
            for (int i = 0; i < fn->u.fun.nparams; i++) s3 = scope_push(c, s3, fn->u.fun.params[i]);
            Anf *fbody = norm_tail(fn->u.fun.body, ret, anf_ret(c->a, atom_var(ret)), c, s3);
            Anf *body = norm(e->u.let.body, vname, k, c, s2);
            return anf_let(c->a, name, atom_lam(fn->u.fun.params, fn->u.fun.nparams, fbody), body);
        }
        Atom batom;
        if (atomize(e->u.let.bound, &batom, c, s2)) {
            Anf *body = norm(e->u.let.body, vname, k, c, s2);
            return anf_let(c->a, name, batom, body);
        }
        char *bf = fresh(c);
        Anf *body = norm(e->u.let.body, vname, k, c, s2);
        Anf *bind = anf_let(c->a, name, atom_var(bf), body);
        return norm(e->u.let.bound, bf, bind, c, scope);
    }
    case A_BINOP: {
        char *lf = fresh(c);
        char *rf = fresh(c);
        Atom *args = (Atom *)arena_alloc(c->a, 2 * sizeof(Atom));
        args[0] = atom_var(lf);
        args[1] = atom_var(rf);
        Anf *call = anf_let_call(c->a, vname, atom_var(binop_prim_name(e->u.bin.op)), args, 2, k);
        Anf *r = norm(e->u.bin.rhs, rf, call, c, scope);
        return norm(e->u.bin.lhs, lf, r, c, scope);
    }
    case A_NOT: {
        char *of = fresh(c);
        Atom *args = (Atom *)arena_alloc(c->a, sizeof(Atom));
        args[0] = atom_var(of);
        Anf *call = anf_let_call(c->a, vname, atom_var("not"), args, 1, k);
        return norm(e->u.operand, of, call, c, scope);
    }
    case A_PRINT: {
        char *of = fresh(c);
        Atom *args = (Atom *)arena_alloc(c->a, sizeof(Atom));
        args[0] = atom_var(of);
        Anf *call = anf_let_call(c->a, vname, atom_var("print"), args, 1, k);
        return norm(e->u.operand, of, call, c, scope);
    }
    case A_CALLCC: {
        char *of = fresh(c);
        Anf *cc = anf_let_callcc(c->a, vname, atom_var(of), k);
        return norm(e->u.operand, of, cc, c, scope);
    }
    case A_THROW: {
        char *kf = fresh(c);
        char *vf = fresh(c);
        Anf *th = anf_tail_throw(c->a, atom_var(kf), atom_var(vf));
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
    case A_VAR: {
        if (!in_scope(c, scope, e->u.name) && !prim_lookup(e->u.name)) {
            nerr(c, "%d:%d: unbound variable '%s'", e->line, e->col, e->u.name);
            return false;
        }
        *out = atom_var(e->u.name);
        return true;
    }
    case A_FUN: {
        char *ret = fresh(c);
        Scope *s2 = scope;
        for (int i = 0; i < e->u.fun.nparams; i++) s2 = scope_push(c, s2, e->u.fun.params[i]);
        Anf *body = norm_tail(e->u.fun.body, ret, anf_ret(c->a, atom_var(ret)), c, s2);
        *out = atom_lam(e->u.fun.params, e->u.fun.nparams, body);
        return true;
    }
    default:
        return false;
    }
}

bool ast_to_anf(const Ast *prog, Arena *a, Anf **out, char **errmsg) {
    NrmCtx c = {a, 0, NULL};
    char *result = fresh(&c);
    Anf *anf = norm_tail(prog, result, anf_ret(a, atom_var(result)), &c, NULL);
    if (c.error) {
        *out = NULL;
        if (errmsg) *errmsg = c.error;
        return false;
    }
    *out = anf;
    return true;
}

/* ---- dump ---- */

static void print_atom(const Atom *a, int depth) {
    for (int i = 0; i < depth; i++) printf("  ");
    switch (a->kind) {
    case AT_VAR: printf("var %s\n", a->u.var.name); break;
    case AT_LIT: {
        char *s = value_to_string(NULL, a->u.lit);
        printf("lit %s\n", s ? s : "<str>");
        break;
    }
    case AT_LAM: {
        printf("lam (");
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
        printf("let %s =\n", node->u.let.name);
        print_atom(&node->u.let.atom, depth + 1);
        anf_dump(node->u.let.body, depth + 1);
        break;
    case N_LET_CALL:
        printf("let %s = call\n", node->u.call.name);
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
        printf("let %s = callcc\n", node->u.callcc.name);
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