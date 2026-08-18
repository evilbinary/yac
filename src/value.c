#include "value.h"

#include <stdio.h>
#include <string.h>

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

/* ---- primitives ---- */

static Value prim_add(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    Value a = args[0], b = args[1];
    bool bi;
    double x, y;
    if (!num_pair(a, b, &x, &y, &bi)) { bad_operands(ctx, "+"); return VALUE_NULL; }
    if (bi) {
        if (a.u.i > 0 && b.u.i > 0 && a.u.i > INT64_MAX - b.u.i) { ctx->errored = true; strcpy(ctx->errmsg, "integer overflow"); return VALUE_NULL; }
        return v_int(a.u.i + b.u.i);
    }
    return v_float(x + y);
}

static Value prim_sub(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    Value a = args[0], b = args[1];
    bool bi;
    double x, y;
    if (!num_pair(a, b, &x, &y, &bi)) { bad_operands(ctx, "-"); return VALUE_NULL; }
    return bi ? v_int(a.u.i - b.u.i) : v_float(x - y);
}

static Value prim_mul(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    Value a = args[0], b = args[1];
    bool bi;
    double x, y;
    if (!num_pair(a, b, &x, &y, &bi)) { bad_operands(ctx, "*"); return VALUE_NULL; }
    return bi ? v_int(a.u.i * b.u.i) : v_float(x * y);
}

static Value prim_div(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    Value a = args[0], b = args[1];
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

static Value prim_lt(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    Value a = args[0], b = args[1];
    bool bi;
    double x, y;
    if (!num_pair(a, b, &x, &y, &bi)) { bad_operands(ctx, "<"); return VALUE_NULL; }
    return v_bool(bi ? a.u.i < b.u.i : x < y);
}

static Value prim_le(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    Value a = args[0], b = args[1];
    bool bi;
    double x, y;
    if (!num_pair(a, b, &x, &y, &bi)) { bad_operands(ctx, "<="); return VALUE_NULL; }
    return v_bool(bi ? a.u.i <= b.u.i : x <= y);
}

static Value prim_gt(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    Value a = args[0], b = args[1];
    bool bi;
    double x, y;
    if (!num_pair(a, b, &x, &y, &bi)) { bad_operands(ctx, ">"); return VALUE_NULL; }
    return v_bool(bi ? a.u.i > b.u.i : x > y);
}

static Value prim_ge(Value *args, int nargs, PrimCtx *ctx) {
    (void)nargs;
    Value a = args[0], b = args[1];
    bool bi;
    double x, y;
    if (!num_pair(a, b, &x, &y, &bi)) { bad_operands(ctx, ">="); return VALUE_NULL; }
    return v_bool(bi ? a.u.i >= b.u.i : x >= y);
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

static const Prim PRIMS[] = {
    {"+", 2, true, prim_add},
    {"-", 2, true, prim_sub},
    {"*", 2, true, prim_mul},
    {"/", 2, true, prim_div},
    {"%", 2, true, prim_mod},
    {"==", 2, true, prim_eq},
    {"!=", 2, true, prim_ne},
    {"<", 2, true, prim_lt},
    {"<=", 2, true, prim_le},
    {">", 2, true, prim_gt},
    {">=", 2, true, prim_ge},
    {"and", 2, true, prim_and},
    {"or", 2, true, prim_or},
    {"not", 1, true, prim_not},
    {"print", 1, false, prim_print},
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
    if (a.tag == V_INT && b.tag == V_INT) return a.u.i == b.u.i;
    if (a.tag == V_INT && b.tag == V_FLOAT) return (double)a.u.i == b.u.f;
    if (a.tag == V_FLOAT && b.tag == V_INT) return a.u.f == (double)b.u.i;
    if (a.tag == V_FLOAT && b.tag == V_FLOAT) return a.u.f == b.u.f;
    if (a.tag == V_BOOL && b.tag == V_BOOL) return a.u.b == b.u.b;
    if (a.tag == V_UNIT && b.tag == V_UNIT) return true;
    if (a.tag == V_STR && b.tag == V_STR)
        return a.u.s->len == b.u.s->len && memcmp(a.u.s->data, b.u.s->data, a.u.s->len) == 0;
    return false;
}

char *value_to_string(Arena *a, Value v) {
    char buf[256];
    switch (v.tag) {
    case V_INT: snprintf(buf, sizeof(buf), "%lld", (long long)v.u.i); break;
    case V_FLOAT: snprintf(buf, sizeof(buf), "%g", v.u.f); break;
    case V_BOOL: snprintf(buf, sizeof(buf), "%s", v.u.b ? "true" : "false"); break;
    case V_STR:
        if (a) return arena_strdup(a, v.u.s->data);
        return v.u.s->data;
    case V_UNIT: snprintf(buf, sizeof(buf), "()"); break;
    case V_FUN: snprintf(buf, sizeof(buf), "<fun>"); break;
    case V_CONT: snprintf(buf, sizeof(buf), "<cont>"); break;
    case V_PRIM: snprintf(buf, sizeof(buf), "<prim:%s>", v.u.prim->name); break;
    }
    if (a) return arena_strdup(a, buf);
    return strdup(buf);
}
