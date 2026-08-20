#ifndef YAC_GC_H
#define YAC_GC_H

#include <stdbool.h>
#include <stddef.h>

#include "gcobj.h"
#include "value.h"

/* Mark-sweep collector for runtime objects: closures, environment bindings,
 * ANF continuation frames, and call-argument value arrays. The IR (AST/ANF/
 * CPS) and string literals stay in the arena and are never collected. The
 * machine keeps its current environment and any in-flight values on the
 * collector's root set; gc_alloc triggers a collection when the allocation
 * budget since the last collection is exceeded. */

typedef struct Gc {
    GObj *all;            /* all-objects list (used by sweep) */
    size_t threshold;     /* bytes allocated before a collection */
    size_t allocated;     /* bytes allocated since the last collection */
    size_t live;          /* approximate live bytes */
    size_t live_objs;     /* live object count (for --limit-nodes) */
    size_t total_objs;    /* objects on the all-list right now */
    size_t max_objs;      /* 0 = unlimited; else abort when exceeded */
    bool enabled;         /* false = never collect (arena-style growth) */
    Frame *envroot;       /* root: the machine's current frame */
    GObj *frameroot;      /* root: the ANF machine's current frame chain */
    GObj **roots;         /* root stack: in-flight values/objects */
    int nroots;
    int caproots;
} Gc;

/* A heap-resident array of Values (e.g. call arguments). */
typedef struct ValArr {
    GObj hdr;
    int n;
    Value data[]; /* flexible array of n Values */
} ValArr;

void gc_init(Gc *g, size_t threshold);
void gc_free(Gc *g);

/* Allocate a GC object. `size` is the FULL allocation size (header included);
 * returns the object pointer (the header is its first field). May trigger a
 * collection first. */
void *gc_alloc(Gc *g, GKind kind, size_t size);

/* Root management. */
void gc_push_root(Gc *g, GObj *o);
void gc_pop_root(Gc *g);
void gc_push_value(Gc *g, Value v); /* push the GC pointer inside a Value, if any */
void gc_set_env(Gc *g, Frame *env);
void gc_set_frame(Gc *g, GObj *frame);

/* Typed allocations. */
Frame *gc_new_frame(Gc *g, int nslots);
Closure *gc_new_closure(Gc *g);
ValArr *gc_new_valarr(Gc *g, int n);
List *gc_new_list(Gc *g, int n);
Bignum *gc_new_bignum(Gc *g, int ndigits);

void gc_collect(Gc *g);

#endif