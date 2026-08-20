#include "eval_cps.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gc.h"

/* CPS trampoline over flat environment frames. The machine state is just
 * (code, current frame). A call either allocates the callee's frame and
 * jumps (tail position) or hands the result to an explicit continuation
 * value. Continuations are applied without allocating a frame: the value is
 * written into the continuation's rslot in its captured frame, and control
 * switches to that frame. The C stack never grows. */

typedef struct {
    Gc *gc;
    Arena *a;
    char **errmsg;
    bool errored;
    Frame *env; /* current frame (rooted across nested primitive calls) */
} Cst;

static bool call_value(void *ud, Value head, Value *args, int nargs,
                       Value *out, char *errmsg, size_t errsz);
int eval_cps_run_in(const CExp *prog, Frame *env0, Arena *a,
                    Value *result, char **errmsg, Gc *gc);

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

static Value *frame_slot(Frame *env, int depth, int slot) {
    for (int i = 0; i < depth; i++) env = env->parent;
    return &env->slots[slot];
}

static void apply_prim(Value head, Value *args, int nargs, Value *out, Cst *st) {
    const Prim *p = head.u.prim;
    if (p->arity >= 0 && p->arity != nargs) {
        fail(st, "primitive '%s' expects %d argument(s), got %d", p->name, p->arity, nargs);
        return;
    }
    PrimCtx ctx = {false, "", st->gc, st->a, call_value, st};
    *out = p->fn(args, nargs, &ctx);
    if (ctx.errored) fail(st, "%s", ctx.errmsg);
}

/* Run a user function as a sub-computation in CPS. A CPS function takes an
 * extra continuation parameter (the last closure param); we pass a fresh
 * "return" continuation whose body halts the nested run with the value. */
static bool call_value(void *ud, Value head, Value *args, int nargs,
                       Value *out, char *errmsg, size_t errsz) {
    Cst *st = (Cst *)ud;
    (void)errmsg;
    (void)errsz;
    if (head.tag == V_PRIM) {
        apply_prim(head, args, nargs, out, st);
        if (st->errored) return false;
        gc_push_value(st->gc, *out); /* root while the caller copies it */
        gc_pop_root(st->gc);
        return true;
    }
    if (head.tag != V_FUN) {
        fail(st, "cannot apply a non-function value");
        return false;
    }
    Closure *clo = head.u.clo;
    if (clo->nparams != nargs + 1) {
        fail(st, "function expects %d argument(s), got %d", clo->nparams - 1, nargs);
        return false;
    }
    /* halt continuation: kappa(#r). halt #r, with #r in slot 0 of a fresh
     * frame; the nested run ends when the function applies it */
    CExp *halt = (CExp *)arena_alloc(st->a, sizeof(CExp));
    halt->kind = CE_HALT;
    halt->line = 0;
    halt->u.halt.v.kind = CV_VAR;
    halt->u.halt.v.u.var.name = "#r";
    halt->u.halt.v.u.var.depth = 0;
    halt->u.halt.v.u.var.slot = 0;
    Closure *k = gc_new_closure(st->gc);
    k->body = halt;
    k->params = NULL;
    k->nparams = 1;
    k->nslots = 1;
    k->kslot = -1;
    k->frame = gc_new_frame(st->gc, 1);
    k->cont_name = "#r";
    k->rslot = 0;

    gc_push_root(st->gc, (GObj *)st->env);
    gc_push_value(st->gc, v_cont(k));
    Frame *nf = gc_new_frame(st->gc, clo->nslots);
    nf->parent = clo->frame;
    for (int i = 0; i < nargs; i++) nf->slots[i] = args[i];
    nf->slots[clo->kslot] = v_cont(k);
    int rc = eval_cps_run_in(clo->body, nf, st->a, out, st->errmsg, st->gc);
    if (rc != 0) {
        gc_pop_root(st->gc); /* k */
        gc_pop_root(st->gc); /* env */
        return false;
    }
    gc_push_value(st->gc, *out); /* keep the result alive until the caller pops it */
    gc_pop_root(st->gc);         /* *out */
    gc_pop_root(st->gc);         /* k */
    gc_pop_root(st->gc);         /* env */
    gc_set_env(st->gc, st->env); /* restore the caller's root */
    return true;
}

/* ---- eval_val: values only (no calls) ---- */

static Value eval_val(const CVal *v, Frame *env, Cst *st) {
    switch (v->kind) {
    case CV_VAR: {
        if (v->u.var.slot < 0) {
            const Prim *p = prim_lookup(v->u.var.name);
            if (p) return v_prim(p);
            fail(st, "unbound variable '%s'", v->u.var.name);
            return VALUE_NULL;
        }
        return *frame_slot(env, v->u.var.depth, v->u.var.slot);
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
        clo->nslots = v->u.fun.nslots;
        clo->kslot = v->u.fun.kslot;
        clo->frame = env;
        clo->cont_name = NULL;
        clo->rslot = -1;
        return v_fun(clo);
    }
    case CV_CONT: {
        Closure *clo = gc_new_closure(st->gc);
        clo->body = v->u.cont.body;
        clo->params = NULL;
        clo->nparams = 1;
        clo->nslots = 1;
        clo->frame = env;
        clo->cont_name = v->u.cont.param;
        clo->rslot = v->u.cont.rslot;
        return v_cont(clo);
    }
    }
    fail(st, "internal: bad CPS value");
    return VALUE_NULL;
}

/* ---- apply_cont: jump to a continuation with a value ----
 * The continuation `k` and value `v` must be rooted by the caller. */
static void apply_cont(Value k, Value v, Frame **env, const CExp **code,
                       Cst *st) {
    if (k.tag != V_CONT) {
        fail(st, "expected a continuation, got a non-continuation value");
        return;
    }
    Closure *clo = k.u.clo;
    *env = clo->frame;
    gc_set_env(st->gc, clo->frame);
    clo->frame->slots[clo->rslot] = v;
    *code = clo->body;
}

int eval_cps_run_in(const CExp *prog, Frame *env0, Arena *a, Value *result,
                    char **errmsg, Gc *gc) {
    Cst st = {gc, a, errmsg, false, env0};
    const CExp *code = prog;
    Frame *env = env0;
    gc_set_env(gc, env);

    for (;;) {
        switch (code->kind) {
        case CE_LET: {
            Value v = eval_val(&code->u.let.val, env, &st);
            if (st.errored) goto err;
            gc_push_value(gc, v);
            env->slots[code->u.let.slot] = v;
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
                Frame *nf = gc_new_frame(gc, clo->nslots);
                nf->parent = clo->frame;
                for (int i = 0; i < code->u.call.nargs - 1; i++) nf->slots[i] = args[i];
                nf->slots[clo->kslot] = args[code->u.call.nargs - 1];
                env = nf;
                gc_set_env(gc, nf);
                code = clo->body;
            } else if (head.tag == V_PRIM) {
                if (code->u.call.nargs < 1) {
                    fail(&st, "%d:%d: internal: primitive call without continuation",
                         code->line, 0);
                    goto err;
                }
                Value v;
                st.env = env;
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
            gc_push_value(gc, k); /* k must be rooted before v is evaluated */
            Value v = eval_val(&code->u.throw_.v, env, &st);
            if (st.errored) goto err;
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

int eval_cps_run(const CExp *prog, int top_nslots, Arena *a, Value *result,
                 char **errmsg, Gc *gc) {
    Frame *top = gc_new_frame(gc, top_nslots);
    return eval_cps_run_in(prog, top, a, result, errmsg, gc);
}