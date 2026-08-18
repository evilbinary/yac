#include "eval_cps.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "env.h"
#include "gc.h"

/* CPS trampoline: the machine state is just (code, env). There is no frame
 * stack -- continuations are first-class closures, so a call either binds the
 * callee's parameters and continues (tail jump) or hands the result to an
 * explicit continuation value. The C stack never grows. */

typedef struct {
    Gc *gc;
    Arena *a;
    char **errmsg;
    bool errored;
} Cst;

static void fail(Cst *st, const char *fmt, ...) {
    if (st->errored) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (st->errmsg) *st->errmsg = arena_strdup(st->a, buf);
    st->errored = true;
}

static Binding *bind_prims(Gc *gc) {
    Binding *env = NULL;
    int n;
    const Prim *prims = prim_table(&n);
    for (int i = 0; i < n; i++) {
        env = env_bind(gc, env, prims[i].name, v_prim(&prims[i]));
    }
    return env;
}

static Binding *bind_params(Gc *gc, const Closure *clo, Value *args) {
    Binding *env = clo->env;
    for (int i = 0; i < clo->nparams; i++) {
        env = env_bind(gc, env, clo->params[i], args[i]);
    }
    return env;
}

static void apply_prim(Value head, Value *args, int nargs, Value *out, Cst *st) {
    const Prim *p = head.u.prim;
    if (p->arity >= 0 && p->arity != nargs) {
        fail(st, "primitive '%s' expects %d argument(s), got %d", p->name, p->arity, nargs);
        return;
    }
    PrimCtx ctx = {false, ""};
    *out = p->fn(args, nargs, &ctx);
    if (ctx.errored) fail(st, "%s", ctx.errmsg);
}

/* ---- eval_val: values only (no calls) ---- */

static Value eval_val(const CVal *v, Binding *env, Cst *st) {
    switch (v->kind) {
    case CV_VAR: {
        Value r;
        if (!env_lookup(env, v->u.var.name, &r)) {
            fail(st, "unbound variable '%s'", v->u.var.name);
            return VALUE_NULL;
        }
        return r;
    }
    case CV_LIT:
        return v->u.lit;
    case CV_PRIM: {
        const Prim *p = prim_lookup(v->u.prim.name);
        if (!p) {
            fail(st, "unbound primitive '%s'", v->u.prim.name);
            return VALUE_NULL;
        }
        return v_prim(p);
    }
    case CV_FUN: {
        Closure *clo = gc_new_closure(st->gc);
        clo->body = v->u.fun.body;
        clo->params = v->u.fun.params;
        clo->nparams = v->u.fun.nparams;
        clo->env = env;
        clo->cont_name = NULL;
        return v_fun(clo);
    }
    case CV_CONT: {
        Closure *clo = gc_new_closure(st->gc);
        clo->body = v->u.cont.body;
        clo->params = NULL;
        clo->nparams = 1;
        clo->env = env;
        clo->cont_name = v->u.cont.param;
        return v_cont(clo);
    }
    }
    fail(st, "internal: bad CPS value");
    return VALUE_NULL;
}

/* ---- apply_cont: jump to a continuation with a value ----
 * The continuation `k` and value `v` must be rooted by the caller. */
static void apply_cont(Value k, Value v, Binding **env, const CExp **code,
                       Cst *st) {
    if (k.tag != V_CONT) {
        fail(st, "expected a continuation, got a non-continuation value");
        return;
    }
    Closure *clo = k.u.clo;
    *env = env_bind(st->gc, clo->env, clo->cont_name, v);
    *code = clo->body;
}

