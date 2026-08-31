#include "parser.h"

#include "oscompat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const Token *toks;
    int n;
    int pos;
    Arena *a;
    char *error;
} Parser;

static Ast *parse_expr(Parser *p);

static const Token *peek(const Parser *p) { return &p->toks[p->pos]; }

static const Token *advance(Parser *p) {
    const Token *t = &p->toks[p->pos];
    if (t->kind != TK_EOF) p->pos++;
    return t;
}

static bool at(const Parser *p, TokKind k) { return peek(p)->kind == k; }

static bool eat(Parser *p, TokKind k) {
    if (at(p, k)) {
        advance(p);
        return true;
    }
    return false;
}

static void p_err(Parser *p, const char *fmt, ...) {
    if (p->error) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    const Token *t = peek(p);
    char msg[1280];
    snprintf(msg, sizeof(msg), "%d:%d: %s (near %s)", t->line, t->col, buf, tok_kind_name(t->kind));
    p->error = arena_strdup(p->a, msg);
}

static Ast *mk(Parser *p, AstKind k, int line, int col) {
    Ast *n = (Ast *)arena_alloc(p->a, sizeof(Ast));
    memset(n, 0, sizeof(Ast));
    n->kind = k;
    n->line = line;
    n->col = col;
    return n;
}

static Ast *mk_unit(Parser *p, int line, int col) {
    Ast *n = mk(p, A_UNIT, line, col);
    return n;
}

static Ast *mk_bin(Parser *p, int op, Ast *lhs, Ast *rhs, int line, int col) {
    Ast *n = mk(p, A_BINOP, line, col);
    n->u.bin.op = op;
    n->u.bin.lhs = lhs;
    n->u.bin.rhs = rhs;
    return n;
}

static Ast *mk_let(Parser *p, const char *name, Ast *bound, Ast *body, int line, int col) {
    Ast *n = mk(p, A_LET, line, col);
    n->u.let.name = (char *)name;
    n->u.let.bound = bound;
    n->u.let.body = body;
    return n;
}

static Ast *mk_fun(Parser *p, char **params, int nparams, Ast *body, int line, int col) {
    Ast *n = mk(p, A_FUN, line, col);
    n->u.fun.params = params;
    n->u.fun.nparams = nparams;
    n->u.fun.body = body;
    return n;
}

static bool atom_start(TokKind k) {
    switch (k) {
    case TK_INT:
    case TK_FLOAT:
    case TK_STRING:
    case TK_TRUE:
    case TK_FALSE:
    case TK_UNIT:
    case TK_IDENT:
    case TK_LPAREN:
    case TK_KW_FUN:
    case TK_LBRACKET:
        return true;
    default:
        return false;
    }
}

/* parses '(' x, y, z ')' -- caller has consumed nothing. */
static bool parse_param_list(Parser *p, char ***params_out, int *nparams_out) {
    if (!eat(p, TK_LPAREN)) {
        *params_out = NULL;
        *nparams_out = 0;
        return true;
    }
    char **params = NULL;
    int cnt = 0, cap = 0;
    while (!at(p, TK_RPAREN)) {
        if (!at(p, TK_IDENT)) {
            p_err(p, "expected parameter name");
            free(params);
            return false;
        }
        const Token *pt = advance(p);
        if (cnt == cap) {
            cap = cap ? cap * 2 : 4;
            params = (char **)realloc(params, (size_t)cap * sizeof(char *));
        }
        params[cnt++] = (char *)pt->text;
        if (!eat(p, TK_COMMA)) break;
    }
    if (!eat(p, TK_RPAREN)) {
        p_err(p, "expected ')' to close parameter list");
        free(params);
        return false;
    }
    *params_out = params;
    *nparams_out = cnt;
    return true;
}

static char **params_to_arena(Parser *p, char **params, int nparams) {
    char **parr = (char **)arena_alloc(p->a, (size_t)nparams * sizeof(char *));
    memcpy(parr, params, (size_t)nparams * sizeof(char *));
    return parr;
}

