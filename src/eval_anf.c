#include "eval_anf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "env.h"

/* Return-frame stack: the direct-style machine's explicit continuation.
 * A non-tail call pushes a frame {bind name, continue rest}; a tail call
 * (N_TAIL_CALL) never pushes, so deep tail recursion stays on the heap. */
typedef struct Frame {
    struct Frame *prev;
    const char *name; /* bind the returned value to this name */
    const Anf *rest;  /* continue evaluating here */
    Binding *env;     /* environment at the continuation point */
} Frame;

typedef struct {
    Arena *a;
    char **errmsg;
    bool errored;
} Est;

static void fail(Est *st, const char *fmt, ...) {
    if (st->errored) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (st->errmsg) *st->errmsg = arena_strdup(st->a, buf);
    st->errored = true;
}

static Value eval_atom(const Atom *atom, Binding *env, Est *st) {
    switch (atom->kind) {
    case AT_VAR: {
        Value v;
        if (!env_lookup(env, atom->u.var.name, &v)) {
            fail(st, "unbound variable '%s'", atom->u.var.name);
            return VALUE_NULL;
        }
        return v;
    }
    case AT_LIT:
        return atom->u.lit;
    case AT_LAM: {
        Closure *clo = (Closure *)arena_alloc(st->a, sizeof(Closure));
        clo->body = atom->u.lam.body;
        clo->params = atom->u.lam.params;
        clo->nparams = atom->u.lam.nparams;
        clo->env = env;
        return v_fun(clo);
    }
    }
    fail(st, "internal: bad atom");
    return VALUE_NULL;
}

static Binding *bind_params(Arena *a, const Closure *clo, Value *args) {
    Binding *env = clo->env;
    for (int i = 0; i < clo->nparams; i++) {
        env = env_bind(a, env, clo->params[i], args[i]);
    }
    return env;
}

static Binding *bind_prims(Arena *a) {
    Binding *env = NULL;
    int n;
    const Prim *prims = prim_table(&n);
    for (int i = 0; i < n; i++) {
        env = env_bind(a, env, prims[i].name, v_prim(&prims[i]));
    }
    return env;
}

static void apply_prim(Value head, Value *args, int nargs, Value *out, Est *st) {
    const Prim *p = head.u.prim;
    if (p->arity >= 0 && p->arity != nargs) {
        fail(st, "primitive '%s' expects %d argument(s), got %d", p->name, p->arity, nargs);
        return;
    }
    PrimCtx ctx = {false, ""};
    *out = p->fn(args, nargs, &ctx);
    if (ctx.errored) fail(st, "%s", ctx.errmsg);
}

static int tail_return(Value v, Frame **frame, Binding **env, const Anf **node, Arena *a) {
    if (!*frame) return 1; /* top level: done */
    Frame *f = *frame;
    *frame = f->prev;
    *env = env_bind(a, f->env, f->name, v);
    *node = f->rest;
    return 0;
}

int eval_anf_run(const Anf *prog, Arena *a, Value *result, char **errmsg) {
    Est st = {a, errmsg, false};
    const Anf *node = prog;
    Binding *env = bind_prims(a);
    Frame *frame = NULL;

    for (;;) {
        switch (node->kind) {
        case N_LET: {
            Value v = eval_atom(&node->u.let.atom, env, &st);
            if (st.errored) goto err;
            if (node->u.let.atom.kind == AT_LAM) {
                /* recursive binding: closure captures the new env which
                 * already contains the binding (bind-then-fill) */
                Binding *b = env_bind(a, env, node->u.let.name, v);
                v.u.clo->env = b;
                b->value = v;
                env = b;
            } else {
                env = env_bind(a, env, node->u.let.name, v);
            }
            node = node->u.let.body;
            break;
        }
        case N_LET_CALL: {
            Value head = eval_atom(&node->u.call.head, env, &st);
            if (st.errored) goto err;
            Value *args = (Value *)arena_alloc(a, (size_t)node->u.call.nargs * sizeof(Value));
            for (int i = 0; i < node->u.call.nargs; i++) {
                args[i] = eval_atom(&node->u.call.args[i], env, &st);
                if (st.errored) goto err;
            }
            if (head.tag == V_FUN) {
                Closure *clo = head.u.clo;
                if (clo->nparams != node->u.call.nargs) {
                    fail(&st, "%d:%d: function expects %d argument(s), got %d",
                         node->line, 0, clo->nparams, node->u.call.nargs);
                    goto err;
                }
                Frame *f = (Frame *)arena_alloc(a, sizeof(Frame));
                f->prev = frame;
                f->name = node->u.call.name;
                f->rest = node->u.call.body;
                f->env = env;
                frame = f;
                env = bind_params(a, clo, args);
                node = clo->body;
            } else if (head.tag == V_PRIM) {
                Value v;
                apply_prim(head, args, node->u.call.nargs, &v, &st);
                if (st.errored) goto err;
                env = env_bind(a, env, node->u.call.name, v);
                node = node->u.call.body;
            } else {
                fail(&st, "%d:%d: cannot apply a non-function value", node->line, 0);
                goto err;
            }
            break;
        }
        case N_IF: {
            Value c = eval_atom(&node->u.if_.cond, env, &st);
            if (st.errored) goto err;
            if (c.tag != V_BOOL) {
                fail(&st, "%d:%d: if condition must be a boolean", node->line, 0);
                goto err;
            }
            node = c.u.b ? node->u.if_.then : node->u.if_.els;
            break;
        }
        case N_TAIL_CALL: {
            Value head = eval_atom(&node->u.tailcall.head, env, &st);
            if (st.errored) goto err;
            Value *args = (Value *)arena_alloc(a, (size_t)node->u.tailcall.nargs * sizeof(Value));
            for (int i = 0; i < node->u.tailcall.nargs; i++) {
                args[i] = eval_atom(&node->u.tailcall.args[i], env, &st);
                if (st.errored) goto err;
            }
            if (head.tag == V_FUN) {
                Closure *clo = head.u.clo;
                if (clo->nparams != node->u.tailcall.nargs) {
                    fail(&st, "%d:%d: function expects %d argument(s), got %d",
                         node->line, 0, clo->nparams, node->u.tailcall.nargs);
                    goto err;
                }
                env = bind_params(a, clo, args);
                node = clo->body;
            } else if (head.tag == V_PRIM) {
                Value v;
                apply_prim(head, args, node->u.tailcall.nargs, &v, &st);
                if (st.errored) goto err;
                if (tail_return(v, &frame, &env, &node, a)) {
                    *result = v;
                    return 0;
                }
            } else {
                fail(&st, "%d:%d: cannot apply a non-function value", node->line, 0);
                goto err;
            }
            break;
        }
        case N_RETURN: {
            Value v = eval_atom(&node->u.ret, env, &st);
            if (st.errored) goto err;
            if (tail_return(v, &frame, &env, &node, a)) {
                *result = v;
                return 0;
            }
            break;
        }
        case N_LET_CALLCC:
            fail(&st, "%d:%d: callcc is only supported in CPS mode", node->line, 0);
            goto err;
        case N_TAIL_THROW:
            fail(&st, "%d:%d: throw is only supported in CPS mode", node->line, 0);
            goto err;
        }
    }

err:
    return 1;
}