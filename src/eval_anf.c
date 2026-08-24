#include "eval_anf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "profile.h"
#include "value.h"

/* Direct-style machine over flat environment frames. The machine state is
 * (code, current frame, continuation-frame stack). A variable reference
 * (depth, slot) resolves by walking `depth` parent pointers from the current
 * frame, then indexing slots[slot]. A non-tail call pushes a CFrame; a tail
 * call just allocates a fresh frame and jumps (C stack never grows). */

typedef struct {
    Gc *gc;
    Arena *a;
    char **errmsg;
    bool errored;
    Frame *env;    /* current frame (rooted across nested primitive calls) */
    CFrame *cframe; /* current continuation stack (rooted likewise) */
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

static Value *frame_slot(Frame *env, int depth, int slot) {
    for (int i = 0; i < depth; i++) env = env->parent;
    return &env->slots[slot];
}

static bool call_value(void *ud, Value head, Value *args, int nargs,
                       Value *out, char *errmsg, size_t errsz);
static int eval_anf_core(const Anf *root, const Anf *node, Frame *env0,
                         CFrame *cframe0, long start_step, Arena *a,
                         Value *result, char **errmsg, Gc *gc);

static Value eval_atom(const Atom *atom, Frame *env, Est *st) {
    switch (atom->kind) {
    case AT_VAR: {
        if (atom->u.var.slot < 0) {
            const Prim *p = prim_lookup(atom->u.var.name);
            if (p) return v_prim(p);
            fail(st, "unbound variable '%s'", atom->u.var.name);
            return VALUE_NULL;
        }
        return *frame_slot(env, atom->u.var.depth, atom->u.var.slot);
    }
    case AT_LIT:
        return atom->u.lit;
    case AT_LAM: {
        Closure *clo = gc_new_closure(st->gc);
        clo->body = atom->u.lam.body;
        clo->params = atom->u.lam.params;
        clo->nparams = atom->u.lam.nparams;
        clo->nslots = atom->u.lam.nslots;
        clo->kslot = -1;
        clo->frame = env;
        clo->cont_name = NULL;
        clo->rslot = -1;
        clo->debug_name = NULL;
        return v_fun(clo);
    }
    }
    fail(st, "internal: bad atom");
    return VALUE_NULL;
}

static const char *clo_prof_name(const Closure *clo) {
    if (clo->debug_name && clo->debug_name[0]) return clo->debug_name;
    return "lam";
}

static void apply_prim(Value head, Value *args, int nargs, Value *out, Est *st) {
    const Prim *p = head.u.prim;
    if (p->arity >= 0 && p->arity != nargs) {
        fail(st, "primitive '%s' expects %d argument(s), got %d", p->name, p->arity, nargs);
        return;
    }
    PrimCtx ctx = {false, "", st->gc, st->a, call_value, st};
    if (yac_prof_enabled()) yac_prof_enter(p->name);
    *out = p->fn(args, nargs, &ctx);
    if (yac_prof_enabled()) yac_prof_leave();
    if (ctx.errored) fail(st, "%s", ctx.errmsg);
}

/* Run a user function to completion as a sub-computation (used by the
 * higher-order list primitives). The caller's machine state is rooted for the
 * duration of the nested run; the nested machine returns the value of the
 * function's final (tail) return. On success the result is left ROOTED (the
 * caller pops it once it has copied the value into a rooted location). */
static bool call_value(void *ud, Value head, Value *args, int nargs,
                       Value *out, char *errmsg, size_t errsz) {
    Est *st = (Est *)ud;
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
    if (clo->nparams != nargs) {
        fail(st, "function expects %d argument(s), got %d", clo->nparams, nargs);
        return false;
    }
    gc_push_root(st->gc, (GObj *)st->env);
    gc_push_root(st->gc, (GObj *)st->cframe);
    Frame *nf = gc_new_frame(st->gc, clo->nslots);
    nf->parent = clo->frame;
    for (int i = 0; i < nargs; i++) nf->slots[i] = args[i];
    int rc;
    if (yac_prof_enabled()) yac_prof_enter(clo_prof_name(clo));
    rc = eval_anf_core(clo->body, clo->body, nf, NULL, 0, st->a, out,
                           st->errmsg, st->gc);
    if (yac_prof_enabled()) yac_prof_leave();
    if (rc != 0) {
        gc_pop_root(st->gc); /* cframe */
        gc_pop_root(st->gc); /* env */
        return false;
    }
    gc_push_value(st->gc, *out);     /* keep the result alive until the caller pops it */
    gc_pop_root(st->gc);             /* *out (top of stack) */
    gc_pop_root(st->gc);             /* cframe */
    gc_pop_root(st->gc);             /* env */
    gc_set_env(st->gc, st->env);     /* restore the caller's roots */
    gc_set_frame(st->gc, (GObj *)st->cframe);
    return true;
}

/* Allocate the callee's frame, bind args into its param slots, and jump. */
static void enter_call(Gc *gc, const Closure *clo, Value *args, int nargs,
                       Frame **env) {
    Frame *nf = gc_new_frame(gc, clo->nslots);
    nf->parent = clo->frame;
    for (int i = 0; i < nargs; i++) nf->slots[i] = args[i];
    *env = nf;
    gc_set_env(gc, nf);
}

/* Pop the current continuation frame; bind `v` to its slot in the caller's
 * frame and resume its rest. Returns 1 at the top level (program done). */
static int tail_return(Value v, CFrame **cframe, Frame **env, const Anf **node,
                       Gc *gc) {
    if (!*cframe) return 1;
    if (yac_prof_enabled()) yac_prof_leave();
    CFrame *f = *cframe;
    *cframe = f->prev;
    gc_set_frame(gc, (GObj *)*cframe);
    *env = f->env;
    gc_set_env(gc, f->env);
    f->env->slots[f->slot] = v;
    *node = f->rest;
    return 0;
}

int (*yac_ckpt_hook)(const AnfState *st, long step) = NULL;

static int eval_anf_core(const Anf *root, const Anf *node, Frame *env0,
                         CFrame *cframe0, long start_step, Arena *a,
                         Value *result, char **errmsg, Gc *gc) {
    Est st = {gc, a, errmsg, false, env0, cframe0};
    Frame *env = env0;
    gc_set_env(gc, env);
    CFrame *cframe = cframe0;
    long step = start_step;

    for (;;) {
        if (yac_ckpt_hook) {
            AnfState as = {root, node, env, cframe};
            if (yac_ckpt_hook(&as, step)) return 2; /* paused for checkpoint */
        }
        step++;
        switch (node->kind) {
        case N_LET: {
            Value v = eval_atom(&node->u.let.atom, env, &st);
            if (st.errored) goto err;
            if (v.tag == V_FUN && node->u.let.name && node->u.let.name[0] != '#')
                v.u.clo->debug_name = node->u.let.name;
            env->slots[node->u.let.slot] = v;
            node = node->u.let.body;
            break;
        }
        case N_LET_CALL: {
            Value head = eval_atom(&node->u.call.head, env, &st);
            if (st.errored) goto err;
            gc_push_value(gc, head);
            ValArr *va = gc_new_valarr(gc, node->u.call.nargs);
            gc_push_root(gc, (GObj *)va);
            Value *args = va->data;
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
                CFrame *f = (CFrame *)gc_alloc(gc, G_FRAME, sizeof(CFrame));
                f->prev = cframe;
                f->slot = node->u.call.slot;
                f->rest = node->u.call.body;
                f->env = env;
                cframe = f;
                gc_set_frame(gc, (GObj *)f);
                enter_call(gc, clo, args, node->u.call.nargs, &env);
                if (yac_prof_enabled()) yac_prof_enter(clo_prof_name(clo));
                node = clo->body;
            } else if (head.tag == V_PRIM) {
                Value v;
                st.env = env;
                st.cframe = cframe;
                apply_prim(head, args, node->u.call.nargs, &v, &st);
                if (st.errored) goto err;
                gc_set_env(gc, env);
                gc_set_frame(gc, (GObj *)cframe);
                env->slots[node->u.call.slot] = v;
                node = node->u.call.body;
            } else {
                fail(&st, "%d:%d: cannot apply a non-function value", node->line, 0);
                goto err;
            }
            gc_pop_root(gc); /* args array */
            gc_pop_root(gc); /* head */
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
            gc_push_value(gc, head);
            ValArr *va = gc_new_valarr(gc, node->u.tailcall.nargs);
            gc_push_root(gc, (GObj *)va);
            Value *args = va->data;
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
                enter_call(gc, clo, args, node->u.tailcall.nargs, &env);
                if (yac_prof_enabled()) {
                    yac_prof_leave();
                    yac_prof_enter(clo_prof_name(clo));
                }
                node = clo->body;
            } else if (head.tag == V_PRIM) {
                Value v;
                st.env = env;
                st.cframe = cframe;
                apply_prim(head, args, node->u.tailcall.nargs, &v, &st);
                if (st.errored) goto err;
                gc_set_env(gc, env);
                gc_set_frame(gc, (GObj *)cframe);
                gc_push_value(gc, v);
                if (tail_return(v, &cframe, &env, &node, gc)) {
                    *result = v;
                    gc_pop_root(gc); /* v */
                    gc_pop_root(gc); /* args array */
                    gc_pop_root(gc); /* head */
                    return 0;
                }
                gc_pop_root(gc);
            } else {
                fail(&st, "%d:%d: cannot apply a non-function value", node->line, 0);
                goto err;
            }
            gc_pop_root(gc); /* args array */
            gc_pop_root(gc); /* head */
            break;
        }
        case N_RETURN: {
            Value v = eval_atom(&node->u.ret, env, &st);
            if (st.errored) goto err;
            gc_push_value(gc, v);
            if (tail_return(v, &cframe, &env, &node, gc)) {
                *result = v;
                gc_pop_root(gc); /* balance: value escapes through `result` */
                return 0;
            }
            gc_pop_root(gc);
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

int eval_anf_run_in(const Anf *prog, Frame *env0, Arena *a, Value *result,
                    char **errmsg, Gc *gc) {
    return eval_anf_core(prog, prog, env0, NULL, 0, a, result, errmsg, gc);
}

int eval_anf_resume(const Anf *root, const Anf *node, Frame *env,
                    CFrame *cframe, long step, Arena *a, Value *result,
                    char **errmsg, Gc *gc) {
    return eval_anf_core(root, node, env, cframe, step, a, result, errmsg, gc);
}

int eval_anf_run(const Anf *prog, int top_nslots, Arena *a, Value *result,
                 char **errmsg, Gc *gc) {
    Frame *top = gc_new_frame(gc, top_nslots);
    return eval_anf_run_in(prog, top, a, result, errmsg, gc);
}