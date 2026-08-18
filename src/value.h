#ifndef YAC_VALUE_H
#define YAC_VALUE_H

#include <stdbool.h>
#include <stdint.h>

#include "arena.h"
#include "gcobj.h"

typedef struct Binding Binding;
typedef struct Anf Anf;
typedef struct Frame Frame;
typedef struct Closure Closure;

typedef enum {
    V_INT,
    V_FLOAT,
    V_BOOL,
    V_STR,
    V_UNIT,
    V_FUN,
    V_CONT, /* continuation closure (CPS machine) */
    V_PRIM,
} ValTag;

typedef struct Str {
    int len;
    char *data; /* NUL-terminated */
} Str;

typedef struct Prim Prim;
typedef struct PrimCtx {
    bool errored;
    char errmsg[256];
} PrimCtx;

/* primitive callback: computes from args; reports errors via PrimCtx */
typedef struct Value (*PrimFn)(struct Value *args, int nargs, PrimCtx *ctx);

struct Prim {
    const char *name;
    int arity; /* -1 = variadic */
    bool pure; /* safe to fold at compile time (no side effects) */
    PrimFn fn;
};

typedef struct Value {
    ValTag tag;
    union {
        int64_t i;
        double f;
        bool b;
        Str *s;
        Closure *clo;
        const Prim *prim;
    } u;
} Value;

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

const Prim *prim_lookup(const char *name);
const Prim *prim_table(int *count);
const char *binop_prim_name(int op);

bool value_truthy(Value v);              /* only for V_BOOL */
bool value_equal(Value a, Value b);
char *value_to_string(Arena *a, Value v);

#endif
