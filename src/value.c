#include "config.h"
#include "value.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "bignum.h"
#include "gc.h"

const Value VALUE_NULL = {V_INT, {0}};

static int str_polyhash(const char *d, int n) {
    int h = 0;
    int i;
    for (i = 0; i < n; i++)
        h = (h * 131 + (unsigned char)d[i]) & 16777215;
    return h;
}

static int str_hash_of(Str *st) {
    if (st->hash_cached < 0)
        st->hash_cached = str_polyhash(st->data, st->len);
    return st->hash_cached;
}

static Value wrap_str(Str *st) {
    Value v = {V_STR, {0}};
    st->hash_cached = -1;
    v.u.s = st;
    return v;
}

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
    Str *st = (Str *)arena_alloc(a, sizeof(Str));
    st->len = (int)strlen(s);
    st->data = arena_strdup(a, s);
    return wrap_str(st);
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
    List *l = (List *)arena_alloc(a, sizeof(List));
    memset(l, 0, sizeof(List));
    l->len = n;
    l->cap = -1; /* immutable: must not list_push / realloc */
    if (n == 0) {
        l->items = NULL;
    } else {
        l->items = (Value *)arena_alloc(a, (size_t)n * sizeof(Value));
        if (items) memcpy(l->items, items, (size_t)n * sizeof(Value));
        else memset(l->items, 0, (size_t)n * sizeof(Value));
    }
    return v_list(l);
}

Value v_big(Bignum *b) {
    Value v = {V_BIG, {0}};
    v.u.big = b;
    return v;
}

Value v_bytes(Bytes *b) {
    Value v = {V_BYTES, {0}};
    v.u.bytes = b;
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
    fflush(stdout);
    free(s);
    return args[0];
}

/* ---- compiler / I/O primitives (for the self-hosted compiler) ---- */

static int saved_argc;
static char **saved_argv;

/* record the process args so argv()/argc() can serve them */
void yac_set_args(int argc, char **argv) {
    saved_argc = argc;
    saved_argv = argv;
}

static Value prim_argc(Value *args, int nargs, PrimCtx *ctx) {
    (void)args;
    (void)nargs;
    (void)ctx;
    return v_int(saved_argc);
}

/* bytes_new and argc are called as `bytes_new()` which yac parses as a single
 * `unit` argument; accept and ignore it. */

static Value prim_argv(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_INT) { bad_operands(ctx, "argv"); return VALUE_NULL; }
    long i = args[0].u.i;
    if (i < 0 || i >= saved_argc) { ctx->errored = true; strcpy(ctx->errmsg, "argv: index out of range"); return VALUE_NULL; }
    return v_str(ctx->a, saved_argv[i]);
}

static Value prim_exit(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    (void)ctx;
    int code = 0;
    if (args[0].tag == V_INT) code = (int)args[0].u.i;
    exit(code);
    return VALUE_NULL;
}

/* str-len(s) -> int */
static Value prim_strlen(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_STR) { ctx->errored = true; strcpy(ctx->errmsg, "str-len: expected a string"); return VALUE_NULL; }
    return v_int(args[0].u.s->len);
}

/* str_hash(s) -> int; same polyhash as former hash_str_loop (131, mask 24 bits). */
static Value prim_str_hash(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_STR) { ctx->errored = true; strcpy(ctx->errmsg, "str_hash: expected a string"); return VALUE_NULL; }
    return v_int(str_hash_of(args[0].u.s));
}

/* ident_hash(s) -> int; identity of interned string object (not content). */
static Value prim_ident_hash(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_STR) { ctx->errored = true; strcpy(ctx->errmsg, "ident_hash: expected a string"); return VALUE_NULL; }
    uintptr_t p = (uintptr_t)args[0].u.s;
    return v_int((int64_t)((p >> 3) & 16777215));
}

/* str-cat(a, b) -> string */
static Value prim_strcat(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_STR || args[1].tag != V_STR) { ctx->errored = true; strcpy(ctx->errmsg, "str-cat: expected strings"); return VALUE_NULL; }
    Str *a = args[0].u.s, *b = args[1].u.s;
    size_t n = (size_t)a->len + (size_t)b->len;
    char *buf = (char *)arena_alloc(ctx->a, n + 1);
    memcpy(buf, a->data, (size_t)a->len);
    memcpy(buf + a->len, b->data, (size_t)b->len);
    buf[n] = '\0';
    Str *st = (Str *)arena_alloc(ctx->a, sizeof(Str));
    st->len = (int)n;
    st->data = buf;
    return wrap_str(st);
}

