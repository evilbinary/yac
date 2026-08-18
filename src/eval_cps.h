#ifndef YAC_EVAL_CPS_H
#define YAC_EVAL_CPS_H

#include "cps.h"

struct Gc;

/* Run a CPS program to completion via the trampoline. Returns 0 on success
 * and sets *result; returns nonzero and sets *errmsg (arena-owned) on a
 * runtime error. callcc/throw are supported here (unlike eval_anf).
 * `top_nslots` is the CPS top-level frame size; `gc` owns all runtime
 * allocations (closures, frames, arrays). */
int eval_cps_run(const CExp *prog, int top_nslots, Arena *a, Value *result,
                 char **errmsg, struct Gc *gc);

/* Run a CPS program starting from a caller-provided top-level frame. */
int eval_cps_run_in(const CExp *prog, struct Frame *top, Arena *a,
                    Value *result, char **errmsg, struct Gc *gc);

#endif