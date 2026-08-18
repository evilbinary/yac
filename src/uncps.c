#include "uncps.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Restricted CPS -> ANF by abstract interpretation over continuation
 * templates. Each continuation name is tracked as a "template" describing
 * what happens when that continuation receives a value:
 *
 *   CT_APP    - inline continuation kappa(param).C  ->  let param = v in unCPS(C)
 *   CT_RETURN - a function's own continuation       ->  return v
 *   CT_HALT   - the top-level halt continuation     ->  return v
 *
 * When a continuation appears in a non-continuation position (stored, passed
 * as a normal argument, or produced by callcc) the program cannot be
 * un-CPS-ed and we fail. */

typedef enum { CT_APP, CT_RETURN, CT_HALT } ContKind;

typedef struct Cont {
    ContKind kind;
    const char *param; /* CT_APP: continuation-bound name */
    const CExp *body;  /* CT_APP: continuation body (CPS) */
} Cont;

typedef struct Kenv {
    const char *name;
    Cont cont;
    struct Kenv *prev;
} Kenv;

typedef struct {
    Arena *a;
    char *error;
} Uc;

static void uerr(Uc *u, const char *fmt, ...) {
    if (u->error) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    u->error = arena_strdup(u->a, buf);
}

static Kenv *kenv_push(Arena *a, const Kenv *prev, const char *name, Cont cont) {
    Kenv *k = (Kenv *)arena_alloc(a, sizeof(Kenv));
    k->name = name;
    k->cont = cont;
    k->prev = prev;
    return k;
}

static bool kenv_lookup(const Kenv *k, const char *name, Cont *out) {
    for (; k; k = k->prev) {
        if (strcmp(k->name, name) == 0) {
            *out = k->cont;
            return true;
        }
    }
    return false;
}

static Anf *uncps(const CExp *C, const Kenv *kenv, Uc *u);

/* CVal -> Atom; fails (returns false, sets u->error) on continuation values. */
static bool cv_atom(const CVal *v, Uc *u, Atom *out) {
    switch (v->kind) {
    case CV_VAR:
        *out = atom_var(v->u.var.name);
        return true;
    case CV_LIT:
        *out = atom_lit(v->u.lit);
        return true;
    case CV_PRIM:
        *out = atom_var(v->u.prim.name);
        return true;
    case CV_FUN: {
        char **params = v->u.fun.params;
        int np = v->u.fun.nparams;
        const char *kname = params[np - 1];
        char **up = (char **)arena_alloc(u->a, (size_t)(np - 1) * sizeof(char *));
        for (int i = 0; i < np - 1; i++) up[i] = params[i];
        /* lambda bodies may not capture outer continuations: fresh kenv */
        Kenv *k2 = kenv_push(u->a, NULL, kname, (Cont){CT_RETURN, NULL, NULL});
        Anf *body = uncps(v->u.fun.body, k2, u);
        if (u->error) return false;
        *out = atom_lam(up, np - 1, body);
        return true;
    }
    case CV_CONT:
        uerr(u, "cannot un-CPS: continuation value used as a data value");
        return false;
    }
    uerr(u, "internal: bad CVal");
    return false;
}

/* Apply a continuation template to a value atom. */
static Anf *cont_apply(Uc *u, Cont cont, const Atom *val, const Kenv *kenv) {
    switch (cont.kind) {
    case CT_RETURN:
    case CT_HALT:
        return anf_ret(u->a, *val);
    case CT_APP:
        return anf_let(u->a, cont.param, *val, uncps(cont.body, kenv, u));
    }
    uerr(u, "internal: bad continuation template");
    return NULL;
}

/* Apply a continuation template to a call head(args). */
static Anf *cont_apply_call(Uc *u, Cont cont, const Atom *head, const Atom *args,
                            int nargs, const Kenv *kenv) {
    switch (cont.kind) {
    case CT_RETURN:
    case CT_HALT:
        return anf_tail_call(u->a, *head, (Atom *)args, nargs);
    case CT_APP:
        return anf_let_call(u->a, cont.param, *head, (Atom *)args, nargs,
                            uncps(cont.body, kenv, u));
    }
    uerr(u, "internal: bad continuation template");
    return NULL;
}

/* Reject continuations in non-continuation argument positions. */
static bool check_no_escape(const CVal *args, int nargs, const Kenv *kenv, Uc *u) {
    for (int i = 0; i < nargs; i++) {
        const CVal *a = &args[i];
        if (a->kind == CV_CONT) {
            uerr(u, "cannot un-CPS: continuation escapes (used as a value)");
            return false;
        }
        if (a->kind == CV_VAR) {
            Cont c;
            if (kenv_lookup(kenv, a->u.var.name, &c)) {
                uerr(u, "cannot un-CPS: continuation escapes (passed as an argument)");
                return false;
            }
        }
    }
    return true;
}