/* str-slice(s, start, len) -> string */
static Value prim_strslice(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_STR) { ctx->errored = true; strcpy(ctx->errmsg, "str-slice: expected a string"); return VALUE_NULL; }
    if (args[1].tag != V_INT || args[2].tag != V_INT) { ctx->errored = true; strcpy(ctx->errmsg, "str-slice: start/len must be integers"); return VALUE_NULL; }
    Str *s = args[0].u.s;
    long start = args[1].u.i, len = args[2].u.i;
    if (start < 0) start = 0;
    if (start > s->len) start = s->len;
    if (len < 0) len = 0;
    if (start + len > s->len) len = s->len - start;
    char *buf = (char *)arena_alloc(ctx->a, (size_t)len + 1);
    memcpy(buf, s->data + start, (size_t)len);
    buf[len] = '\0';
    Str *st = (Str *)arena_alloc(ctx->a, sizeof(Str));
    st->len = (int)len;
    st->data = buf;
    return wrap_str(st);
}

/* str-ref(s, i) -> int (byte value at index) */
static Value prim_strref(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_STR || args[1].tag != V_INT) { ctx->errored = true; strcpy(ctx->errmsg, "str-ref: expected string and index"); return VALUE_NULL; }
    Str *s = args[0].u.s;
    long i = args[1].u.i;
    if (i < 0 || i >= s->len) { ctx->errored = true; strcpy(ctx->errmsg, "str-ref: index out of range"); return VALUE_NULL; }
    return v_int((unsigned char)s->data[i]);
}

/* bytes-new() -> mutable byte buffer */
static Value prim_bytes_new(Value *args, int nargs, PrimCtx *ctx) {
    (void)args;
    (void)nargs;
    return v_bytes(gc_new_bytes(ctx->gc));
}

/* list_new() -> empty growable list (for amortized push while compiling) */
static Value prim_list_new(Value *args, int nargs, PrimCtx *ctx) {
    (void)args;
    (void)nargs;
    return v_list(gc_new_list(ctx->gc, 0));
}

/* list_push(lst, x) -> unit. Amortized O(1) append; mutates lst. */
static Value prim_list_push(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_LIST) {
        ctx->errored = true;
        strcpy(ctx->errmsg, "list_push: expected a list");
        return VALUE_NULL;
    }
    List *l = args[0].u.l;
    if (l->cap < 0) {
        ctx->errored = true;
        strcpy(ctx->errmsg, "list_push: list is immutable");
        return VALUE_NULL;
    }
    if (l->len >= l->cap) {
        int ncap = l->cap ? l->cap * 2 : 8;
        Value *ni = (Value *)realloc(l->items, (size_t)ncap * sizeof(Value));
        if (!ni) {
            ctx->errored = true;
            strcpy(ctx->errmsg, "list_push: out of memory");
            return VALUE_NULL;
        }
        l->items = ni;
        l->cap = ncap;
    }
    l->items[l->len++] = args[1];
    return v_unit();
}

/* list_set(lst, i, x) -> unit. Mutates slot i (open-addressing tables). */
static Value prim_list_set(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_LIST) {
        ctx->errored = true;
        strcpy(ctx->errmsg, "list_set: expected a list");
        return VALUE_NULL;
    }
    List *l = args[0].u.l;
    if (l->cap < 0) {
        ctx->errored = true;
        strcpy(ctx->errmsg, "list_set: list is immutable");
        return VALUE_NULL;
    }
    if (args[1].tag != V_INT) {
        ctx->errored = true;
        strcpy(ctx->errmsg, "list_set: index must be an integer");
        return VALUE_NULL;
    }
    long long i = args[1].u.i;
    if (i < 0 || i >= l->len) {
        ctx->errored = true;
        snprintf(ctx->errmsg, sizeof(ctx->errmsg),
                 "list_set: index %lld out of range (len=%d)", i, l->len);
        return VALUE_NULL;
    }
    l->items[i] = args[2];
    return v_unit();
}

