#ifndef YAC_VALUE_H
#define YAC_VALUE_H

#include <stdbool.h>
#include <stdint.h>

#include "arena.h"
#include "gcobj.h"

typedef struct Binding Binding;
typedef struct Anf Anf;

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

typedef struct Closure {
    GObj hdr;      /* GC heap header */
    void *body;    /* Anf* in eval_anf, CExp* in eval_cps */
    char **params; /* V_FUN: IR params (continuation param is last);
                      V_CONT: unused (see cont_name) */
    int nparams;
    Binding *env;  /* captured lexical environment */
    const char *cont_name; /* V_CONT: the single parameter name; NULL for V_FUN */
} Closure;

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
