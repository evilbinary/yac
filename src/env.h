#ifndef YAC_ENV_H
#define YAC_ENV_H

#include <stdbool.h>

#include "arena.h"
#include "gcobj.h"
#include "value.h"

/* Lexical environment: a singly-linked chain of bindings. Bindings are GC
 * heap objects; the chain is rooted through the machine's envroot. */

struct Gc;

typedef struct Binding {
    GObj hdr;          /* GC heap header */
    const char *name;  /* arena/IR string */
    Value value;
    struct Binding *prev;
} Binding;

Binding *env_bind(struct Gc *g, Binding *env, const char *name, Value v);
bool env_lookup(Binding *env, const char *name, Value *out);

#endif