/* reverse(lst) -> new list, O(n) */
static Value prim_reverse(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_LIST) {
        ctx->errored = true;
        strcpy(ctx->errmsg, "reverse: expected a list");
        return VALUE_NULL;
    }
    List *src = args[0].u.l;
    List *out = gc_new_list(ctx->gc, src->len);
    for (int i = 0; i < src->len; i++)
        out->items[i] = src->items[src->len - 1 - i];
    return v_list(out);
}

/* zero-arity prims are called as name() which the parser desugars to a single
 * unit argument, so expose arity 1 and ignore the arg */

static void bytes_grow(Bytes *b, int need) {
    if (need <= b->cap) return;
    int ncap = b->cap ? b->cap * 2 : 16;
    while (ncap < need) ncap *= 2;
    b->data = (unsigned char *)realloc(b->data, (size_t)ncap);
    b->cap = ncap;
}

/* bytes-put(b, i, byte) -> unit (mutates; grows as needed) */
static Value prim_bytes_put(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_BYTES || args[1].tag != V_INT || args[2].tag != V_INT) {
        ctx->errored = true; strcpy(ctx->errmsg, "bytes-put: expected buffer, index, byte"); return VALUE_NULL;
    }
    Bytes *b = args[0].u.bytes;
    long i = args[1].u.i, v = args[2].u.i;
    if (i < 0 || v < 0 || v > 255) { ctx->errored = true; strcpy(ctx->errmsg, "bytes-put: index or byte out of range"); return VALUE_NULL; }
    bytes_grow(b, (int)i + 1);
    b->data[i] = (unsigned char)v;
    if (i >= b->len) b->len = (int)i + 1;
    return v_unit();
}

/* bytes-append(b, byte) -> unit (appends at end) */
static Value prim_bytes_append(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_BYTES || args[1].tag != V_INT) { ctx->errored = true; strcpy(ctx->errmsg, "bytes-append: expected buffer and byte"); return VALUE_NULL; }
    Bytes *b = args[0].u.bytes;
    long v = args[1].u.i;
    if (v < 0 || v > 255) { ctx->errored = true; strcpy(ctx->errmsg, "bytes-append: byte out of range"); return VALUE_NULL; }
    bytes_grow(b, b->len + 1);
    b->data[b->len++] = (unsigned char)v;
    return v_unit();
}

/* bytes_extend(dst, src) -> unit. Append all bytes of src onto dst (memcpy). */
static Value prim_bytes_extend(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_BYTES || args[1].tag != V_BYTES) {
        ctx->errored = true;
        strcpy(ctx->errmsg, "bytes_extend: expected two buffers");
        return VALUE_NULL;
    }
    Bytes *dst = args[0].u.bytes;
    Bytes *src = args[1].u.bytes;
    if (src->len <= 0) return v_unit();
    bytes_grow(dst, dst->len + src->len);
    memcpy(dst->data + dst->len, src->data, (size_t)src->len);
    dst->len += src->len;
    return v_unit();
}

/* bytes-len(b) -> int */
static Value prim_bytes_len(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_BYTES) { ctx->errored = true; strcpy(ctx->errmsg, "bytes-len: expected a buffer"); return VALUE_NULL; }
    return v_int(args[0].u.bytes->len);
}

/* bytes-ref(b, i) -> int */
static Value prim_bytes_ref(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_BYTES || args[1].tag != V_INT) { ctx->errored = true; strcpy(ctx->errmsg, "bytes-ref: expected buffer and index"); return VALUE_NULL; }
    Bytes *b = args[0].u.bytes;
    long i = args[1].u.i;
    if (i < 0 || i >= b->len) { ctx->errored = true; strcpy(ctx->errmsg, "bytes-ref: index out of range"); return VALUE_NULL; }
    return v_int(b->data[i]);
}

/* bytes->str(b) -> string (copies; binary safe via stored length) */
static Value prim_bytes_to_str(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_BYTES) { ctx->errored = true; strcpy(ctx->errmsg, "bytes->str: expected a buffer"); return VALUE_NULL; }
    Bytes *b = args[0].u.bytes;
    char *buf = (char *)arena_alloc(ctx->a, (size_t)b->len + 1);
    memcpy(buf, b->data, (size_t)b->len);
    buf[b->len] = '\0';
    Str *st = (Str *)arena_alloc(ctx->a, sizeof(Str));
    st->len = b->len;
    st->data = buf;
    return wrap_str(st);
}

