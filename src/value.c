#include "config.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bignum.h"
#include "gc.h"

const Value VALUE_NULL = {V_INT, {0}};

Value v_int(int64_t i) {
    Value v = {V_INT, {0}};
    v.u.i = i;
    return v;
}

Value v_float(double f) {
    Value v = {V_FLOAT, {0}};
    v.u.f = f;
    return v;
}

Value v_bool(bool b) {
    Value v = {V_BOOL, {0}};
    v.u.b = b;
    return v;
}

Value v_str(Arena *a, const char *s) {
    Value v = {V_STR, {0}};
    Str *st = (Str *)arena_alloc(a, sizeof(Str));
    st->len = (int)strlen(s);
    st->data = arena_strdup(a, s);
    v.u.s = st;
    return v;
}

Value v_unit(void) {
    Value v = {V_UNIT, {0}};
    return v;
}

Value v_fun(Closure *c) {
    Value v = {V_FUN, {0}};
    v.u.clo = c;
    return v;
}

Value v_cont(Closure *c) {
    Value v = {V_CONT, {0}};
    v.u.clo = c;
    return v;
}

Value v_prim(const Prim *p) {
    Value v = {V_PRIM, {0}};
    v.u.prim = p;
    return v;
}

Value v_list(List *l) {
    Value v = {V_LIST, {0}};
    v.u.l = l;
    return v;
}

Value v_list_arena(Arena *a, const Value *items, int n) {
    List *l = (List *)arena_alloc(a, sizeof(List) + (size_t)n * sizeof(Value));
    memset(l, 0, sizeof(List));
    l->len = n;
    if (items) memcpy(l->items, items, (size_t)n * sizeof(Value));
    return v_list(l);
}

Value v_big(Bignum *b) {
    Value v = {V_BIG, {0}};
    v.u.big = b;
    return v;
}

/* ---- numeric / comparison helpers ---- */

static bool num_pair(Value a, Value b, double *x, double *y, bool *both_int) {
    if (a.tag == V_INT && b.tag == V_INT) {
        *both_int = true;
        return true;
    }
    if ((a.tag == V_INT || a.tag == V_FLOAT) && (b.tag == V_INT || b.tag == V_FLOAT)) {
        *both_int = false;
        *x = a.tag == V_INT ? (double)a.u.i : a.u.f;
        *y = b.tag == V_INT ? (double)b.u.i : b.u.f;
        return true;
    }
    return false;
}

static void bad_operands(PrimCtx *ctx, const char *op) {
    ctx->errored = true;
    snprintf(ctx->errmsg, sizeof(ctx->errmsg), "operator '%s' applied to non-numeric operand", op);
}

/* Does `a` need bignum treatment (it is already a bignum)? */
static bool is_big(Value a) { return a.tag == V_BIG; }

/* Convert a numeric operand to a bignum (widening ints). Caller must root
 * the result if a GC can run before it is used. */
static Bignum *to_bignum(Gc *gc, Value a) {
    return a.tag == V_BIG ? a.u.big : bignum_from_i64(gc, a.u.i);
}

/* The result of a bignum operation: narrow back to int64 when it fits. */
static Value from_bignum(Bignum *r) {
    int64_t i;
    return bignum_to_i64(r, &i) ? v_int(i) : v_big(r);
}

/* ---- primitives ---- */

static Value prim_add(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    Value a = args[0], b = args[1];
    if (is_big(a) || is_big(b)) {
        Bignum *ba = to_bignum(ctx->gc, a);
        gc_push_root(ctx->gc, (GObj *)ba);
        Bignum *bb = to_bignum(ctx->gc, b);
        gc_push_root(ctx->gc, (GObj *)bb);
        Bignum *r = bignum_add(ctx->gc, ba, bb);
        gc_pop_root(ctx->gc);
        gc_pop_root(ctx->gc);
        return from_bignum(r);
    }
    bool bi;
    double x, y;
    if (!num_pair(a, b, &x, &y, &bi)) { bad_operands(ctx, "+"); return VALUE_NULL; }
    if (bi) {
        /* overflow detection via __int128 */
        __int128 s = (__int128)a.u.i + b.u.i;
        if (s > INT64_MAX || s < INT64_MIN) {
            Bignum *ba = bignum_from_i64(ctx->gc, a.u.i);
            gc_push_root(ctx->gc, (GObj *)ba);
            Bignum *bb = bignum_from_i64(ctx->gc, b.u.i);
            gc_push_root(ctx->gc, (GObj *)bb);
            Bignum *r = bignum_add(ctx->gc, ba, bb);
            gc_pop_root(ctx->gc);
            gc_pop_root(ctx->gc);
            return from_bignum(r);
        }
        return v_int((int64_t)s);
    }
    return v_float(x + y);
}

