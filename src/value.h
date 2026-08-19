#ifndef YAC_VALUE_H
#define YAC_VALUE_H

#include <stdbool.h>
#include <stdint.h>

#include "arena.h"
#include "gcobj.h"

typedef struct Anf Anf;
typedef struct Frame Frame;
typedef struct Closure Closure;
typedef struct Gc Gc;
typedef struct List List;

typedef enum {
    V_INT,
    V_FLOAT,
    V_BOOL,
    V_STR,
    V_UNIT,
    V_FUN,
    V_CONT, /* continuation closure (CPS machine) */
    V_PRIM,
    V_LIST,
} ValTag;

typedef struct Str {
    int len;
    char *data; /* NUL-terminated */
} Str;

typedef struct Prim Prim;
typedef struct PrimCtx PrimCtx;
typedef struct Value Value;

/* Invoke a user function value (used by higher-order primitives such as
 * map/filter/fold). Implemented by the evaluator that calls the primitive;
 * `ud` is the machine's private state. Returns false and sets errmsg on
 * error (the evaluator may already have recorded a more precise error). */
typedef bool (*PrimCallFn)(void *ud, Value head, Value *args, int nargs,
                           Value *out, char *errmsg, size_t errsz);

struct PrimCtx {
    bool errored;
    char errmsg[256];
    Gc *gc;          /* allocator for primitives that build heap values */
    PrimCallFn call; /* invoke a function value (map/filter/fold) */
    void *ud;        /* machine state handed to call */
};

/* primitive callback: computes from args; reports errors via PrimCtx */
typedef Value (*PrimFn)(Value *args, int nargs, PrimCtx *ctx);

struct Prim {
    const char *name;
    int arity; /* -1 = variadic */
    bool pure; /* safe to fold at compile time (no side effects) */
    bool needs_gc; /* allocates GC objects; not foldable, needs ctx->gc */
    PrimFn fn;
};

struct Value {
    ValTag tag;
    union {
        int64_t i;
        double f;
        bool b;
        Str *s;
        Closure *clo;
        const Prim *prim;
        List *l;
    } u;
};

/* Flat environment frame: a function activation's slots. Variable references
 * (depth, slot) resolve by walking `depth` parent pointers from the current
 * frame, then indexing slots[slot]. */
struct Frame {
    GObj hdr;            /* GC heap header */
    struct Frame *parent; /* enclosing frame */
    int nslots;
    Value slots[];       /* nslots entries */
};

struct Closure {
    GObj hdr;       /* GC heap header */
    void *body;     /* Anf* in eval_anf, CExp* in eval_cps */
    char **params;  /* V_FUN: IR params (continuation param is last);
                       V_CONT: unused (see cont_name) */
    int nparams;
    int nslots;     /* V_FUN: activation frame size (params + locals) */
    int kslot;      /* V_FUN: slot of the continuation param in the frame */
    Frame *frame;   /* captured lexical frame */
    const char *cont_name; /* V_CONT: the single parameter name; NULL for V_FUN */
    int rslot;      /* V_CONT: slot of the param in the captured frame */
};

/* Immutable list value: a GC- or arena-resident array of Values. GC lists
 * are allocated via gc_new_list() (gc.h); arena lists are used for literals
 * and for values deserialized from checkpoints/runtime files. */
struct List {
    GObj hdr;
    int len;
    Value items[]; /* flexible array of len Values */
};

extern const Value VALUE_NULL;

/* constructors */
Value v_int(int64_t i);
Value v_float(double f);
Value v_bool(bool b);
Value v_str(Arena *a, const char *s);
Value v_unit(void);
Value v_fun(Closure *c);
Value v_cont(Closure *c);
Value v_prim(const Prim *p);
Value v_list(List *l);
Value v_list_arena(Arena *a, const Value *items, int n);

const Prim *prim_lookup(const char *name);
const Prim *prim_table(int *count);
const char *binop_prim_name(int op);

bool value_truthy(Value v);              /* only for V_BOOL */
bool value_equal(Value a, Value b);
char *value_to_string(Arena *a, Value v);

#endif