/* str->bytes(s) -> buffer (copies bytes) */
static Value prim_str_to_bytes(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_STR) { ctx->errored = true; strcpy(ctx->errmsg, "str->bytes: expected a string"); return VALUE_NULL; }
    Str *s = args[0].u.s;
    Bytes *b = gc_new_bytes(ctx->gc);
    bytes_grow(b, s->len);
    memcpy(b->data, s->data, (size_t)s->len);
    b->len = s->len;
    return v_bytes(b);
}

/* read-file(path) -> string, or unit on failure */
static Value prim_read_file(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_STR) { ctx->errored = true; strcpy(ctx->errmsg, "read-file: expected a path string"); return VALUE_NULL; }
    const char *path = args[0].u.s->data;
    FILE *f = fopen(path, "rb");
    if (!f) return v_unit();
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return v_unit(); }
    long sz = ftell(f);
    rewind(f);
    char *buf = (char *)arena_alloc(ctx->a, (size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = '\0';
    fclose(f);
    Str *st = (Str *)arena_alloc(ctx->a, sizeof(Str));
    st->len = (int)got;
    st->data = buf;
    return wrap_str(st);
}

/* write-file(path, bytes-or-str) -> unit */
static Value prim_write_file(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_STR) { ctx->errored = true; strcpy(ctx->errmsg, "write-file: expected a path string"); return VALUE_NULL; }
    const char *path = args[0].u.s->data;
    const void *data;
    size_t len;
    if (args[1].tag == V_BYTES) { data = args[1].u.bytes->data; len = (size_t)args[1].u.bytes->len; }
    else if (args[1].tag == V_STR) { data = args[1].u.s->data; len = (size_t)args[1].u.s->len; }
    else { ctx->errored = true; strcpy(ctx->errmsg, "write-file: second arg must be bytes or string"); return VALUE_NULL; }
    FILE *f = fopen(path, "wb");
    if (!f) { ctx->errored = true; snprintf(ctx->errmsg, sizeof(ctx->errmsg), "write-file: cannot open '%s'", path); return VALUE_NULL; }
    fwrite(data, 1, len, f);
    fclose(f);
    return v_unit();
}

static int wait_status_to_rc(int st) {
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return st;
}

static Value v_str_buf(Arena *a, const char *data, size_t len) {
    if (len > (size_t)INT_MAX) len = (size_t)INT_MAX;
    char *buf = (char *)arena_alloc(a, len + 1);
    if (len) memcpy(buf, data, len);
    buf[len] = '\0';
    Str *st = (Str *)arena_alloc(a, sizeof(Str));
    st->len = (int)len;
    st->data = buf;
    return wrap_str(st);
}

typedef struct {
    char *p;
    size_t n, cap;
} CapBuf;

static bool capbuf_append(CapBuf *b, const char *src, size_t n) {
    if (n == 0) return true;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n) cap *= 2;
        char *p = (char *)realloc(b->p, cap);
        if (!p) return false;
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, src, n);
    b->n += n;
    return true;
}

/* Drain until EAGAIN or EOF. *eof set on read()==0. */
static bool drain_fd(int fd, CapBuf *b, bool *eof) {
    char tmp[4096];
    for (;;) {
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n > 0) {
            if (!capbuf_append(b, tmp, (size_t)n)) return false;
            continue;
        }
        if (n == 0) {
            *eof = true;
            return true;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        return false;
    }
}

/* Write until EAGAIN. *done after all bytes sent (caller then closes fd). */
static bool feed_fd(int fd, const char *src, size_t len, size_t *off, bool *done) {
    while (*off < len) {
        ssize_t n = write(fd, src + *off, len - *off);
        if (n > 0) {
            *off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        if (n < 0 && errno == EPIPE) {
            *done = true;
            return true;
        }
        return false;
    }
    *done = true;
    return true;
}

/* system(cmd) -> exit code. /bin/sh -c, inherit stdio (same as C system(3)). */
static Value prim_system(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_STR) {
        ctx->errored = true;
        strcpy(ctx->errmsg, "system: expected a command string");
        return VALUE_NULL;
    }
    int st = system(args[0].u.s->data);
    if (st == -1) {
        ctx->errored = true;
        strcpy(ctx->errmsg, "system: failed to invoke shell");
        return VALUE_NULL;
    }
    return v_int(wait_status_to_rc(st));
}