static Value prim_sub(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    Value a = args[0], b = args[1];
    if (is_big(a) || is_big(b)) {
        Bignum *ba = to_bignum(ctx->gc, a);
        gc_push_root(ctx->gc, (GObj *)ba);
        Bignum *bb = to_bignum(ctx->gc, b);
        gc_push_root(ctx->gc, (GObj *)bb);
        Bignum *r = bignum_sub(ctx->gc, ba, bb);
        gc_pop_root(ctx->gc);
        gc_pop_root(ctx->gc);
        return from_bignum(r);
    }
    bool bi;
    double x, y;
    if (!num_pair(a, b, &x, &y, &bi)) { bad_operands(ctx, "-"); return VALUE_NULL; }
    if (bi) {
        __int128 s = (__int128)a.u.i - b.u.i;
        if (s > INT64_MAX || s < INT64_MIN) {
            Bignum *ba = bignum_from_i64(ctx->gc, a.u.i);
            gc_push_root(ctx->gc, (GObj *)ba);
            Bignum *bb = bignum_from_i64(ctx->gc, b.u.i);
            gc_push_root(ctx->gc, (GObj *)bb);
            Bignum *r = bignum_sub(ctx->gc, ba, bb);
            gc_pop_root(ctx->gc);
            gc_pop_root(ctx->gc);
            return from_bignum(r);
        }
        return v_int((int64_t)s);
    }
    return v_float(x - y);
}

static Value prim_mul(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    Value a = args[0], b = args[1];
    if (is_big(a) || is_big(b)) {
        Bignum *ba = to_bignum(ctx->gc, a);
        gc_push_root(ctx->gc, (GObj *)ba);
        Bignum *bb = to_bignum(ctx->gc, b);
        gc_push_root(ctx->gc, (GObj *)bb);
        Bignum *r = bignum_mul(ctx->gc, ba, bb);
        gc_pop_root(ctx->gc);
        gc_pop_root(ctx->gc);
        return from_bignum(r);
    }
    bool bi;
    double x, y;
    if (!num_pair(a, b, &x, &y, &bi)) { bad_operands(ctx, "*"); return VALUE_NULL; }
    if (bi) {
        __int128 s = (__int128)a.u.i * b.u.i;
        if (s > INT64_MAX || s < INT64_MIN) {
            Bignum *ba = bignum_from_i64(ctx->gc, a.u.i);
            gc_push_root(ctx->gc, (GObj *)ba);
            Bignum *bb = bignum_from_i64(ctx->gc, b.u.i);
            gc_push_root(ctx->gc, (GObj *)bb);
            Bignum *r = bignum_mul(ctx->gc, ba, bb);
            gc_pop_root(ctx->gc);
            gc_pop_root(ctx->gc);
            return from_bignum(r);
        }
        return v_int((int64_t)s);
    }
    return v_float(x * y);
}

static Value prim_div(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    Value a = args[0], b = args[1];
    if (is_big(a) || is_big(b)) {
        Bignum *ba = to_bignum(ctx->gc, a);
        gc_push_root(ctx->gc, (GObj *)ba);
        Bignum *bb = to_bignum(ctx->gc, b);
        gc_push_root(ctx->gc, (GObj *)bb);
        bool ok;
        Bignum *r = bignum_div(ctx->gc, ba, bb, &ok);
        gc_pop_root(ctx->gc);
        gc_pop_root(ctx->gc);
        if (!ok) { ctx->errored = true; strcpy(ctx->errmsg, "division by zero"); return VALUE_NULL; }
        return from_bignum(r);
    }
    bool bi;
    double x, y;
    if (!num_pair(a, b, &x, &y, &bi)) { bad_operands(ctx, "/"); return VALUE_NULL; }
    if (bi) {
        if (b.u.i == 0) { ctx->errored = true; strcpy(ctx->errmsg, "division by zero"); return VALUE_NULL; }
        return v_int(a.u.i / b.u.i);
    }
    if (y == 0.0) { ctx->errored = true; strcpy(ctx->errmsg, "division by zero"); return VALUE_NULL; }
    return v_float(x / y);
}