static Anf *uncps(const CExp *C, const Kenv *kenv, Uc *u) {
    switch (C->kind) {
    case CE_LET: {
        const CVal *val = &C->u.let.val;
        if (val->kind == CV_CONT) {
            Cont cont = {CT_APP, val->u.cont.param, val->u.cont.body};
            Kenv *k2 = kenv_push(u->a, kenv, C->u.let.name, cont);
            return uncps(C->u.let.body, k2, u);
        }
        if (val->kind == CV_VAR) {
            Cont cont;
            if (kenv_lookup(kenv, val->u.var.name, &cont)) {
                /* alias: name now names the same continuation */
                Kenv *k2 = kenv_push(u->a, kenv, C->u.let.name, cont);
                return uncps(C->u.let.body, k2, u);
            }
            Atom at;
            if (!cv_atom(val, u, &at)) return NULL;
            return anf_let(u->a, C->u.let.name, at, uncps(C->u.let.body, kenv, u));
        }
        Atom at;
        if (!cv_atom(val, u, &at)) return NULL;
        return anf_let(u->a, C->u.let.name, at, uncps(C->u.let.body, kenv, u));
    }

    case CE_CALL: {
        CVal head = C->u.call.head;
        int nargs = C->u.call.nargs;
        CVal *args = C->u.call.args;

        /* inline continuation applied directly: call kappa(x).C v */
        if (head.kind == CV_CONT) {
            if (nargs != 1) {
                uerr(u, "cannot un-CPS: continuation applied to %d value(s)", nargs);
                return NULL;
            }
            Atom val;
            if (!cv_atom(&args[0], u, &val)) return NULL;
            Cont cont = {CT_APP, head.u.cont.param, head.u.cont.body};
            return cont_apply(u, cont, &val, kenv);
        }

        /* applying a named continuation to a value: call k v */
        if (head.kind == CV_VAR) {
            Cont cont;
            if (kenv_lookup(kenv, head.u.var.name, &cont)) {
                if (nargs != 1) {
                    uerr(u, "cannot un-CPS: continuation applied to %d value(s)", nargs);
                    return NULL;
                }
                Atom val;
                if (!cv_atom(&args[0], u, &val)) return NULL;
                return cont_apply(u, cont, &val, kenv);
            }
        }

        if (nargs < 1) {
            uerr(u, "internal: call without any arguments");
            return NULL;
        }
        if (!check_no_escape(args, nargs - 1, kenv, u)) return NULL;

        Atom head_at;
        if (!cv_atom(&head, u, &head_at)) return NULL;

        Atom *aargs = (Atom *)arena_alloc(u->a, (size_t)(nargs - 1) * sizeof(Atom));
        for (int i = 0; i < nargs - 1; i++) {
            if (!cv_atom(&args[i], u, &aargs[i])) return NULL;
        }

        CVal *last = &args[nargs - 1];
        if (last->kind == CV_CONT) {
            Cont cont = {CT_APP, last->u.cont.param, last->u.cont.body};
            return cont_apply_call(u, cont, &head_at, aargs, nargs - 1, kenv);
        }
        if (last->kind == CV_VAR) {
            Cont cont;
            if (kenv_lookup(kenv, last->u.var.name, &cont)) {
                return cont_apply_call(u, cont, &head_at, aargs, nargs - 1, kenv);
            }
        }
        uerr(u, "cannot un-CPS: call without a recognizable continuation");
        return NULL;
    }

    case CE_THROW: {
        CVal k = C->u.throw_.k;
        if (k.kind == CV_VAR) {
            Cont cont;
            if (kenv_lookup(kenv, k.u.var.name, &cont)) {
                Atom val;
                if (!cv_atom(&C->u.throw_.v, u, &val)) return NULL;
                return cont_apply(u, cont, &val, kenv);
            }
        }
        uerr(u, "cannot un-CPS: throw to a value that is not a known continuation");
        return NULL;
    }

    case CE_IF: {
        Atom cond;
        if (!cv_atom(&C->u.if_.cond, u, &cond)) return NULL;
        Anf *then = uncps(C->u.if_.then, kenv, u);
        if (u->error) return NULL;
        Anf *els = uncps(C->u.if_.els, kenv, u);
        return anf_if(u->a, cond, then, els);
    }

    case CE_HALT: {
        Atom val;
        if (!cv_atom(&C->u.halt.v, u, &val)) return NULL;
        return anf_ret(u->a, val);
    }
    }
    uerr(u, "internal: bad CExp");
    return NULL;
}

bool cps_to_anf(const CExp *prog, Arena *a, Anf **out, char **errmsg) {
    Uc u = {a, NULL};
    if (prog->kind != CE_LET || prog->u.let.val.kind != CV_CONT) {
        uerr(&u, "internal: bad CPS program shape (expected top-level continuation let)");
        *out = NULL;
        if (errmsg) *errmsg = u.error;
        return false;
    }
    Cont halt = {CT_HALT, NULL, NULL};
    Kenv *k0 = kenv_push(a, NULL, prog->u.let.name, halt);
    Anf *res = uncps(prog->u.let.body, k0, &u);
    if (u.error) {
        *out = NULL;
        if (errmsg) *errmsg = u.error;
        return false;
    }
    *out = res;
    return true;
}