/* popen(cmd, stdin) -> [rc, stdout, stderr].
 * Same /bin/sh -c as system, but stdio is three pipes: stdin is the
 * given string (or bytes); stdout/stderr are collected. Compose a
 * pipeline in yac by feeding nth(r,1) into the next popen. */
static Value prim_popen(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_STR) {
        ctx->errored = true;
        strcpy(ctx->errmsg, "popen: expected a command string");
        return VALUE_NULL;
    }
    const char *in_data;
    size_t in_len;
    if (args[1].tag == V_STR) {
        in_data = args[1].u.s->data;
        in_len = (size_t)args[1].u.s->len;
    } else if (args[1].tag == V_BYTES) {
        in_data = (const char *)args[1].u.bytes->data;
        in_len = (size_t)args[1].u.bytes->len;
    } else {
        ctx->errored = true;
        strcpy(ctx->errmsg, "popen: stdin must be a string or bytes");
        return VALUE_NULL;
    }
    const char *cmd = args[0].u.s->data;
    int inp[2] = {-1, -1};
    int outp[2] = {-1, -1};
    int errp[2] = {-1, -1};
    if (pipe(inp) != 0 || pipe(outp) != 0 || pipe(errp) != 0) {
        if (inp[0] >= 0) { close(inp[0]); close(inp[1]); }
        if (outp[0] >= 0) { close(outp[0]); close(outp[1]); }
        if (errp[0] >= 0) { close(errp[0]); close(errp[1]); }
        ctx->errored = true;
        strcpy(ctx->errmsg, "popen: pipe failed");
        return VALUE_NULL;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(inp[0]); close(inp[1]);
        close(outp[0]); close(outp[1]);
        close(errp[0]); close(errp[1]);
        ctx->errored = true;
        strcpy(ctx->errmsg, "popen: fork failed");
        return VALUE_NULL;
    }
    if (pid == 0) {
        close(inp[1]);
        close(outp[0]);
        close(errp[0]);
        dup2(inp[0], STDIN_FILENO);
        dup2(outp[1], STDOUT_FILENO);
        dup2(errp[1], STDERR_FILENO);
        if (inp[0] != STDIN_FILENO) close(inp[0]);
        if (outp[1] != STDOUT_FILENO) close(outp[1]);
        if (errp[1] != STDERR_FILENO) close(errp[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(inp[0]);
    close(outp[1]);
    close(errp[1]);
    void (*old_pipe)(int) = signal(SIGPIPE, SIG_IGN);
    fcntl(outp[0], F_SETFL, O_NONBLOCK);
    fcntl(errp[0], F_SETFL, O_NONBLOCK);
    fcntl(inp[1], F_SETFL, O_NONBLOCK);

    CapBuf ob = {0}, eb = {0};
    bool out_eof = false, err_eof = false, in_done = (in_len == 0);
    size_t in_off = 0;
    if (in_done) {
        close(inp[1]);
        inp[1] = -1;
    }
    bool ok = true;
    while (ok && (!out_eof || !err_eof)) {
        struct pollfd pfd[3];
        pfd[0].fd = out_eof ? -1 : outp[0];
        pfd[0].events = POLLIN;
        pfd[1].fd = err_eof ? -1 : errp[0];
        pfd[1].events = POLLIN;
        pfd[2].fd = in_done ? -1 : inp[1];
        pfd[2].events = POLLOUT;
        int n = poll(pfd, 3, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (!out_eof && pfd[0].fd >= 0 && (pfd[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (!drain_fd(outp[0], &ob, &out_eof)) ok = false;
        }
        if (!err_eof && pfd[1].fd >= 0 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (!drain_fd(errp[0], &eb, &err_eof)) ok = false;
        }
        if (!in_done && pfd[2].fd >= 0 && (pfd[2].revents & (POLLOUT | POLLERR | POLLHUP))) {
            if (!feed_fd(inp[1], in_data, in_len, &in_off, &in_done)) ok = false;
            if (in_done && inp[1] >= 0) {
                close(inp[1]);
                inp[1] = -1;
            }
        }
    }
    if (inp[1] >= 0) close(inp[1]);
    close(outp[0]);
    close(errp[0]);
    signal(SIGPIPE, old_pipe);

    int st = 0;
    int wrc;
    do {
        wrc = waitpid(pid, &st, 0);
    } while (wrc < 0 && errno == EINTR);

    if (!ok || wrc < 0) {
        free(ob.p);
        free(eb.p);
        ctx->errored = true;
        strcpy(ctx->errmsg, "popen: failed to talk to child");
        return VALUE_NULL;
    }

    Value items[3];
    items[0] = v_int(wait_status_to_rc(st));
    items[1] = v_str_buf(ctx->a, ob.p ? ob.p : "", ob.n);
    items[2] = v_str_buf(ctx->a, eb.p ? eb.p : "", eb.n);
    free(ob.p);
    free(eb.p);
    return v_list_arena(ctx->a, items, 3);
}

/* bit ops on int64 */
static Value prim_bshl(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_INT || args[1].tag != V_INT) { bad_operands(ctx, "bshl"); return VALUE_NULL; }
    long long k = args[1].u.i;
    if (k < 0 || k >= 64) { ctx->errored = true; strcpy(ctx->errmsg, "bshl: shift out of range"); return VALUE_NULL; }
    return v_int(args[0].u.i << k);
}

static Value prim_bshr(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_INT || args[1].tag != V_INT) { bad_operands(ctx, "bshr"); return VALUE_NULL; }
    long long k = args[1].u.i;
    if (k < 0 || k >= 64) { ctx->errored = true; strcpy(ctx->errmsg, "bshr: shift out of range"); return VALUE_NULL; }
    return v_int((int64_t)((uint64_t)args[0].u.i >> k));
}

static Value prim_band(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_INT || args[1].tag != V_INT) { bad_operands(ctx, "band"); return VALUE_NULL; }
    return v_int(args[0].u.i & args[1].u.i);
}

static Value prim_bor(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_INT || args[1].tag != V_INT) { bad_operands(ctx, "bor"); return VALUE_NULL; }
    return v_int(args[0].u.i | args[1].u.i);
}

static Value prim_bxor(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_INT || args[1].tag != V_INT) { bad_operands(ctx, "bxor"); return VALUE_NULL; }
    return v_int(args[0].u.i ^ args[1].u.i);
}