static Ast *parse_atom(Parser *p) {
    const Token *t = peek(p);
    switch (t->kind) {
    case TK_INT: {
        advance(p);
        if (t->big) {
            Ast *n = mk(p, A_BIG, t->line, t->col);
            n->u.sval = (char *)t->text;
            return n;
        }
        Ast *n = mk(p, A_INT, t->line, t->col);
        n->u.ival = t->ival;
        return n;
    }
    case TK_FLOAT: {
        advance(p);
        Ast *n = mk(p, A_FLOAT, t->line, t->col);
        n->u.fval = t->fval;
        return n;
    }
    case TK_STRING: {
        advance(p);
        Ast *n = mk(p, A_STR, t->line, t->col);
        n->u.sval = (char *)t->text;
        return n;
    }
    case TK_TRUE: {
        advance(p);
        Ast *n = mk(p, A_BOOL, t->line, t->col);
        n->u.bval = true;
        return n;
    }
    case TK_FALSE: {
        advance(p);
        Ast *n = mk(p, A_BOOL, t->line, t->col);
        n->u.bval = false;
        return n;
    }
    case TK_UNIT: {
        advance(p);
        return mk(p, A_UNIT, t->line, t->col);
    }
    case TK_IDENT: {
        advance(p);
        Ast *n = mk(p, A_VAR, t->line, t->col);
        n->u.name = (char *)t->text;
        return n;
    }
    case TK_LPAREN: {
        advance(p);
        Ast *e = parse_expr(p);
        if (!eat(p, TK_RPAREN)) p_err(p, "expected ')'");
        return e;
    }
    case TK_KW_FUN: {
        advance(p);
        char **params;
        int nparams;
        if (!parse_param_list(p, &params, &nparams)) return NULL;
        if (!eat(p, TK_ARROW)) {
            p_err(p, "expected '->' after parameter list");
            free(params);
            return NULL;
        }
        Ast *body = parse_expr(p);
        if (!body) { free(params); return NULL; }
        char **parr = params_to_arena(p, params, nparams);
        free(params);
        Ast *n = mk_fun(p, parr, nparams, body, t->line, t->col);
        return n;
    }
    case TK_LBRACKET: {
        advance(p);
        Ast **items = NULL;
        int n = 0, cap = 0;
        while (!at(p, TK_RBRACKET)) {
            Ast *e = parse_expr(p);
            if (!e) {
                free(items);
                return NULL;
            }
            if (p->error) { free(items); return NULL; }
            if (n == cap) {
                cap = cap ? cap * 2 : 4;
                items = (Ast **)realloc(items, (size_t)cap * sizeof(Ast *));
            }
            items[n++] = e;
            if (!eat(p, TK_COMMA)) break;
        }
        if (!eat(p, TK_RBRACKET)) {
            p_err(p, "expected ']' to close list literal");
            free(items);
            return NULL;
        }
        Ast **arr = (Ast **)arena_alloc(p->a, (size_t)n * sizeof(Ast *));
        memcpy(arr, items, (size_t)n * sizeof(Ast *));
        free(items);
        /* desugar [e1, ..., en]  ==>  cons(e1, cons(e2, ... cons(en, []))) */
        Ast *cur = mk(p, A_LIST, t->line, t->col);
        cur->u.list.n = 0;
        for (int i = n - 1; i >= 0; i--) {
            Ast *app = mk(p, A_APP, t->line, t->col);
            Ast *head = mk(p, A_VAR, t->line, t->col);
            head->u.name = (char *)"cons";
            Ast **args = (Ast **)arena_alloc(p->a, 2 * sizeof(Ast *));
            args[0] = arr[i];
            args[1] = cur;
            app->u.app.fn = head;
            app->u.app.args = args;
            app->u.app.nargs = 2;
            cur = app;
        }
        return cur;
    }
    default:
        p_err(p, "expected an expression");
        return NULL;
    }
}

