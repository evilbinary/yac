#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "anf.h"
#include "arena.h"
#include "ckpt.h"
#include "cps.h"
#include "eval_anf.h"
#include "eval_cps.h"
#include "gc.h"
#include "lexer.h"
#include "parser.h"
#include "rtio.h"
#include "scheme.h"
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

/* ---- REPL ---- */

typedef struct {
    const char **names;
    Value *vals;
    int count;
    int cap;
    Arena *a;
    Gc *gc;
    bool cps;
    bool scheme;
} Globals;

static void globals_add(Globals *g, const char *name, Value v) {
    if (g->count >= g->cap) {
        g->cap = g->cap ? g->cap * 2 : 16;
        g->names = (const char **)realloc(g->names, (size_t)g->cap * sizeof(char *));
        g->vals = (Value *)realloc(g->vals, (size_t)g->cap * sizeof(Value));
    }
    g->names[g->count] = arena_strdup(g->a, name);
    g->vals[g->count] = v;
    gc_push_value(g->gc, v); /* keep the global alive across collections */
    g->count++;
}

/* Walk the top-level let chain of an ANF program; every binding with a slot
 * >= from_slot and a non-temporary name is a new global. */
static void collect_globals(const Anf *n, int from_slot, const Frame *tmp,
                            const char **names, Value *vals, int *count) {
    for (;;) {
        if (!n) return;
        switch (n->kind) {
        case N_LET:
            if (n->u.let.slot >= from_slot && n->u.let.name[0] != '#') {
                names[*count] = n->u.let.name;
                vals[*count] = tmp->slots[n->u.let.slot];
                (*count)++;
            }
            n = n->u.let.body;
            break;
        case N_LET_CALL:
            if (n->u.call.slot >= from_slot && n->u.call.name[0] != '#') {
                names[*count] = n->u.call.name;
                vals[*count] = tmp->slots[n->u.call.slot];
                (*count)++;
            }
            n = n->u.call.body;
            break;
        case N_LET_CALLCC:
            if (n->u.callcc.slot >= from_slot && n->u.callcc.name[0] != '#') {
                names[*count] = n->u.callcc.name;
                vals[*count] = tmp->slots[n->u.callcc.slot];
                (*count)++;
            }
            n = n->u.callcc.body;
            break;
        default:
            return;
        }
    }
}

/* parse + normalize + evaluate `src` against the persistent globals.
 * Returns true and sets *out; *new_count is the number of globals added. */
static bool repl_eval(Globals *g, const char *src, Value *out, int *new_count,
                      char **errmsg, bool scheme_mode) {
    Arena *a = g->a;
    char *yac_src = NULL;
    if (scheme_mode) {
        yac_src = scheme_to_yac(src, errmsg);
        if (!yac_src) return false;
        src = yac_src;
    }
    LexResult lx = lex_program(src, a);
    if (lx.error) {
        *errmsg = lx.error;
        free(lx.toks);
        free(yac_src);
        return false;
    }
    ParseResult pr = parse_program(lx.toks, lx.n, a);
    free(lx.toks);
    if (pr.error) {
        *errmsg = pr.error;
        free(yac_src);
        return false;
    }
    Anf *anf = NULL;
    int top = 0;
    if (!ast_to_anf_prelude(pr.program, a, &anf, &top, errmsg, g->names, g->count)) {
        free(yac_src);
        return false;
    }
    Frame *tmp = NULL;
    CExp *cps = NULL;
    int ctop = 0;
    if (g->cps) {
        if (!anf_to_cps(anf, top, a, &cps, &ctop, errmsg)) { free(yac_src); return false; }
        tmp = gc_new_frame(g->gc, ctop);
    } else {
        tmp = gc_new_frame(g->gc, top);
    }
    for (int i = 0; i < g->count; i++) tmp->slots[i] = g->vals[i];
    bool ok = true;
    if (g->cps) {
        if (eval_cps_run_in(cps, tmp, a, out, errmsg, g->gc) != 0) ok = false;
    } else if (eval_anf_run_in(anf, tmp, a, out, errmsg, g->gc) != 0) {
        ok = false;
    }
    if (ok) {
        const char *nn[YAC_MAX_GLOBALS];
        Value nv[YAC_MAX_GLOBALS];
        int nc = 0;
        collect_globals(anf, g->count, tmp, nn, nv, &nc);
        for (int i = 0; i < nc; i++) globals_add(g, nn[i], nv[i]);
        if (new_count) *new_count = nc;
    }
    free(yac_src);
    return ok;
}

