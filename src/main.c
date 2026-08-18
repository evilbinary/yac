#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anf.h"
#include "arena.h"
#include "cps.h"
#include "eval_anf.h"
#include "eval_cps.h"
#include "lexer.h"
#include "parser.h"
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
            "  --cps         run via the CPS interpreter\n"
            "  --dump-anf    print the ANF and exit\n"
            "  --dump-cps    print ANF->CPS and exit\n"
            "  --both        run both interpreters and compare\n"
            "  --ast         dump the parsed AST and exit\n",
            prog);
}

int main(int argc, char **argv) {
    bool dump_ast = false, dump_anf = false, dump_cps = false, cps_mode = false, both = false;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ast") == 0) dump_ast = true;
        else if (strcmp(argv[i], "--dump-anf") == 0) dump_anf = true;
        else if (strcmp(argv[i], "--dump-cps") == 0) dump_cps = true;
        else if (strcmp(argv[i], "--cps") == 0) cps_mode = true;
        else if (strcmp(argv[i], "--both") == 0) both = true;
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

    char *src = read_file(file);
    if (!src) {
        fprintf(stderr, "cannot read file: %s\n", file);
        return 2;
    }

    Arena a;
    arena_init(&a, 1 << 20);

    LexResult lx = lex_program(src, &a);
    if (lx.error) {
        fprintf(stderr, "%s\n", lx.error);
        free(lx.toks);
        free(src);
        arena_free_all(&a);
        return 1;
    }

    ParseResult pr = parse_program(lx.toks, lx.n, &a);
    if (pr.error) {
        fprintf(stderr, "%s\n", pr.error);
        free(lx.toks);
        free(src);
        arena_free_all(&a);
        return 1;
    }

    if (dump_ast) {
        ast_dump(pr.program, 0);
        free(lx.toks);
        free(src);
        arena_free_all(&a);
        return 0;
    }

    Anf *anf = NULL;
    char *errmsg = NULL;
    if (!ast_to_anf(pr.program, &a, &anf, &errmsg)) {
        fprintf(stderr, "error: %s\n", errmsg);
        free(lx.toks);
        free(src);
        arena_free_all(&a);
        return 1;
    }

    if (dump_anf) {
        anf_dump(anf, 0);
        free(lx.toks);
        free(src);
        arena_free_all(&a);
        return 0;
    }

    CExp *cps = NULL;
    if (cps_mode || both || dump_cps) {
        if (!anf_to_cps(anf, &a, &cps, &errmsg)) {
            fprintf(stderr, "error: %s\n", errmsg);
            free(lx.toks);
            free(src);
            arena_free_all(&a);
            return 1;
        }
        if (dump_cps) {
            cps_dump(cps, 0);
            free(lx.toks);
            free(src);
            arena_free_all(&a);
            return 0;
        }
    }

    if (both) {
        Value r1, r2;
        char *e1 = NULL, *e2 = NULL;
        int rc1 = eval_anf_run(anf, &a, &r1, &e1);
        int rc2 = eval_cps_run(cps, &a, &r2, &e2);
        if (rc1 != 0 || rc2 != 0) {
            fprintf(stderr, "runtime error: %s%s%s\n",
                    rc1 ? e1 : "", rc1 && rc2 ? " | " : "", rc2 ? e2 : "");
            free(lx.toks);
            free(src);
            arena_free_all(&a);
            return 1;
        }
        if (!value_equal(r1, r2)) {
            char *s1 = value_to_string(&a, r1);
            char *s2 = value_to_string(&a, r2);
            fprintf(stderr, "mismatch: ANF=%s CPS=%s\n", s1, s2);
            free(lx.toks);
            free(src);
            arena_free_all(&a);
            return 1;
        }
        char *out = value_to_string(&a, r1);
        printf("%s\n", out);
        free(lx.toks);
        free(src);
        arena_free_all(&a);
        return 0;
    }

    if (cps_mode) {
        Value result;
        if (eval_cps_run(cps, &a, &result, &errmsg) != 0) {
            fprintf(stderr, "runtime error: %s\n", errmsg);
            free(lx.toks);
            free(src);
            arena_free_all(&a);
            return 1;
        }
        char *out = value_to_string(&a, result);
        printf("%s\n", out);
        free(lx.toks);
        free(src);
        arena_free_all(&a);
        return 0;
    }

    Value result;
    if (eval_anf_run(anf, &a, &result, &errmsg) != 0) {
        fprintf(stderr, "runtime error: %s\n", errmsg);
        free(lx.toks);
        free(src);
        arena_free_all(&a);
        return 1;
    }

    char *out = value_to_string(&a, result);
    printf("%s\n", out);

    free(lx.toks);
    free(src);
    arena_free_all(&a);
    return 0;
}