static Value prim_mod(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    Value a = args[0], b = args[1];
    if (is_big(a) || is_big(b)) {
        Bignum *ba = to_bignum(ctx->gc, a);
        gc_push_root(ctx->gc, (GObj *)ba);
        Bignum *bb = to_bignum(ctx->gc, b);
        gc_push_root(ctx->gc, (GObj *)bb);
        bool ok;
        Bignum *r = bignum_mod(ctx->gc, ba, bb, &ok);
        gc_pop_root(ctx->gc);
        gc_pop_root(ctx->gc);
        if (!ok) { ctx->errored = true; strcpy(ctx->errmsg, "division by zero"); return VALUE_NULL; }
        return from_bignum(r);
    }
    if (a.tag != V_INT || b.tag != V_INT) {
        ctx->errored = true;
        snprintf(ctx->errmsg, sizeof(ctx->errmsg), "operator '%%' requires integer operands");
        return VALUE_NULL;
    }
    if (b.u.i == 0) { ctx->errored = true; strcpy(ctx->errmsg, "division by zero"); return VALUE_NULL; }
    return v_int(a.u.i % b.u.i);
}

static Value prim_eq(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    (void)ctx;
    return v_bool(value_equal(args[0], args[1]));
}

static Value prim_ne(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    (void)ctx;
    return v_bool(!value_equal(args[0], args[1]));
}

/* Compare two numeric values (int/float/bignum); -1/0/+1. */
static int num_cmp(Value a, Value b, PrimCtx *ctx, const char *op) {
    if (is_big(a) || is_big(b)) {
        if (is_big(a) && is_big(b)) return bignum_cmp(a.u.big, b.u.big);
        if (is_big(a) && b.tag == V_INT) return bignum_cmp_i64(a.u.big, b.u.i);
        if (a.tag == V_INT && is_big(b)) return -bignum_cmp_i64(b.u.big, a.u.i);
        /* bignum vs float: widen bignum to double */
        double x = is_big(a) ? bignum_to_double(a.u.big) : (double)a.u.i;
        double y = is_big(b) ? bignum_to_double(b.u.big) : (double)b.u.i;
        return x < y ? -1 : x > y ? 1 : 0;
    }
    bool bi;
    double x, y;
    if (!num_pair(a, b, &x, &y, &bi)) { bad_operands(ctx, op); return 0; }
    if (bi) return a.u.i < b.u.i ? -1 : a.u.i > b.u.i ? 1 : 0;
    return x < y ? -1 : x > y ? 1 : 0;
}

static Value prim_lt(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    return v_bool(num_cmp(args[0], args[1], ctx, "<") < 0);
}

static Value prim_le(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    return v_bool(num_cmp(args[0], args[1], ctx, "<=") <= 0);
}

static Value prim_gt(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    return v_bool(num_cmp(args[0], args[1], ctx, ">") > 0);
}

static Value prim_ge(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    return v_bool(num_cmp(args[0], args[1], ctx, ">=") >= 0);
}

static Value prim_and(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_BOOL || args[1].tag != V_BOOL) { bad_operands(ctx, "and"); return VALUE_NULL; }
    return v_bool(args[0].u.b && args[1].u.b);
}

static Value prim_or(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_BOOL || args[1].tag != V_BOOL) { bad_operands(ctx, "or"); return VALUE_NULL; }
    return v_bool(args[0].u.b || args[1].u.b);
}

static Value prim_not(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_BOOL) { bad_operands(ctx, "not"); return VALUE_NULL; }
    return v_bool(!args[0].u.b);
}

