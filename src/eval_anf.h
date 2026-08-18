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

#endif