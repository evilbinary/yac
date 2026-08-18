#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anf.h"
#include "arena.h"
#include "eval_anf.h"
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
            "  --cps         run via the CPS interpreter (not yet implemented)\n"
            "  --dump-anf    print the ANF and exit\n"
            "  --dump-cps    print ANF->CPS and exit (not yet implemented)\n"
            "  --both        run both interpreters and compare (not yet implemented)\n"
            "  --ast         dump the parsed AST and exit\n",
            prog);
}

int main(int argc, char **argv) {
    bool dump_ast = false, dump_anf = false, cps_mode = false, both = false;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ast") == 0) dump_ast = true;
        else if (strcmp(argv[i], "--dump-anf") == 0) dump_anf = true;
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

    if (cps_mode || both) {
        fprintf(stderr, "--cps / --both not yet implemented (M2)\n");
        free(lx.toks);
        free(src);
        arena_free_all(&a);
        return 1;
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