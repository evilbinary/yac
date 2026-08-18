#ifndef YAC_RTIO_H
#define YAC_RTIO_H

#include <stdbool.h>
#include <stdio.h>

#include "anf.h"

/* Serialize an ANF program (the "runtime" of a compiled program) to a text
 * file, and rebuild it later, so a program can be loaded and run without
 * re-parsing. The text format is stable across the IR's shape. */

void anf_write(const Anf *node, FILE *f);

/* Read an ANF program from a file (or NULL for stdin). Returns true and sets
 * *out and *top_nslots on success; returns false and sets *errmsg on error. */
bool anf_read_file(const char *path, Arena *a, Anf **out, int *top_nslots,
                   char **errmsg);

#endif