static Value prim_print(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    (void)ctx;
    char *s = value_to_string(NULL, args[0]);
    fputs(s, stdout);
    fputc('\n', stdout);
    return args[0];
}

/* ---- lists ---- */

static Value prim_cons(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[1].tag != V_LIST) {
        ctx->errored = true;
        snprintf(ctx->errmsg, sizeof(ctx->errmsg),
                 "cons: second argument must be a list");
        return VALUE_NULL;
    }
    List *src = args[1].u.l;
    List *out = gc_new_list(ctx->gc, src->len + 1);
    out->items[0] = args[0];
    memcpy(out->items + 1, src->items, (size_t)src->len * sizeof(Value));
    return v_list(out);
}

static Value prim_append(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_LIST || args[1].tag != V_LIST) {
        ctx->errored = true;
        snprintf(ctx->errmsg, sizeof(ctx->errmsg),
                 "append: both arguments must be lists");
        return VALUE_NULL;
    }
    List *a = args[0].u.l, *b = args[1].u.l;
    List *out = gc_new_list(ctx->gc, a->len + b->len);
    memcpy(out->items, a->items, (size_t)a->len * sizeof(Value));
    memcpy(out->items + a->len, b->items, (size_t)b->len * sizeof(Value));
    return v_list(out);
}

static Value prim_len(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    (void)ctx;
    if (args[0].tag != V_LIST) {
        ctx->errored = true;
        snprintf(ctx->errmsg, sizeof(ctx->errmsg),
                 "len: argument must be a list");
        return VALUE_NULL;
    }
    return v_int(args[0].u.l->len);
}

static Value prim_nth(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_LIST) {
        ctx->errored = true;
        snprintf(ctx->errmsg, sizeof(ctx->errmsg),
                 "nth: first argument must be a list");
        return VALUE_NULL;
    }
    if (args[1].tag != V_INT) {
        ctx->errored = true;
        snprintf(ctx->errmsg, sizeof(ctx->errmsg),
                 "nth: index must be an integer");
        return VALUE_NULL;
    }
    List *l = args[0].u.l;
    long long i = args[1].u.i;
    if (i < 0 || i >= l->len) {
        ctx->errored = true;
        snprintf(ctx->errmsg, sizeof(ctx->errmsg),
                 "nth: index %lld out of range (len=%d)", i, l->len);
        return VALUE_NULL;
    }
    return l->items[i];
}

/* drop(l, n): the list without the first n elements; clamped */
static Value prim_drop(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_LIST) {
        ctx->errored = true;
        snprintf(ctx->errmsg, sizeof(ctx->errmsg),
                 "drop: first argument must be a list");
        return VALUE_NULL;
    }
    if (args[1].tag != V_INT) {
        ctx->errored = true;
        snprintf(ctx->errmsg, sizeof(ctx->errmsg),
                 "drop: count must be an integer");
        return VALUE_NULL;
    }
    List *l = args[0].u.l;
    long long n = args[1].u.i;
    if (n <= 0) return args[0];
    if (n >= l->len) return v_list(gc_new_list(ctx->gc, 0));
    List *out = gc_new_list(ctx->gc, l->len - (int)n);
    memcpy(out->items, l->items + n, (size_t)out->len * sizeof(Value));
    return v_list(out);
}

static Value prim_map(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[1].tag != V_LIST) {
        ctx->errored = true;
        snprintf(ctx->errmsg, sizeof(ctx->errmsg),
                 "map: second argument must be a list");
        return VALUE_NULL;
    }
    List *src = args[1].u.l;
    List *out = gc_new_list(ctx->gc, src->len);
    gc_push_root(ctx->gc, (GObj *)out);
    char err[64];
    for (int i = 0; i < src->len; i++) {
        Value item = src->items[i];
        if (!ctx->call(ctx->ud, args[0], &item, 1, &out->items[i], err, sizeof(err))) {
            ctx->errored = true;
            snprintf(ctx->errmsg, sizeof(ctx->errmsg), "map: %s", err);
            gc_pop_root(ctx->gc);
            return VALUE_NULL;
        }
    }
    gc_pop_root(ctx->gc);
    return v_list(out);
}

