#ifndef YAC_EVAL_ANF_H
#define YAC_EVAL_ANF_H

#include "anf.h"
#include "gcobj.h"

/* Return-frame stack: the direct-style machine's explicit continuation.
 * A non-tail call pushes a frame {bind name, continue rest}; a tail call
 * (N_TAIL_CALL) never pushes, so deep tail recursion stays on the heap.
 * Frames are GC heap objects; the current frame chain is a root. */

typedef struct Frame {
    GObj hdr;
    struct Frame *prev;
    const char *name; /* bind the returned value to this name */
    const Anf *rest;  /* continue evaluating here */
    Binding *env;     /* environment at the continuation point */
} Frame;

struct Gc;

/* Run an ANF program to completion. Returns 0 on success and sets *result;
 * returns nonzero and sets *errmsg (arena-owned) on a runtime error.
 * `gc` owns all runtime allocations (closures, frames, bindings, arrays). */
int eval_anf_run(const Anf *prog, Arena *a, Value *result, char **errmsg,
                 struct Gc *gc);

#endif