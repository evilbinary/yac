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

/* ---- low-level token reader (shared with the checkpoint format) ---- */

typedef struct Rd Rd;

Rd *rd_open(const char *data, size_t len, Arena *a);
void rd_close(Rd *r);
const char *rd_error(Rd *r);
void rd_set_error(Rd *r, const char *msg);
Arena *rd_arena(Rd *r);
char *rd_word(Rd *r);
char *rd_name(Rd *r);
void rd_expect(Rd *r, char c);
int rd_peek(Rd *r);
bool rd_atom(Rd *r, Atom *out);

/* Read "(rt <top_nslots> <node>)" from the reader. */
bool anf_read(Rd *r, Anf **out, int *top_nslots);

#endif