/* true if the '(' at the current position opens a comma-separated argument
 * list like f(a, b, c) rather than a parenthesized expression */
static bool paren_is_arg_list(const Parser *p) {
    int depth = 0;
    for (int i = p->pos; i < p->n; i++) {
        TokKind k = p->toks[i].kind;
        if (k == TK_LPAREN) depth++;
        else if (k == TK_RPAREN) {
            depth--;
            if (depth == 0) return false;
        } else if (k == TK_COMMA && depth == 1) {
            return true;
        } else if (k == TK_EOF) {
            return false;
        }
    }
    return false;
}

static Ast *parse_app(Parser *p) {
    Ast *head = parse_atom(p);
    if (!head) return NULL;
    Ast **args = NULL;
    int nargs = 0, cap = 0;

    int app_line = p->toks[p->pos - 1].line;
    while (atom_start(peek(p)->kind)) {
        if (at(p, TK_LPAREN) && paren_is_arg_list(p)) {
            advance(p); /* consume '(' */
            Ast *a0 = parse_expr(p);
            if (nargs == cap) {
                cap = cap ? cap * 2 : 4;
                args = (Ast **)realloc(args, (size_t)cap * sizeof(Ast *));
            }
            args[nargs++] = a0;
            while (eat(p, TK_COMMA)) {
                Ast *ai = parse_expr(p);
                if (!ai) {
                    free(args);
                    return NULL;
                }
                if (nargs == cap) {
                    cap = cap ? cap * 2 : 4;
                    args = (Ast **)realloc(args, (size_t)cap * sizeof(Ast *));
                }
                args[nargs++] = ai;
            }
            if (!eat(p, TK_RPAREN)) {
                p_err(p, "expected ')' to close argument list");
                free(args);
                return NULL;
            }
            app_line = p->toks[p->pos - 1].line;
            continue;
        }
        if (peek(p)->line != app_line) break;
        Ast *arg = parse_atom(p);
        if (!arg) {
            free(args);
            return NULL;
        }
        if (nargs == cap) {
            cap = cap ? cap * 2 : 4;
            args = (Ast **)realloc(args, (size_t)cap * sizeof(Ast *));
        }
        args[nargs++] = arg;
        app_line = p->toks[p->pos - 1].line;
    }

    if (nargs == 0) {
        free(args);
        return head;
    }
    Ast **arr = (Ast **)arena_alloc(p->a, (size_t)nargs * sizeof(Ast *));
    memcpy(arr, args, (size_t)nargs * sizeof(Ast *));
    free(args);
    Ast *n = mk(p, A_APP, head->line, head->col);
    n->u.app.fn = head;
    n->u.app.args = arr;
    n->u.app.nargs = nargs;
    return n;
}

static Ast *parse_app(Parser *p);
static Ast *parse_atom(Parser *p);

static Ast *parse_unary(Parser *p) {
    const Token *t = peek(p);
    if (at(p, TK_KW_NOT)) {
        advance(p);
        Ast *op = parse_unary(p);
        if (!op) return NULL;
        Ast *n = mk(p, A_NOT, t->line, t->col);
        n->u.operand = op;
        return n;
    }
    if (at(p, TK_MINUS)) {
        advance(p);
        Ast *op = parse_unary(p);
        if (!op) return NULL;
        Ast *zero = mk(p, A_INT, t->line, t->col);
        zero->u.ival = 0;
        return mk_bin(p, OP_SUB, zero, op, t->line, t->col);
    }
    if (at(p, TK_KW_PRINT)) {
        advance(p);
        Ast *n = mk(p, A_PRINT, t->line, t->col);
        if (at(p, TK_LPAREN) && paren_is_arg_list(p)) {
            advance(p);
            Ast *v = parse_expr(p);
            if (!v) return NULL;
            if (!eat(p, TK_COMMA)) {
                p_err(p, "expected ',' in print(...)");
                return NULL;
            }
            Ast *nl = parse_expr(p);
            if (!nl) return NULL;
            if (!eat(p, TK_RPAREN)) {
                p_err(p, "expected ')' after print arguments");
                return NULL;
            }
            n->u.print.val = v;
            n->u.print.nl = nl;
            return n;
        }
        Ast *op = parse_expr(p);
        if (!op) return NULL;
        n->u.print.val = op;
        n->u.print.nl = NULL;
        return n;
    }
    if (at(p, TK_KW_CALLCC)) {
        advance(p);
        Ast *op = parse_expr(p);
        if (!op) return NULL;
        Ast *n = mk(p, A_CALLCC, t->line, t->col);
        n->u.operand = op;
        return n;
    }
    if (at(p, TK_KW_THROW)) {
        advance(p);
        Ast *k = parse_atom(p);
        Ast *v = parse_atom(p);
        if (!k || !v) return NULL;
        Ast *n = mk(p, A_THROW, t->line, t->col);
        n->u.thr.k = k;
        n->u.thr.v = v;
        return n;
    }
    return parse_app(p);
}