static Value prim_filter(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[1].tag != V_LIST) {
        ctx->errored = true;
        snprintf(ctx->errmsg, sizeof(ctx->errmsg),
                 "filter: second argument must be a list");
        return VALUE_NULL;
    }
    List *src = args[1].u.l;
    List *out = gc_new_list(ctx->gc, src->len);
    gc_push_root(ctx->gc, (GObj *)out);
    char err[64];
    int m = 0;
    for (int i = 0; i < src->len; i++) {
        Value item = src->items[i];
        Value keep;
        if (!ctx->call(ctx->ud, args[0], &item, 1, &keep, err, sizeof(err))) {
            ctx->errored = true;
            snprintf(ctx->errmsg, sizeof(ctx->errmsg), "filter: %s", err);
            gc_pop_root(ctx->gc);
            return VALUE_NULL;
        }
        if (keep.tag != V_BOOL) {
            ctx->errored = true;
            snprintf(ctx->errmsg, sizeof(ctx->errmsg),
                     "filter: predicate must return a boolean");
            gc_pop_root(ctx->gc);
            return VALUE_NULL;
        }
        if (keep.u.b) out->items[m++] = item;
    }
    gc_pop_root(ctx->gc);
    List *res = gc_new_list(ctx->gc, m);
    memcpy(res->items, out->items, (size_t)m * sizeof(Value));
    return v_list(res);
}

static Value prim_foldl(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[2].tag != V_LIST) {
        ctx->errored = true;
        snprintf(ctx->errmsg, sizeof(ctx->errmsg),
                 "foldl: third argument must be a list");
        return VALUE_NULL;
    }
    List *xs = args[2].u.l;
    Value acc = args[1];
    char err[64];
    Value call_args[2];
    for (int i = 0; i < xs->len; i++) {
        gc_push_value(ctx->gc, acc);
        call_args[0] = acc;
        call_args[1] = xs->items[i];
        if (!ctx->call(ctx->ud, args[0], call_args, 2, &acc, err, sizeof(err))) {
            ctx->errored = true;
            snprintf(ctx->errmsg, sizeof(ctx->errmsg), "foldl: %s", err);
            gc_pop_root(ctx->gc);
            return VALUE_NULL;
        }
        gc_pop_root(ctx->gc); /* old acc */
    }
    return acc;
}

static Value prim_foldr(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[2].tag != V_LIST) {
        ctx->errored = true;
        snprintf(ctx->errmsg, sizeof(ctx->errmsg),
                 "foldr: third argument must be a list");
        return VALUE_NULL;
    }
    List *xs = args[2].u.l;
    Value acc = args[1];
    char err[64];
    Value call_args[2];
    for (int i = xs->len - 1; i >= 0; i--) {
        gc_push_value(ctx->gc, acc);
        call_args[0] = xs->items[i];
        call_args[1] = acc;
        if (!ctx->call(ctx->ud, args[0], call_args, 2, &acc, err, sizeof(err))) {
            ctx->errored = true;
            snprintf(ctx->errmsg, sizeof(ctx->errmsg), "foldr: %s", err);
            gc_pop_root(ctx->gc);
            return VALUE_NULL;
        }
        gc_pop_root(ctx->gc); /* old acc */
    }
    return acc;
}

static const Prim PRIMS[] = {
    {"+", 2, true, true, prim_add},
    {"-", 2, true, true, prim_sub},
    {"*", 2, true, true, prim_mul},
    {"/", 2, true, true, prim_div},
    {"%", 2, true, true, prim_mod},
    {"==", 2, true, false, prim_eq},
    {"!=", 2, true, false, prim_ne},
    {"<", 2, true, false, prim_lt},
    {"<=", 2, true, false, prim_le},
    {">", 2, true, false, prim_gt},
    {">=", 2, true, false, prim_ge},
    {"and", 2, true, false, prim_and},
    {"or", 2, true, false, prim_or},
    {"not", 1, true, false, prim_not},
    {"print", 1, false, false, prim_print},
    {"cons", 2, true, true, prim_cons},
    {"append", 2, true, true, prim_append},
    {"len", 1, true, false, prim_len},
    {"nth", 2, true, false, prim_nth},
    {"drop", 2, true, true, prim_drop},
    {"map", 2, true, true, prim_map},
    {"filter", 2, true, true, prim_filter},
    {"foldl", 3, true, true, prim_foldl},
    {"foldr", 3, true, true, prim_foldr},
};

