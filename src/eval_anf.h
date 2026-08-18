#ifndef YAC_EVAL_ANF_H
#define YAC_EVAL_ANF_H

#include "anf.h"

/* Run an ANF program to completion. Returns 0 on success and sets *result;
 * returns nonzero and sets *errmsg (arena-owned) on a runtime error. */
int eval_anf_run(const Anf *prog, Arena *a, Value *result, char **errmsg);

#endif