static int repl_loop(Globals *g) {
    char line[YAC_REPL_LINE_MAX];
    for (;;) {
        printf("yac> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;
        if (strcmp(line, ":q") == 0 || strcmp(line, ":quit") == 0) break;
        char *errmsg = NULL;
        Value result;
        int nc = 0;
        if (repl_eval(g, line, &result, &nc, &errmsg, g->scheme)) {
            if (nc > 0) {
                char *s = value_to_string(g->a, g->vals[g->count - 1]);
                printf("%s = %s\n", g->names[g->count - 1], s);
            } else {
                char *s = value_to_string(g->a, result);
                printf("%s\n", s);
            }
        } else {
            printf("error: %s\n", errmsg);
        }
    }
    return 0;
}

/* ---- checkpoints ---- */

static long ckpt_target = -1;
static const char *ckpt_path = NULL;

static int ckpt_hook(const AnfState *st, long step) {
    if (step == ckpt_target) {
        const char *p = ckpt_path ? ckpt_path : "yac.ckpt";
        if (ckpt_dump(st, step, p) != 0) {
            fprintf(stderr, "checkpoint dump failed\n");
        } else {
            fprintf(stderr, "checkpointed at step %ld -> %s\n", step, p);
        }
        return 1; /* pause */
    }
    return 0;
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
            "  --limit-nodes N  abort when live objects exceed N (0 = unlimited)\n"
            "  --dump-rt FILE   serialize the compiled runtime (ANF) to FILE\n"
            "  --load-rt FILE   load a runtime FILE instead of parsing source\n"
            "  --repl           interactive loop with persistent globals\n"
            "  --scheme         translate a Scheme subset file to yac, then run it\n"
            "                   (combine with --repl for a Scheme REPL)\n"
            "  --checkpoint-at N  dump the machine state at step N and pause\n"
            "  --resume FILE    load a checkpoint and continue execution\n",
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
    yac_set_args(argc, argv);
    bool dump_ast = false, dump_anf = false, dump_cps = false;
    bool cps_mode = false, both = false, uncps_mode = false, dump_uncps = false;
    bool no_gc = false, do_opt = false, repl_mode = false, scheme_mode = false;
    const char *dump_rt = NULL, *load_rt = NULL, *resume_path = NULL;
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
        else if (strcmp(argv[i], "--repl") == 0) repl_mode = true;
        else if (strcmp(argv[i], "--scheme") == 0) scheme_mode = true;
        else if (strcmp(argv[i], "--checkpoint-at") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--checkpoint-at requires a number\n");
                return 2;
            }
            ckpt_target = strtol(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--resume") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--resume requires a file\n");
                return 2;
            }
            resume_path = argv[++i];
        }
        else if (strcmp(argv[i], "--dump-rt") == 0 || strcmp(argv[i], "--load-rt") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires a file\n", argv[i]);
                return 2;
            }
            if (argv[i][2] == 'd') dump_rt = argv[++i];
            else load_rt = argv[++i];
        }
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
    if (!file && !load_rt && !repl_mode && !resume_path) {
        usage(argv[0]);
        return 2;
    }

    Res r;
    memset(&r, 0, sizeof(r));
    arena_init(&r.a, YAC_ARENA_BLOCK_SIZE);
    gc_init(&r.gc, YAC_GC_THRESHOLD);
    const char *th = getenv("YAC_GC_THRESHOLD");
    if (th && *th) r.gc.threshold = (size_t)strtoul(th, NULL, 10);
    r.gc.enabled = !no_gc;
    r.gc.max_objs = limit_nodes;

    if (repl_mode) {
        Globals g = {NULL, NULL, 0, 0, &r.a, &r.gc, cps_mode, scheme_mode};
        if (file) {
            char *src = read_file(file);
            if (src) {
                char *errmsg2 = NULL;
                Value result;
                int nc = 0;
                if (repl_eval(&g, src, &result, &nc, &errmsg2, scheme_mode)) {
                    char *s = value_to_string(&r.a, result);
                    printf("%s\n", s);
                } else {
                    fprintf(stderr, "error: %s\n", errmsg2);
                }
                free(src);
            } else {
                fprintf(stderr, "cannot read file: %s\n", file);
            }
        }
        repl_loop(&g);
        free(g.names);
        free(g.vals);
        cleanup(&r);
        return 0;
    }

    if (resume_path) {
        const Anf *root = NULL, *node = NULL;
        Frame *env = NULL;
        CFrame *cframe = NULL;
        long step = 0;
        char *ckpt_err = NULL;
        if (!ckpt_resume(resume_path, &r.a, &r.gc, &root, &node, &env, &cframe,
                         &step, &ckpt_err)) {
            fprintf(stderr, "error: %s\n", ckpt_err);
            cleanup(&r);
            return 1;
        }
        Value result;
        char *res_err = NULL;
        int rc = eval_anf_resume(root, node, env, cframe, step, &r.a, &result,
                                 &res_err, &r.gc);
        if (rc != 0) {
            fprintf(stderr, "runtime error: %s\n", res_err);
            cleanup(&r);
            return 1;
        }
        char *out = value_to_string(&r.a, result);
        printf("%s\n", out);
        cleanup(&r);
        return 0;
    }

    Anf *anf = NULL;
    int top_nslots = 0;
    char *errmsg = NULL;

    if (load_rt) {
        /* load a serialized runtime instead of parsing source */
        if (!anf_read_file(load_rt, &r.a, &anf, &top_nslots, &errmsg)) {
            fprintf(stderr, "error: %s\n", errmsg);
            cleanup(&r);
            return 1;
        }
    } else {
        r.src = read_file(file);
        if (!r.src) {
            fprintf(stderr, "cannot read file: %s\n", file);
            cleanup(&r);
            return 2;
        }
        if (scheme_mode) {
            char *errmsg2 = NULL;
            char *yac_src = scheme_to_yac(r.src, &errmsg2);
            if (!yac_src) {
                fprintf(stderr, "error: %s\n", errmsg2);
                free(errmsg2);
                cleanup(&r);
                return 1;
            }
            free(r.src);
            r.src = yac_src;
        }
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

        if (!ast_to_anf(pr.program, &r.a, &anf, &top_nslots, &errmsg)) {
            fprintf(stderr, "error: %s\n", errmsg);
            cleanup(&r);
            return 1;
        }
    }

    if (dump_rt) {
        FILE *f = fopen(dump_rt, "w");
        if (!f) {
            fprintf(stderr, "cannot write runtime file: %s\n", dump_rt);
            cleanup(&r);
            return 1;
        }
        fprintf(f, "(rt %d ", top_nslots);
        anf_write(anf, f);
        fputs(")\n", f);
        fclose(f);
        cleanup(&r);
        return 0;
    }

    if (dump_anf) {
        anf_dump(anf, 0);
        cleanup(&r);
        return 0;
    }

    CExp *cps = NULL;
    int cps_top = 0;
    if (cps_mode || both || dump_cps || uncps_mode || dump_uncps) {
        if (!anf_to_cps(anf, top_nslots, &r.a, &cps, &cps_top, &errmsg)) {
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
        int top2 = 0;
        if (!cps_to_anf(cps, &r.a, &anf2, &top2, &errmsg)) {
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
        if (eval_anf_run(anf2, top2, &r.a, &result, &errmsg, &r.gc) != 0) {
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
        int rc1 = eval_anf_run(anf, top_nslots, &r.a, &r1, &e1, &r.gc);
        if (rc1 == 0) gc_push_value(&r.gc, r1); /* protect r1 while the CPS machine collects */
        int rc2 = eval_cps_run(cps, cps_top, &r.a, &r2, &e2, &r.gc);
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
        if (eval_cps_run(cps, cps_top, &r.a, &result, &errmsg, &r.gc) != 0) {
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
    int rc;
    if (ckpt_target >= 0) {
        yac_ckpt_hook = ckpt_hook;
        rc = eval_anf_run(anf, top_nslots, &r.a, &result, &errmsg, &r.gc);
        yac_ckpt_hook = NULL;
    } else {
        rc = eval_anf_run(anf, top_nslots, &r.a, &result, &errmsg, &r.gc);
    }
    if (rc == 2) {
        cleanup(&r);
        return 0;
    }
    if (rc != 0) {
        fprintf(stderr, "runtime error: %s\n", errmsg);
        cleanup(&r);
        return 1;
    }

    char *out = value_to_string(&r.a, result);
    printf("%s\n", out);

    cleanup(&r);
    return 0;
}