int eval_cps_run(const CExp *prog, Arena *a, Value *result, char **errmsg, Gc *gc) {
    Cst st = {gc, a, errmsg, false};
    const CExp *code = prog;
    Binding *env = bind_prims(gc);

    for (;;) {
        switch (code->kind) {
        case CE_LET: {
            Value v = eval_val(&code->u.let.val, env, &st);
            if (st.errored) goto err;
            gc_push_value(gc, v);
            if (code->u.let.val.kind == CV_FUN || code->u.let.val.kind == CV_CONT) {
                /* literal closure: recursive binding -- the closure captures
                 * the env containing the binding (bind-then-fill). Aliases
                 * (CV_VAR etc.) must NOT re-patch, or the shared closure's
                 * env would drift to an ever-deeper chain. */
                Binding *b = env_bind(gc, env, code->u.let.name, v);
                v.u.clo->env = b;
                b->value = v;
                env = b;
            } else {
                env = env_bind(gc, env, code->u.let.name, v);
            }
            gc_pop_root(gc);
            code = code->u.let.body;
            break;
        }
        case CE_CALL: {
            Value head = eval_val(&code->u.call.head, env, &st);
            if (st.errored) goto err;
            gc_push_value(gc, head);
            ValArr *va = gc_new_valarr(gc, code->u.call.nargs);
            gc_push_root(gc, (GObj *)va);
            Value *args = va->data;
            for (int i = 0; i < code->u.call.nargs; i++) {
                args[i] = eval_val(&code->u.call.args[i], env, &st);
                if (st.errored) goto err;
            }
            if (head.tag == V_FUN) {
                Closure *clo = head.u.clo;
                if (clo->nparams != code->u.call.nargs) {
                    fail(&st, "%d:%d: function expects %d argument(s), got %d",
                         code->line, 0, clo->nparams, code->u.call.nargs);
                    goto err;
                }
                env = bind_params(gc, clo, args);
                code = clo->body;
            } else if (head.tag == V_PRIM) {
                if (code->u.call.nargs < 1) {
                    fail(&st, "%d:%d: internal: primitive call without continuation",
                         code->line, 0);
                    goto err;
                }
                Value v;
                apply_prim(head, args, code->u.call.nargs - 1, &v, &st);
                if (st.errored) goto err;
                gc_push_value(gc, v);
                Value cont = args[code->u.call.nargs - 1];
                gc_push_value(gc, cont);
                apply_cont(cont, v, &env, &code, &st);
                if (st.errored) goto err;
                gc_pop_root(gc); /* cont */
                gc_pop_root(gc); /* v */
            } else if (head.tag == V_CONT) {
                if (code->u.call.nargs != 1) {
                    fail(&st, "%d:%d: continuation applied to %d value(s), expected 1",
                         code->line, 0, code->u.call.nargs);
                    goto err;
                }
                gc_push_value(gc, args[0]);
                apply_cont(head, args[0], &env, &code, &st);
                if (st.errored) goto err;
                gc_pop_root(gc); /* args[0] */
            } else {
                fail(&st, "%d:%d: cannot apply a non-function value", code->line, 0);
                goto err;
            }
            gc_pop_root(gc); /* args array */
            gc_pop_root(gc); /* head */
            break;
        }
        case CE_THROW: {
            Value k = eval_val(&code->u.throw_.k, env, &st);
            if (st.errored) goto err;
            Value v = eval_val(&code->u.throw_.v, env, &st);
            if (st.errored) goto err;
            gc_push_value(gc, k);
            gc_push_value(gc, v);
            apply_cont(k, v, &env, &code, &st);
            if (st.errored) goto err;
            gc_pop_root(gc); /* v */
            gc_pop_root(gc); /* k */
            break;
        }
        case CE_IF: {
            Value c = eval_val(&code->u.if_.cond, env, &st);
            if (st.errored) goto err;
            if (c.tag != V_BOOL) {
                fail(&st, "%d:%d: if condition must be a boolean", code->line, 0);
                goto err;
            }
            code = c.u.b ? code->u.if_.then : code->u.if_.els;
            break;
        }
        case CE_HALT: {
            *result = eval_val(&code->u.halt.v, env, &st);
            if (st.errored) goto err;
            return 0;
        }
        }
    }

err:
    return 1;
}