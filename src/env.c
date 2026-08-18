#include "env.h"

#include <string.h>

Binding *env_bind(Arena *a, Binding *env, const char *name, Value v) {
    Binding *b = (Binding *)arena_alloc(a, sizeof(Binding));
    b->name = name;
    b->value = v;
    b->prev = env;
    return b;
}

bool env_lookup(Binding *env, const char *name, Value *out) {
    for (Binding *b = env; b; b = b->prev) {
        if (strcmp(b->name, name) == 0) {
            *out = b->value;
            return true;
        }
    }
    return false;
}
