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
typedef struct Bignum Bignum;
typedef struct Bytes Bytes;

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
    V_BIG,  /* Bignum: arbitrary-precision integer */
    V_BYTES, /* mutable byte buffer (compiler codegen) */
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
    Arena *a;        /* arena for string/array literals the prim creates */
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
        Bignum *big;
        Bytes *bytes;
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
    const char *debug_name; /* let-bound name for --prof-out; NULL if anonymous */
};

/* List value: len elements in a separately allocated items array (like Bytes).
 * GC lists come from gc_new_list(); arena lists from v_list_arena().
 * `list_push` grows items with amortized O(1) append (bootstrap compiler hot path).
 * `cap < 0` means immutable (arena/literal): list_push must not be used. */
struct List {
    GObj hdr;
    int len;
    int cap;     /* capacity of items[]; -1 = immutable (do not realloc) */
    Value *items;
};

/* Arbitrary-precision signed integer: base-2^32 little-endian digits.
 * sign is -1, 0, or +1; a zero value has sign 0 and ndigits 1 (digit 0).
 * GC-resident (allocated via gc_new_bignum in gc.h). */
typedef struct Bignum {
    GObj hdr;
    int sign;
    int ndigits;
    uint32_t digits[]; /* little-endian base-2^32 magnitude digits */
} Bignum;

/* Growable byte buffer used for machine-code generation. `data` is a
 * separate malloc block of `cap` bytes; `len` is the used length.
 * GC-resident; the data block is freed when the object is collected. */
typedef struct Bytes {
    GObj hdr;
    int len;
    int cap;
    unsigned char *data;
} Bytes;

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
Value v_big(Bignum *b);
Value v_bytes(Bytes *b);

const Prim *prim_lookup(const char *name);
const Prim *prim_table(int *count);
const char *binop_prim_name(int op);
void yac_set_args(int argc, char **argv);

bool value_truthy(Value v);              /* only for V_BOOL */
bool value_equal(Value a, Value b);
char *value_to_string(Arena *a, Value v);

#endif
