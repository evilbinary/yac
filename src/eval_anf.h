#ifndef YAC_EVAL_ANF_H
#define YAC_EVAL_ANF_H

#include "anf.h"
#include "gcobj.h"

/* Return-frame stack: the direct-style machine's explicit continuation.
 * A non-tail call pushes a CFrame {bind slot, continue rest}; a tail call
 * (N_TAIL_CALL) never pushes, so deep tail recursion stays on the heap.
 * CFrames are GC heap objects; the current chain is a root. */

typedef struct CFrame {
    GObj hdr;
    struct CFrame *prev;
    int slot;         /* bind the returned value to this frame slot */
    const Anf *rest;  /* continue evaluating here */
    struct Frame *env; /* the caller's frame (where the slot lives) */
} CFrame;

struct Gc;

/* Run an ANF program to completion. Returns 0 on success and sets *result;
 * returns nonzero and sets *errmsg (arena-owned) on a runtime error.
 * `gc` owns all runtime allocations (closures, frames, bindings, arrays). */
int eval_anf_run(const Anf *prog, int top_nslots, Arena *a, Value *result,
                 char **errmsg, struct Gc *gc);

/* Run an ANF program starting from a caller-provided top-level frame (used by
 * the REPL to keep global bindings across inputs). */
int eval_anf_run_in(const Anf *prog, struct Frame *top, Arena *a, Value *result,
                    char **errmsg, struct Gc *gc);

/* The machine state at a checkpoint: the IR root, the current node, the
 * current frame, and the continuation-frame stack. */
typedef struct AnfState {
    const Anf *root;
    const Anf *node;
    struct Frame *env;
    struct CFrame *cframe;
} AnfState;

/* Checkpoint hook: if set, called at the start of every trampoline step with
 * the current state and the step counter. Return nonzero to pause the machine
 * (eval returns 2) after dumping the state. */
extern int (*yac_ckpt_hook)(const AnfState *st, long step);

/* Resume a paused machine from a checkpoint. Returns 0 on completion, 2 if
 * paused again, nonzero on error. */
int eval_anf_resume(const Anf *root, const Anf *node, struct Frame *env,
                    struct CFrame *cframe, long step, Arena *a, Value *result,
                    char **errmsg, struct Gc *gc);

#endif