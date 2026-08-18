#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anf.h"
#include "arena.h"
#include "cps.h"
#include "eval_anf.h"
#include "eval_cps.h"
#include "gc.h"
#include "lexer.h"
#include "parser.h"
#include "uncps.h"
#include "value.h"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [options] file.yac\n"
            "options:\n"
            "  --cps            run via the CPS interpreter\n"
            "  --dump-anf       print the ANF and exit\n"
            "  --dump-cps       print ANF->CPS and exit\n"
            "  --both           run both interpreters and compare\n"
            "  --uncps          run ANF->CPS->ANF (un-CPS round trip) and execute\n"
            "  --dump-uncps     print the un-CPS'd ANF and exit\n"
            "  --opt            simplify the CPS IR (constant folding, eta)\n"
            "  --ast            dump the parsed AST and exit\n"
            "  --no-gc          disable garbage collection (arena-style growth)\n"
            "  --limit-nodes N  abort when live objects exceed N (0 = unlimited)\n",
            prog);
}

typedef struct {
    LexResult lx;
    char *src;
    Arena a;
    Gc gc;
    bool have_lx;
} Res;

static void cleanup(Res *r) {
    if (r->have_lx) free(r->lx.toks);
    free(r->src);
    arena_free_all(&r->a);
    gc_free(&r->gc);
}

int main(int argc, char **argv) {
    bool dump_ast = false, dump_anf = false, dump_cps = false;
    bool cps_mode = false, both = false, uncps_mode = false, dump_uncps = false;
    bool no_gc = false, do_opt = false;
    size_t limit_nodes = 0;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ast") == 0) dump_ast = true;
        else if (strcmp(argv[i], "--dump-anf") == 0) dump_anf = true;
        else if (strcmp(argv[i], "--dump-cps") == 0) dump_cps = true;
        else if (strcmp(argv[i], "--cps") == 0) cps_mode = true;
        else if (strcmp(argv[i], "--both") == 0) both = true;
        else if (strcmp(argv[i], "--uncps") == 0) uncps_mode = true;
        else if (strcmp(argv[i], "--dump-uncps") == 0) dump_uncps = true;
        else if (strcmp(argv[i], "--opt") == 0) do_opt = true;
        else if (strcmp(argv[i], "--no-gc") == 0) no_gc = true;
        else if (strcmp(argv[i], "--limit-nodes") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--limit-nodes requires a number\n");
                return 2;
            }
            limit_nodes = strtoul(argv[++i], NULL, 10);
        }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        } else if (!file) file = argv[i];
        else {
            fprintf(stderr, "unexpected argument: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }
    if (!file) {
        usage(argv[0]);
        return 2;
    }

    Res r;
    memset(&r, 0, sizeof(r));
    r.src = read_file(file);
    if (!r.src) {
        fprintf(stderr, "cannot read file: %s\n", file);
        return 2;
    }
    arena_init(&r.a, 1 << 20);
    gc_init(&r.gc, 1 << 20);
    const char *th = getenv("YAC_GC_THRESHOLD");
    if (th && *th) r.gc.threshold = (size_t)strtoul(th, NULL, 10);
    r.gc.enabled = !no_gc;
    r.gc.max_objs = limit_nodes;

    r.lx = lex_program(r.src, &r.a);
    r.have_lx = true;
    if (r.lx.error) {
        fprintf(stderr, "%s\n", r.lx.error);
        cleanup(&r);
        return 1;
    }

    ParseResult pr = parse_program(r.lx.toks, r.lx.n, &r.a);
    if (pr.error) {
        fprintf(stderr, "%s\n", pr.error);
        cleanup(&r);
        return 1;
    }

    if (dump_ast) {
        ast_dump(pr.program, 0);
        cleanup(&r);
        return 0;
    }

    Anf *anf = NULL;
    char *errmsg = NULL;
    if (!ast_to_anf(pr.program, &r.a, &anf, &errmsg)) {
        fprintf(stderr, "error: %s\n", errmsg);
        cleanup(&r);
        return 1;
    }

    if (dump_anf) {
        anf_dump(anf, 0);
        cleanup(&r);
        return 0;
    }

    CExp *cps = NULL;
    if (cps_mode || both || dump_cps || uncps_mode || dump_uncps) {
        if (!anf_to_cps(anf, &r.a, &cps, &errmsg)) {
            fprintf(stderr, "error: %s\n", errmsg);
            cleanup(&r);
            return 1;
        }
        if (do_opt) cps = cps_simplify(cps, &r.a);
        if (dump_cps) {
            cps_dump(cps, 0);
            cleanup(&r);
            return 0;
        }
    }

    if (uncps_mode || dump_uncps) {
        Anf *anf2 = NULL;
        if (!cps_to_anf(cps, &r.a, &anf2, &errmsg)) {
            fprintf(stderr, "error: %s\n", errmsg);
            cleanup(&r);
            return 1;
        }
        if (dump_uncps) {
            anf_dump(anf2, 0);
            cleanup(&r);
            return 0;
        }
        Value result;
        if (eval_anf_run(anf2, &r.a, &result, &errmsg, &r.gc) != 0) {
            fprintf(stderr, "runtime error: %s\n", errmsg);
            cleanup(&r);
            return 1;
        }
        char *out = value_to_string(&r.a, result);
        printf("%s\n", out);
        cleanup(&r);
        return 0;
    }

    if (both) {
        Value r1, r2;
        char *e1 = NULL, *e2 = NULL;
        int rc1 = eval_anf_run(anf, &r.a, &r1, &e1, &r.gc);
        if (rc1 == 0) gc_push_value(&r.gc, r1); /* protect r1 while the CPS machine collects */
        int rc2 = eval_cps_run(cps, &r.a, &r2, &e2, &r.gc);
        if (rc1 == 0) gc_pop_root(&r.gc);
        if (rc1 != 0 || rc2 != 0) {
            fprintf(stderr, "runtime error: %s%s%s\n",
                    rc1 ? e1 : "", rc1 && rc2 ? " | " : "", rc2 ? e2 : "");
            cleanup(&r);
            return 1;
        }
        if (!value_equal(r1, r2)) {
            char *s1 = value_to_string(&r.a, r1);
            char *s2 = value_to_string(&r.a, r2);
            fprintf(stderr, "mismatch: ANF=%s CPS=%s\n", s1, s2);
            cleanup(&r);
            return 1;
        }
        char *out = value_to_string(&r.a, r1);
        printf("%s\n", out);
        cleanup(&r);
        return 0;
    }

    if (cps_mode) {
        Value result;
        if (eval_cps_run(cps, &r.a, &result, &errmsg, &r.gc) != 0) {
            fprintf(stderr, "runtime error: %s\n", errmsg);
            cleanup(&r);
            return 1;
        }
        char *out = value_to_string(&r.a, result);
        printf("%s\n", out);
        cleanup(&r);
        return 0;
    }

    Value result;
    if (eval_anf_run(anf, &r.a, &result, &errmsg, &r.gc) != 0) {
        fprintf(stderr, "runtime error: %s\n", errmsg);
        cleanup(&r);
        return 1;
    }

    char *out = value_to_string(&r.a, result);
    printf("%s\n", out);

    cleanup(&r);
    return 0;
}