static Ast *parse_mul(Parser *p) {
    Ast *l = parse_unary(p);
    if (!l) return NULL;
    while (at(p, TK_STAR) || at(p, TK_SLASH) || at(p, TK_PERCENT)) {
        const Token *t = advance(p);
        Ast *r = parse_unary(p);
        if (!r) return NULL;
        int op = t->kind == TK_STAR ? OP_MUL : t->kind == TK_SLASH ? OP_DIV : OP_MOD;
        l = mk_bin(p, op, l, r, t->line, t->col);
    }
    return l;
}

static Ast *parse_add(Parser *p) {
    Ast *l = parse_mul(p);
    if (!l) return NULL;
    while (at(p, TK_PLUS) || at(p, TK_MINUS)) {
        const Token *t = advance(p);
        Ast *r = parse_mul(p);
        if (!r) return NULL;
        int op = t->kind == TK_PLUS ? OP_ADD : OP_SUB;
        l = mk_bin(p, op, l, r, t->line, t->col);
    }
    return l;
}

static int cmp_op(TokKind k) {
    switch (k) {
    case TK_EQEQ: return OP_EQ;
    case TK_NEQ: return OP_NE;
    case TK_LT: return OP_LT;
    case TK_LE: return OP_LE;
    case TK_GT: return OP_GT;
    case TK_GE: return OP_GE;
    default: return -1;
    }
}

static Ast *parse_cmp(Parser *p) {
    Ast *l = parse_add(p);
    if (!l) return NULL;
    while (at(p, TK_EQEQ) || at(p, TK_NEQ) || at(p, TK_LT) || at(p, TK_LE) || at(p, TK_GT) || at(p, TK_GE)) {
        const Token *t = advance(p);
        Ast *r = parse_add(p);
        if (!r) return NULL;
        l = mk_bin(p, cmp_op(t->kind), l, r, t->line, t->col);
    }
    return l;
}

static Ast *parse_and(Parser *p) {
    Ast *l = parse_cmp(p);
    if (!l) return NULL;
    while (at(p, TK_KW_AND)) {
        const Token *t = advance(p);
        Ast *r = parse_cmp(p);
        if (!r) return NULL;
        l = mk_bin(p, OP_AND, l, r, t->line, t->col);
    }
    return l;
}

static Ast *parse_or(Parser *p) {
    Ast *l = parse_and(p);
    if (!l) return NULL;
    while (at(p, TK_KW_OR)) {
        const Token *t = advance(p);
        Ast *r = parse_and(p);
        if (!r) return NULL;
        l = mk_bin(p, OP_OR, l, r, t->line, t->col);
    }
    return l;
}

