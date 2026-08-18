#ifndef YAC_GCOBJ_H
#define YAC_GCOBJ_H

#include <stddef.h>

/* GC heap object header. Every heap-resident runtime object (closure,
 * environment binding, continuation frame, value array) begins with
 * a GObj. `next` chains the object into the collector's all-objects list;
 * `mark` is set during the mark phase. */

typedef enum {
    G_STR,    /* Str        */
    G_BIND,   /* Binding    */
    G_CLO,    /* Closure    */
    G_FRAME,  /* Frame (ANF continuation stack) */
    G_VALARR, /* Value array (call arguments)   */
} GKind;

typedef struct GObj {
    struct GObj *next; /* all-objects list */
    size_t size;       /* total allocation size (header + payload) */
    unsigned kind : 4;
    unsigned mark : 1;
} GObj;

#endif