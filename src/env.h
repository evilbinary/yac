#ifndef YAC_ENV_H
#define YAC_ENV_H

#include <stdbool.h>

#include "arena.h"
#include "value.h"

/* Lexical environment: a singly-linked chain of bindings. */

typedef struct Binding {
    const char *name;
    Value value;
    struct Binding *prev;
} Binding;

Binding *env_bind(Arena *a, Binding *env, const char *name, Value v);
bool env_lookup(Binding *env, const char *name, Value *out);

#endif
