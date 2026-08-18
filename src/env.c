#include "env.h"

#include <string.h>

#include "gc.h"

Binding *env_bind(struct Gc *g, Binding *env, const char *name, Value v) {
    Binding *b = gc_new_binding(g, name, v, env);
    gc_set_env(g, b); /* keep the growing chain rooted */
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