const Prim *prim_lookup(const char *name) {
    for (size_t i = 0; i < sizeof(PRIMS) / sizeof(PRIMS[0]); i++) {
        if (strcmp(PRIMS[i].name, name) == 0) return &PRIMS[i];
    }
    return NULL;
}

const Prim *prim_table(int *count) {
    *count = (int)(sizeof(PRIMS) / sizeof(PRIMS[0]));
    return PRIMS;
}

bool value_truthy(Value v) {
    return v.tag == V_BOOL && v.u.b;
}

bool value_equal(Value a, Value b) {
    if (a.tag == V_BIG || b.tag == V_BIG) {
        /* numeric equality across int/float/bignum */
        PrimCtx tmp = {0};
        return num_cmp(a, b, &tmp, "==") == 0;
    }
    if (a.tag == V_INT && b.tag == V_INT) return a.u.i == b.u.i;
    if (a.tag == V_INT && b.tag == V_FLOAT) return (double)a.u.i == b.u.f;
    if (a.tag == V_FLOAT && b.tag == V_INT) return a.u.f == (double)b.u.i;
    if (a.tag == V_FLOAT && b.tag == V_FLOAT) return a.u.f == b.u.f;
    if (a.tag == V_BOOL && b.tag == V_BOOL) return a.u.b == b.u.b;
    if (a.tag == V_UNIT && b.tag == V_UNIT) return true;
    if (a.tag == V_STR && b.tag == V_STR)
        return a.u.s->len == b.u.s->len && memcmp(a.u.s->data, b.u.s->data, a.u.s->len) == 0;
    if (a.tag == V_LIST && b.tag == V_LIST) {
        if (a.u.l->len != b.u.l->len) return false;
        for (int i = 0; i < a.u.l->len; i++)
            if (!value_equal(a.u.l->items[i], b.u.l->items[i])) return false;
        return true;
    }
    return false;
}

typedef struct {
    char *buf;
    size_t len, cap;
} SB;

static void sb_put(SB *s, const char *t, size_t n) {
    if (s->len + n + 1 > s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 32;
        while (ncap < s->len + n + 1) ncap *= 2;
        s->buf = (char *)realloc(s->buf, ncap);
        s->cap = ncap;
    }
    memcpy(s->buf + s->len, t, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

static void sb_puts(SB *s, const char *t) { sb_put(s, t, strlen(t)); }

static void v_to_sb(SB *s, Value v) {
    char buf[64];
    switch (v.tag) {
    case V_INT: snprintf(buf, sizeof(buf), "%lld", (long long)v.u.i); sb_puts(s, buf); break;
    case V_BIG: {
        char *t = bignum_to_string(v.u.big);
        sb_puts(s, t);
        free(t);
        break;
    }
    case V_FLOAT: snprintf(buf, sizeof(buf), "%g", v.u.f); sb_puts(s, buf); break;
    case V_BOOL: sb_puts(s, v.u.b ? "true" : "false"); break;
    case V_STR: sb_puts(s, v.u.s->data); break;
    case V_UNIT: sb_puts(s, "()"); break;
    case V_FUN: sb_puts(s, "<fun>"); break;
    case V_CONT: sb_puts(s, "<cont>"); break;
    case V_PRIM: snprintf(buf, sizeof(buf), "<prim:%s>", v.u.prim->name); sb_puts(s, buf); break;
    case V_LIST: {
        List *l = v.u.l;
        sb_put(s, "[", 1);
        for (int i = 0; i < l->len; i++) {
            if (i) sb_puts(s, ", ");
            v_to_sb(s, l->items[i]);
        }
        sb_put(s, "]", 1);
        break;
    }
    }
}

char *value_to_string(Arena *a, Value v) {
    SB s = {0};
    v_to_sb(&s, v);
    if (a) {
        char *r = arena_strdup(a, s.buf ? s.buf : "");
        free(s.buf);
        return r;
    }
    return s.buf ? s.buf : strdup("");
}