static Ast *parse_if(Parser *p) {
    const Token *t = peek(p);
    if (at(p, TK_KW_IF)) {
        advance(p);
        Ast *cond = parse_expr(p);
        if (!cond) return NULL;
        if (!eat(p, TK_KW_THEN)) {
            p_err(p, "expected 'then'");
            return NULL;
        }
        Ast *then = parse_expr(p);
        if (!then) return NULL;
        if (!eat(p, TK_KW_ELSE)) {
            p_err(p, "expected 'else'");
            return NULL;
        }
        Ast *els = parse_expr(p);
        if (!els) return NULL;
        Ast *n = mk(p, A_IF, t->line, t->col);
        n->u.if_.cond = cond;
        n->u.if_.then = then;
        n->u.if_.els = els;
        return n;
    }
    return parse_or(p);
}

static Ast *parse_expr(Parser *p) {
    if (at(p, TK_KW_LET)) {
        const Token *lt = advance(p);
        if (!at(p, TK_IDENT)) {
            p_err(p, "expected identifier after 'let'");
            return NULL;
        }
        const Token *nt = advance(p);
        char **params;
        int nparams;
        if (!parse_param_list(p, &params, &nparams)) return NULL;
        if (!eat(p, TK_EQ)) {
            p_err(p, "expected '='");
            free(params);
            return NULL;
        }
        Ast *bound = parse_expr(p);
        if (!bound) { free(params); return NULL; }
        if (!eat(p, TK_KW_IN)) {
            p_err(p, "expected 'in'");
            free(params);
            return NULL;
        }
        Ast *body = parse_expr(p);
        if (!body) { free(params); return NULL; }
        Ast *b = bound;
        if (nparams > 0) {
            char **parr = params_to_arena(p, params, nparams);
            b = mk_fun(p, parr, nparams, bound, lt->line, lt->col);
        }
        free(params);
        return mk_let(p, nt->text, b, body, lt->line, lt->col);
    }
    return parse_if(p);
}

typedef struct {
    bool is_bind;
    const char *name; /* bindings only */
    char **params;    /* bindings only */
    int nparams;      /* bindings only */
    Ast *expr;        /* bound expr (bindings) or plain expr */
} Item;

static int skip_dotted(Parser *p) {
    if (!at(p, TK_IDENT)) {
        p_err(p, "expected identifier");
        return 0;
    }
    advance(p);
    while (at(p, TK_DOT)) {
        advance(p);
        if (!at(p, TK_IDENT)) {
            p_err(p, "expected identifier after '.'");
            return 0;
        }
        advance(p);
    }
    return 1;
}

static int skip_idlist(Parser *p) {
    if (!at(p, TK_IDENT)) {
        p_err(p, "expected identifier");
        return 0;
    }
    advance(p);
    while (eat(p, TK_COMMA)) {
        if (!at(p, TK_IDENT)) {
            p_err(p, "expected identifier");
            return 0;
        }
        advance(p);
    }
    return 1;
}

#define YAC_IMPORT_MAX 16

typedef struct {
    char pkgs[YAC_IMPORT_MAX][64];
    int n;
} ImportCtx;

static int parse_items(Parser *p, ImportCtx *ictx, Item **items, int *nitems, int *cap);

static int parse_dotted_name(Parser *p, char *buf, size_t bufsz) {
    if (!at(p, TK_IDENT)) {
        p_err(p, "expected identifier");
        return 0;
    }
    const Token *t = advance(p);
    snprintf(buf, bufsz, "%s", t->text ? t->text : "");
    while (at(p, TK_DOT)) {
        advance(p);
        if (!at(p, TK_IDENT)) {
            p_err(p, "expected identifier after '.'");
            return 0;
        }
        t = advance(p);
        size_t n = strlen(buf);
        if (n + 2 + (t->text ? strlen(t->text) : 0) >= bufsz) {
            p_err(p, "package name too long");
            return 0;
        }
        snprintf(buf + n, bufsz - n, ".%s", t->text ? t->text : "");
    }
    return 1;
}

