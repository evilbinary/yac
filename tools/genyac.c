/* genyac: deterministic random Yac program generator for property tests.
 *
 * Generates a fully parenthesized, scoping-correct program so it parses and
 * runs without unbound variables. `let` does not put its name in scope inside
 * its own bound expression, so programs are non-recursive and always
 * terminate. With --callcc it occasionally emits the classic escape pattern
 *   (let k = callcc (fun (k) -> k) in throw k E)
 * which always runs to completion under the CPS machine.
 *
 * Usage: genyac [--callcc] [seed]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint64_t rng;

static uint64_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static int pick(int n) { return n <= 0 ? 0 : (int)(rnd() % (unsigned)n); }

static char *scope[64];
static int nscope = 0;
static int namec = 0;

static const char *fresh(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "v%d", namec++);
    return strdup(buf);
}

static void spush(const char *s) { scope[nscope++] = (char *)s; }
static void spop(void) { if (nscope > 0) nscope--; }
static const char *spick(void) { return nscope ? scope[pick(nscope)] : NULL; }

static void gen_expr(int depth);

static void gen_atom(void) {
    switch (pick(6)) {
    case 0: printf("%lld", (long long)((int64_t)rnd() % 201 - 100)); break;
    case 1: printf("%g", (double)(rnd() % 1000) / 40.0 - 10.0); break;
    case 2: printf("%s", pick(2) ? "true" : "false"); break;
    case 3: printf("()"); break;
    case 4: printf("\"str\""); break;
    default: {
        const char *v = spick();
        if (v) printf("%s", v);
        else printf("42");
    }
    }
}

/* prints (fun (p0, p1) -> body) and returns the arity */
static int gen_lambda(int depth) {
    int np = 1 + pick(2);
    int base = nscope;
    for (int i = 0; i < np; i++) spush(fresh());
    printf("(fun (");
    for (int i = 0; i < np; i++) printf("%s%s", i ? ", " : "", scope[base + i]);
    printf(") -> ");
    gen_expr(depth + 1);
    printf(")");
    nscope = base;
    return np;
}

static void gen_app(int depth) {
    int np = gen_lambda(depth);
    for (int i = 0; i < np; i++) {
        printf(" (");
        gen_expr(depth + 1);
        printf(")");
    }
}

static void gen_binop(int depth) {
    static const char *ops[] = {"+", "-", "*", "/", "%", "==", "!=",
                                "<", "<=", ">", ">=", "and", "or"};
    printf("(");
    gen_expr(depth + 1);
    printf(" %s ", ops[pick(13)]);
    gen_expr(depth + 1);
    printf(")");
}

static void gen_if(int depth) {
    printf("(if ");
    gen_expr(depth + 1);
    printf(" then ");
    gen_expr(depth + 1);
    printf(" else ");
    gen_expr(depth + 1);
    printf(")");
}

static void gen_let(int depth) {
    printf("(let ");
    const char *nm = fresh();
    printf("%s = ", nm);
    gen_expr(depth + 1); /* bound: name NOT in scope (no recursion) */
    spush(nm);
    printf(" in ");
    gen_expr(depth + 1);
    spop();
    printf(")");
}

static void gen_not(int depth) {
    printf("(not ");
    gen_expr(depth + 1);
    printf(")");
}

static void gen_print(int depth) {
    printf("(print ");
    gen_expr(depth + 1);
    printf(")");
}

static void gen_expr(int depth) {
    if (depth > 4) {
        gen_atom();
        return;
    }
    switch (pick(10)) {
    case 0:
    case 1:
        gen_atom();
        break;
    case 2:
        gen_lambda(depth);
        break;
    case 3:
        gen_app(depth);
        break;
    case 4:
        gen_binop(depth);
        break;
    case 5:
        gen_if(depth);
        break;
    case 6:
        gen_let(depth);
        break;
    case 7:
        gen_not(depth);
        break;
    case 8:
        gen_print(depth);
        break;
    default:
        printf("42");
        break;
    }
}

static void gen_callcc(void) {
    const char *k = fresh();
    printf("(let %s = callcc (fun (%s) -> %s) in throw %s (", k, k, k, k);
    gen_expr(0);
    printf("))");
}

int main(int argc, char **argv) {
    uint64_t seed = 1;
    int use_callcc = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--callcc") == 0) use_callcc = 1;
        else seed = strtoull(argv[i], NULL, 10);
    }
    rng = seed ? seed : 1;
    if (use_callcc && pick(3) == 0) gen_callcc();
    else gen_expr(0);
    printf("\n");
    return 0;
}