static Value prim_bnot(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_INT) { bad_operands(ctx, "bnot"); return VALUE_NULL; }
    return v_int(~args[0].u.i);
}

static Value prim_int_to_str(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_INT) { bad_operands(ctx, "int_to_str"); return VALUE_NULL; }
    char buf[64];
    snprintf(buf, sizeof(buf), "%lld", (long long)args[0].u.i);
    return v_str(ctx->a, buf);
}

/* time_ms() -> monotonic milliseconds (boot epoch). time_ms(); ignores unit. */
static Value prim_time_ms(Value *args, int nargs, PrimCtx *ctx) {
    (void)args;
    (void)nargs;
    (void)ctx;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return v_int(0);
    return v_int((int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000L));
}

static Value prim_time_ns(Value *args, int nargs, PrimCtx *ctx) {
    (void)args;
    (void)nargs;
    (void)ctx;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return v_int(0);
    return v_int((int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec);
}

static Value prim_box_new(Value *args, int nargs, PrimCtx *ctx) {
    (void)args;
    (void)nargs;
    List *l = gc_new_list(ctx->gc, 1);
    l->items[0] = v_list(gc_new_list(ctx->gc, 0));
    return v_list(l);
}

static Value prim_box_get(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_LIST || args[0].u.l->len < 1) {
        ctx->errored = true;
        strcpy(ctx->errmsg, "box_get: expected a box");
        return VALUE_NULL;
    }
    return args[0].u.l->items[0];
}