/* import a.b.c → rt/os.yac style relative file (no src-self prefix). */
static int pkg_to_file(const char *pkg, char *out, size_t n) {
    size_t o = 0;
    if (!pkg || !pkg[0] || n < 6) return 0;
    for (size_t i = 0; pkg[i]; i++) {
        char c = pkg[i];
        if (o + 5 >= n) return 0;
        if (c == '.') out[o++] = '/';
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_')
            out[o++] = c;
        else return 0;
    }
    memcpy(out + o, ".yac", 5);
    return 1;
}

static char *read_whole_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static char *try_read(const char *path) {
    return path && path[0] ? read_whole_file(path) : NULL;
}

static char *read_pkg_source(const char *pkg) {
    char file[256], path[1024], exe[512];
    char *s;
    const char *lib;
    if (!pkg_to_file(pkg, file, sizeof(file))) return NULL;
    lib = getenv("YAC_STDLIB");
    if (lib && lib[0]) {
        snprintf(path, sizeof(path), "%s/%s", lib, file);
        s = try_read(path);
        if (s) return s;
    }
    snprintf(path, sizeof(path), "src-self/%s", file);
    s = try_read(path);
    if (s) return s;
    s = try_read(file);
    if (s) return s;
    if (yac_exe_dir(exe, sizeof(exe)) != 0) return NULL;
    snprintf(path, sizeof(path), "%s/%s", exe, file);
    s = try_read(path);
    if (s) return s;
    snprintf(path, sizeof(path), "%s/src-self/%s", exe, file);
    s = try_read(path);
    if (s) return s;
    snprintf(path, sizeof(path), "%s/../../src-self/%s", exe, file);
    s = try_read(path);
    if (s) return s;
    snprintf(path, sizeof(path), "%s/../src-self/%s", exe, file);
    return try_read(path);
}

static int import_seen(const ImportCtx *c, const char *pkg) {
    for (int i = 0; i < c->n; i++)
        if (strcmp(c->pkgs[i], pkg) == 0) return 1;
    return 0;
}

static int import_mark(ImportCtx *c, const char *pkg) {
    if (c->n >= YAC_IMPORT_MAX) return 0;
    snprintf(c->pkgs[c->n], sizeof(c->pkgs[0]), "%s", pkg);
    c->n++;
    return 1;
}

static int import_splice(Parser *errp, ImportCtx *ictx, const char *pkg,
                         Item **items, int *nitems, int *cap) {
    if (import_seen(ictx, pkg)) return 1;
    char *src = read_pkg_source(pkg);
    if (!src) {
        p_err(errp, "cannot find package '%s' (need rt/*.yac next to yc or src-self/)", pkg);
        return 0;
    }
    if (!import_mark(ictx, pkg)) {
        free(src);
        p_err(errp, "too many imports");
        return 0;
    }
    LexResult lx = lex_program(src, errp->a);
    if (lx.error) {
        errp->error = lx.error;
        free(src);
        free(lx.toks);
        return 0;
    }
    Parser ip = {lx.toks, lx.n, 0, errp->a, NULL};
    int ok = parse_items(&ip, ictx, items, nitems, cap);
    if (!ok && ip.error) errp->error = ip.error;
    free(lx.toks);
    free(src);
    return ok;
}

