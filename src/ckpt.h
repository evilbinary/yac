#ifndef YAC_CKPT_H
#define YAC_CKPT_H

#include <stdbool.h>

#include "eval_anf.h"
#include "gc.h"

/* Execution checkpoints for the ANF machine: serialize the current IR node,
 * the frame stack, the continuation-frame stack, and every reachable closure,
 * so execution can be resumed later (--resume). */

/* Dump the machine state to `path`; returns 0 on success. */
int ckpt_dump(const AnfState *st, long step, const char *path);

/* Load a checkpoint and reconstruct the machine state. On success returns
 * true and sets the root, node, env and cframe (the values to pass to
 * eval_anf_resume) plus the saved step. */
bool ckpt_resume(const char *path, Arena *a, Gc *gc, const Anf **root,
                 const Anf **node, Frame **env, CFrame **cframe, long *step,
                 char **errmsg);

#endif