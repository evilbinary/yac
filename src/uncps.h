#ifndef YAC_UNCPS_H
#define YAC_UNCPS_H

#include <stdbool.h>

#include "anf.h"
#include "cps.h"

/* Restricted CPS -> ANF ("un-CPS").
 *
 * Works only for programs whose continuations never escape: no callcc, no
 * continuation stored or passed as a value, continuations used only in
 * tail/application position. On any violation it fails with an error message.
 * When it succeeds, the resulting ANF is semantically equivalent to the
 * original ANF the CPS was produced from. */
bool cps_to_anf(const CExp *prog, Arena *a, Anf **out, char **errmsg);

#endif