static Value prim_box_set(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    if (args[0].tag != V_LIST || args[0].u.l->len < 1 || args[0].u.l->cap < 0) {
        ctx->errored = true;
        strcpy(ctx->errmsg, "box_set: expected a box");
        return VALUE_NULL;
    }
    args[0].u.l->items[0] = args[1];
    return v_int(0);
}

static List *prof_cell_list;

static Value prim_yac_prof_cell(Value *args, int nargs, PrimCtx *ctx) {
    (void)args;
    (void)nargs;
    if (!prof_cell_list) {
        prof_cell_list = gc_new_list(ctx->gc, 0);
        gc_push_root(ctx->gc, (GObj *)prof_cell_list);
    }
    return v_list(prof_cell_list);
}

/* time_str() -> local wall-clock "HH:MM:SS.mmm". Called as time_str(). */
static Value prim_time_str(Value *args, int nargs, PrimCtx *ctx) {
    (void)args;
    (void)nargs;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return v_str(ctx->a, "??:??:??.???");
    }
    struct tm tm;
    if (!localtime_r(&ts.tv_sec, &tm)) {
        return v_str(ctx->a, "??:??:??.???");
    }
    char buf[32];
    int n = (int)strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    if (n <= 0 || n >= (int)sizeof(buf) - 5) {
        return v_str(ctx->a, "??:??:??.???");
    }
    snprintf(buf + n, sizeof(buf) - (size_t)n, ".%03ld", ts.tv_nsec / 1000000L);
    return v_str(ctx->a, buf);
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
    {"argc", 1, true, false, prim_argc}, /* called as argc(); parser passes unit */
    {"argv", 1, true, true, prim_argv},
    {"exit", 1, false, false, prim_exit},
    {"str_len", 1, true, false, prim_strlen},
    {"str_hash", 1, true, false, prim_str_hash},
    {"ident_hash", 1, true, false, prim_ident_hash},
    {"str_cat", 2, true, true, prim_strcat},
    {"str_slice", 3, true, true, prim_strslice},
    {"str_ref", 2, true, false, prim_strref},
    {"bytes_new", 1, true, true, prim_bytes_new}, /* called as bytes_new(); parser passes unit */
    {"list_new", 1, true, true, prim_list_new},   /* called as list_new(); parser passes unit */
    {"list_push", 2, false, true, prim_list_push},
    {"list_set", 3, false, true, prim_list_set},
    {"list_rev", 1, true, true, prim_reverse},
    {"bytes_put", 3, false, true, prim_bytes_put},
    {"bytes_append", 2, false, true, prim_bytes_append},
    {"bytes_extend", 2, false, true, prim_bytes_extend},
    {"bytes_len", 1, true, false, prim_bytes_len},
    {"bytes_ref", 2, true, false, prim_bytes_ref},
    {"bytes_to_str", 1, true, true, prim_bytes_to_str},
    {"str_to_bytes", 1, true, true, prim_str_to_bytes},
    {"read_file", 1, false, true, prim_read_file},
    {"write_file", 2, false, true, prim_write_file},
    {"system", 1, false, false, prim_system},
    {"popen", 2, false, true, prim_popen},
    {"bshl", 2, true, false, prim_bshl},
    {"bshr", 2, true, false, prim_bshr},
    {"band", 2, true, false, prim_band},
    {"bor", 2, true, false, prim_bor},
    {"bxor", 2, true, false, prim_bxor},
    {"bnot", 1, true, false, prim_bnot},
    {"int_to_str", 1, true, true, prim_int_to_str},
    {"time_ms", 1, true, false, prim_time_ms}, /* called as time_ms(); parser passes unit */
    {"time_ns", 1, true, false, prim_time_ns},
    {"box_new", 1, true, true, prim_box_new},
    {"box_get", 1, true, true, prim_box_get},
    {"box_set", 2, false, true, prim_box_set},
    {"yac_prof_cell", 1, true, true, prim_yac_prof_cell},
    {"time_str", 1, true, true, prim_time_str}, /* called as time_str(); local HH:MM:SS */
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
    if (a.tag == V_STR && b.tag == V_STR) {
        if (a.u.s == b.u.s) return true;
        return a.u.s->len == b.u.s->len && memcmp(a.u.s->data, b.u.s->data, a.u.s->len) == 0;
    }
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
    case V_BYTES:
        sb_puts(s, "<bytes>");
        break;
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
