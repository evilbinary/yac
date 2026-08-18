#ifndef YAC_EVAL_CPS_H
#define YAC_EVAL_CPS_H

#include "cps.h"

/* Run a CPS program to completion via the trampoline. Returns 0 on success
 * and sets *result; returns nonzero and sets *errmsg (arena-owned) on a
 * runtime error. callcc/throw are supported here (unlike eval_anf). */
int eval_cps_run(const CExp *prog, Arena *a, Value *result, char **errmsg);

#endif