static int parse_items(Parser *p, ImportCtx *ictx, Item **items, int *nitems, int *cap) {
    while (!at(p, TK_EOF)) {
        if (at(p, TK_KW_PACKAGE)) {
            advance(p);
            if (!skip_dotted(p)) return 0;
            eat(p, TK_SEMI);
            continue;
        }
        if (at(p, TK_KW_EXPORT)) {
            advance(p);
            if (!skip_idlist(p)) return 0;
            eat(p, TK_SEMI);
            continue;
        }
        if (at(p, TK_KW_IMPORT)) {
            advance(p);
            char pkg[128];
            if (!parse_dotted_name(p, pkg, sizeof(pkg))) return 0;
            if (at(p, TK_LBRACE)) {
                advance(p);
                if (!skip_idlist(p)) return 0;
                if (!eat(p, TK_RBRACE)) {
                    p_err(p, "expected '}' after import list");
                    return 0;
                }
            } else if (at(p, TK_IDENT) && peek(p)->text &&
                       strcmp(peek(p)->text, "as") == 0) {
                advance(p);
                if (!at(p, TK_IDENT)) {
                    p_err(p, "expected identifier after 'as'");
                    return 0;
                }
                advance(p);
            }
            eat(p, TK_SEMI);
            if (!import_splice(p, ictx, pkg, items, nitems, cap)) return 0;
            continue;
        }
        if (at(p, TK_KW_LET)) {
            const Token *lt = advance(p);
            if (!at(p, TK_IDENT)) {
                p_err(p, "expected identifier after 'let'");
                return 0;
            }
            const Token *nt = advance(p);
            char **params;
            int nparams;
            if (!parse_param_list(p, &params, &nparams)) return 0;
            if (!eat(p, TK_EQ)) {
                p_err(p, "expected '='");
                return 0;
            }
            Ast *bound = parse_expr(p);
            if (!bound) return 0;

            if (at(p, TK_KW_IN)) {
                advance(p);
                Ast *body = parse_expr(p);
                if (!body) return 0;
                Ast *b = bound;
                if (nparams > 0) {
                    char **parr = params_to_arena(p, params, nparams);
                    b = mk_fun(p, parr, nparams, bound, lt->line, lt->col);
                }
                free(params);
                Ast *le = mk_let(p, nt->text, b, body, lt->line, lt->col);
                if (*nitems == *cap) {
                    *cap = *cap ? *cap * 2 : 8;
                    *items = (Item *)realloc(*items, (size_t)*cap * sizeof(Item));
                }
                (*items)[(*nitems)++] = (Item){false, NULL, NULL, 0, le};
            } else {
                if (*nitems == *cap) {
                    *cap = *cap ? *cap * 2 : 8;
                    *items = (Item *)realloc(*items, (size_t)*cap * sizeof(Item));
                }
                (*items)[(*nitems)++] = (Item){true, nt->text, params, nparams, bound};
            }
        } else {
            Ast *e = parse_expr(p);
            if (!e) return 0;
            if (*nitems == *cap) {
                *cap = *cap ? *cap * 2 : 8;
                *items = (Item *)realloc(*items, (size_t)*cap * sizeof(Item));
            }
            (*items)[(*nitems)++] = (Item){false, NULL, NULL, 0, e};
        }
        eat(p, TK_SEMI);
    }
    return 1;
}

ParseResult parse_program(const Token *toks, int n, Arena *a) {
    ParseResult res = {0};
    Parser p = {toks, n, 0, a, NULL};
    ImportCtx ictx;
    memset(&ictx, 0, sizeof(ictx));

    Item *items = NULL;
    int nitems = 0, cap = 0;
    int discard_cnt = 0;

    if (!parse_items(&p, &ictx, &items, &nitems, &cap)) goto err;

    if (nitems == 0) {
        p_err(&p, "empty program");
        goto err;
    }

    Ast *prog = NULL;
    for (int i = nitems - 1; i >= 0; i--) {
        Item *it = &items[i];
        if (it->is_bind) {
            Ast *b = it->expr;
            if (it->nparams > 0) {
                char **parr = params_to_arena(&p, it->params, it->nparams);
                b = mk_fun(&p, parr, it->nparams, b, 0, 0);
            }
            Ast *body = prog ? prog : mk_unit(&p, 0, 0);
            Ast *le = mk_let(&p, it->name, b, body, 0, 0);
            prog = le;
        } else if (prog == NULL) {
            prog = it->expr;
        } else {
            char dname[64];
            snprintf(dname, sizeof(dname), "#discard%d", discard_cnt++);
            Ast *le = mk_let(&p, dname, it->expr, prog, 0, 0);
            prog = le;
        }
    }

    res.program = prog;
    free(items);
    return res;

err:
    free(items);
    res.program = NULL;
    res.error = p.error;
    return res;
}
