/* Scheme subset -> yac source translator.
 *
 * Supported subset: define (top-level), lambda, if, cond, let, let*, begin,
 * quote/' of lists, and/or/not, arithmetic (+ - * / modulo remainder),
 * comparisons (= < > <= >= eq? equal?), car/cdr/c[ad]+r, null?, length,
 * cons, append, list, reverse, map, filter, foldl, foldr, display, newline.
 * Unknown applications are emitted as yac function calls; identifiers are
 * mangled ('-' -> '_'). Unsupported forms are rejected with an error.
 *
 * Notes:
 *  - yac has no zero-arity application: (f) and (define (f) ...) are
 *    rejected at translation time.
 *  - yac's `and`/`or` do not short-circuit; translations are equivalent in
 *    value but always evaluate every operand.
 */

#include "config.h"
#include "scheme.h"

#include <stdbool.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- growable output buffer ---- */

typedef struct {
    char *data;
    size_t len, cap;
} Out;

static void out_grow(Out *o, size_t need) {
    if (o->len + need + 1 < o->cap) return;
    o->cap = o->cap ? o->cap * 2 : 256;
    while (o->len + need + 1 >= o->cap) o->cap *= 2;
    o->data = (char *)realloc(o->data, o->cap);
}

static void out_put(Out *o, char c) {
    out_grow(o, 1);
    o->data[o->len++] = c;
    o->data[o->len] = '\0';
}

static void out_str(Out *o, const char *s) {
    size_t n = strlen(s);
    out_grow(o, n);
    memcpy(o->data + o->len, s, n);
    o->len += n;
    o->data[o->len] = '\0';
}

static void out_insert_str(Out *o, size_t at, const char *s) {
    size_t n = strlen(s);
    out_grow(o, n);
    memmove(o->data + at + n, o->data + at, o->len - at + 1);
    memcpy(o->data + at, s, n);
    o->len += n;
}

static void out_mark(Out *o, size_t *mark) { *mark = o->len; }

/* ---- tokens ---- */

typedef enum { T_LP, T_RP, T_QUOTE, T_ATOM, T_STR, T_EOF } TKind;

typedef struct {
    TKind kind;
    char *text; /* atoms and decoded strings; numbers keep their text */
    int line;
} STok;

typedef struct {
    STok *t;
    int n, pos;
    char *err;
} SP;

static void serr(SP *p, int line, const char *fmt, const char *a1, const char *a2) {
    if (p->err) return;
    char buf[256];
    snprintf(buf, sizeof(buf), fmt, a1 ? a1 : "", a2 ? a2 : "");
    char loc[300];
    snprintf(loc, sizeof(loc), "s2y: %d: %s", line, buf);
    p->err = strdup(loc);
}

static bool atom_char(char c) {
    return c && !isspace((unsigned char)c) && c != '(' && c != ')' &&
           c != '\'' && c != '"' && c != ';';
}

static char *lex_string(const char *src, size_t *pos, int *line, SP *p) {
    size_t cap = 16, n = 0;
    char *buf = (char *)malloc(cap);
    (*pos)++;
    int l = *line;
    while (src[*pos] && src[*pos] != '"') {
        char c = src[(*pos)++];
        if (c == '\n') { (*line)++; l = *line; }
        if (c == '\\') {
            char e = src[(*pos)++];
            if (!e) break;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\n'; break; /* no \r escape in yac: fold to \n */
            case '\\': c = '\\'; break;
            case '"': c = '"'; break;
            default:
                free(buf);
                serr(p, l, "unsupported string escape '\\%c'", "", "");
                return NULL;
            }
        }
        if (n + 1 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
        buf[n++] = c;
    }
    if (src[*pos] != '"') {
        free(buf);
        serr(p, l, "unterminated string literal", "", "");
        return NULL;
    }
    (*pos)++;
    buf[n] = '\0';
    return buf;
}

static void lex(SP *p, const char *src) {
    size_t pos = 0;
    int line = 1;
    size_t cap = 64;
    p->t = (STok *)calloc(cap, sizeof(STok));
    for (;;) {
        while (src[pos] && isspace((unsigned char)src[pos])) {
            if (src[pos] == '\n') line++;
            pos++;
        }
        if (src[pos] == ';') {
            while (src[pos] && src[pos] != '\n') pos++;
            continue;
        }
        if (!src[pos]) break;
        if ((size_t)p->n >= cap) {
            cap *= 2;
            p->t = (STok *)realloc(p->t, cap * sizeof(STok));
        }
        STok *t = &p->t[p->n++];
        t->line = line;
        t->text = NULL;
        char c = src[pos];
        if (c == '(') { t->kind = T_LP; pos++; continue; }
        if (c == ')') { t->kind = T_RP; pos++; continue; }
        if (c == '\'') { t->kind = T_QUOTE; pos++; continue; }
        if (c == '"') {
            t->kind = T_STR;
            t->text = lex_string(src, &pos, &line, p);
            if (p->err) return;
            continue;
        }
        size_t start = pos;
        while (atom_char(src[pos])) pos++;
        size_t n = pos - start;
        t->kind = T_ATOM;
        t->text = (char *)malloc(n + 1);
        memcpy(t->text, src + start, n);
        t->text[n] = '\0';
    }
    STok *e = &p->t[p->n++];
    e->kind = T_EOF;
    e->line = line;
    e->text = NULL;
}

static const STok *peek(SP *p) { return &p->t[p->pos]; }
static TKind peek2k(SP *p) { return p->t[p->pos + 1].kind; }
static const char *peek2t(SP *p) { return p->t[p->pos + 1].text; }
static void next(SP *p) { if (p->t[p->pos].kind != T_EOF) p->pos++; }

/* ---- identifier mangling ---- */

typedef struct {
    char **names;
    int n, cap;
    int *marks;
    int nm, cm;
} Names;

static bool name_used(Names *ns, const char *name) {
    for (int i = 0; i < ns->n; i++)
        if (strcmp(ns->names[i], name) == 0) return true;
    return false;
}

static void name_add(Names *ns, const char *name) {
    if (ns->n >= ns->cap) {
        ns->cap = ns->cap ? ns->cap * 2 : 16;
        ns->names = (char **)realloc(ns->names, (size_t)ns->cap * sizeof(char *));
    }
    ns->names[ns->n++] = strdup(name);
}

/* scope marks: names registered inside a lambda/let are popped at scope end
 * so that shadowing parameters of different functions do not collide */
static void name_mark(Names *ns) {
    if (ns->nm >= ns->cm) {
        ns->cm = ns->cm ? ns->cm * 2 : 16;
        ns->marks = (int *)realloc(ns->marks, (size_t)ns->cm * sizeof(int));
    }
    ns->marks[ns->nm++] = ns->n;
}

static void name_pop(Names *ns) {
    int from = ns->marks[--ns->nm];
    for (int i = from; i < ns->n; i++) free(ns->names[i]);
    ns->n = from;
}

static const char *YAC_KW[] = {
    "let", "in", "fun", "if", "then", "else", "not", "print",
    "callcc", "throw", "and", "or", "true", "false",
};

static bool is_kw(const char *s) {
    for (size_t i = 0; i < sizeof(YAC_KW) / sizeof(YAC_KW[0]); i++)
        if (strcmp(s, YAC_KW[i]) == 0) return true;
    return false;
}

/* Map a Scheme identifier to a yac identifier ('-' -> '_'). If `def` is set
 * the name is registered (define/let/lambda binders) so that distinct source
 * names that mangle to the same yac name are rejected; plain references are
 * not registered. */
static char *mangle(SP *p, Names *ns, const char *s, int line, bool def) {
    size_t len = strlen(s);
    char *out = (char *)malloc(len + 1);
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '-') { out[n++] = '_'; continue; }
        if (isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80) {
            out[n++] = c;
            continue;
        }
        char a[2] = {c, 0};
        free(out);
        serr(p, line, "identifier '%s' contains unsupported character '%s'", s, a);
        return NULL;
    }
    out[n] = '\0';
    if (n == 0) {
        free(out);
        serr(p, line, "empty identifier", "", "");
        return NULL;
    }
    if (isdigit((unsigned char)out[0])) {
        free(out);
        serr(p, line, "identifier '%s' would start with a digit", s, "");
        return NULL;
    }
    if (is_kw(out)) {
        free(out);
        serr(p, line, "identifier '%s' collides with a yac keyword", s, "");
        return NULL;
    }
    if (def && name_used(ns, out)) {
        free(out);
        serr(p, line, "identifier '%s' collides with another name after mangling", s, "");
        return NULL;
    }
    if (def) name_add(ns, out);
    return out;
}

/* fresh temp name that cannot collide with any mangled identifier */
static char *fresh(Names *ns) {
    for (long i = 0;; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "_s2y_%ld_", i);
        if (!name_used(ns, buf)) {
            name_add(ns, buf);
            return strdup(buf);
        }
    }
}

/* ---- expression translation ---- */

/* result of emitting an expression:
 *   kind 0 = atomic (var, literal, call): safe bare
 *   kind 1 = operator chain with precedence `prec` (lower binds looser)
 *   kind 2 = let/if/fun/not/print/begin: must be parenthesized as an operand
 */
typedef struct {
    int kind;
    int prec;
} ERes;

typedef struct {
    SP *p;
    Names *ns;
    Out *o;
} T;

static bool expr(T *t, ERes *res);
static bool is_integer_lit(const char *s);

static void fail(T *t, int line, const char *msg, const char *a1, const char *a2) {
    serr(t->p, line, msg, a1, a2);
}

/* emit an expression as an operand of an infix operator, parenthesizing
 * when the grouping would change. `index` is the operand position (0 = left,
 * 1 = right); equal-precedence chains are left-associative. */
static bool operand(T *t, ERes *res, int op_prec, int index) {
    size_t mark;
    out_mark(t->o, &mark);
    if (!expr(t, res)) return false;
    bool parens = false;
    if (res->kind == 2) {
        parens = true;
    } else if (res->kind == 1) {
        if (res->prec < op_prec || (res->prec == op_prec && index > 0)) parens = true;
    }
    if (parens) {
        out_insert_str(t->o, mark, "(");
        out_put(t->o, ')');
    }
    return true;
}

/* N-ary Scheme operator (+ a b c) -> a + b + c; stops at the closing ')' */
static bool binop_chain(T *t, const char *op, int prec) {
    ERes r0;
    if (!operand(t, &r0, prec, 0)) return false;
    while (peek(t->p)->kind != T_RP && peek(t->p)->kind != T_EOF) {
        out_str(t->o, " ");
        out_str(t->o, op);
        out_str(t->o, " ");
        ERes ri;
        if (!operand(t, &ri, prec, 1)) return false;
    }
    return true;
}

/* a quoted datum: numbers, booleans, strings, and nested lists */
static bool datum(T *t) {
    const STok *tk = peek(t->p);
    switch (tk->kind) {
    case T_LP: {
        next(t->p);
        out_put(t->o, '[');
        bool first = true;
        while (peek(t->p)->kind != T_RP) {
            if (!first) out_str(t->o, ", ");
            if (!datum(t)) return false;
            first = false;
        }
        next(t->p);
        out_put(t->o, ']');
        return true;
    }
    case T_ATOM: {
        const char *s = tk->text;
        if (strcmp(s, "#t") == 0) { out_str(t->o, "true"); next(t->p); return true; }
        if (strcmp(s, "#f") == 0) { out_str(t->o, "false"); next(t->p); return true; }
        if (is_integer_lit(s)) {
            out_str(t->o, s);
            next(t->p);
            return true;
        }
        if (isdigit((unsigned char)s[0]) || (s[0] == '-' && isdigit((unsigned char)s[1])) ||
            (s[0] == '.' && isdigit((unsigned char)s[1]))) {
            fail(t, tk->line, "floating-point number '%s' is not supported", s, "");
            return false;
        }
        fail(t, tk->line, "cannot quote symbol '%s' (yac has no symbols)", s, "");
        return false;
    }
    case T_STR: {
        out_str(t->o, "\"");
        for (const char *s = tk->text; *s; s++) {
            if (*s == '"') out_str(t->o, "\\\"");
            else if (*s == '\\') out_str(t->o, "\\\\");
            else if (*s == '\n') out_str(t->o, "\\n");
            else if (*s == '\t') out_str(t->o, "\\t");
            else out_put(t->o, *s);
        }
        out_str(t->o, "\"");
        next(t->p);
        return true;
    }
    default:
        fail(t, tk->line, "unexpected token in quoted datum", "", "");
        return false;
    }
}

/* a body: e1 e2 ... en (n >= 1); the value is en, earlier expressions are
 * discarded through let bindings. Stops at the closing ')' (not consumed).
 * `res` receives the last expression's emission info. */
static bool seq_body(T *t, ERes *res) {
    if (peek(t->p)->kind == T_RP || peek(t->p)->kind == T_EOF) {
        fail(t, peek(t->p)->line, "expected at least one body expression", "", "");
        return false;
    }
    for (;;) {
        size_t mark;
        out_mark(t->o, &mark);
        char *tmp = fresh(t->ns);
        out_str(t->o, "let ");
        out_str(t->o, tmp);
        out_str(t->o, " = ");
        free(tmp);
        size_t epos = t->o->len;
        ERes e;
        if (!expr(t, &e)) return false;
        if (peek(t->p)->kind == T_RP || peek(t->p)->kind == T_EOF) {
            /* this was the final (only) expression: undo the let prefix but
             * keep the expression text */
            memmove(t->o->data + mark, t->o->data + epos, t->o->len - epos + 1);
            t->o->len -= epos - mark;
            if (res) *res = e;
            return true;
        }
        out_str(t->o, " in ");
    }
}

static bool app_args(T *t) {
    bool first = true;
    while (peek(t->p)->kind != T_RP) {
        if (!first) out_str(t->o, ", ");
        ERes a;
        if (!operand(t, &a, 0, 0)) return false;
        first = false;
    }
    return true;
}

/* (lambda (x y) e1 e2) -> fun (x, y) -> let _s2y_n = e1 in e2 */
static bool lam(T *t, ERes *res) {
    if (peek(t->p)->kind != T_LP) {
        fail(t, peek(t->p)->line, "lambda parameter list must be a parenthesized list", "", "");
        return false;
    }
    name_mark(t->ns);
    next(t->p);
    out_str(t->o, "fun (");
    bool first = true;
    while (peek(t->p)->kind != T_RP) {
        if (!first) out_str(t->o, ", ");
        const STok *pn = peek(t->p);
        if (pn->kind != T_ATOM) {
            fail(t, pn->line, "expected a parameter name", "", "");
            return false;
        }
        char *m = mangle(t->p, t->ns, pn->text, pn->line, true);
        if (!m) return false;
        out_str(t->o, m);
        free(m);
        next(t->p);
        first = false;
    }
    if (first) {
        fail(t, peek(t->p)->line, "zero-argument functions are not supported", "", "");
        return false;
    }
    next(t->p); /* ')' */
    out_str(t->o, ") -> ");
    bool ok = seq_body(t, res);
    name_pop(t->ns);
    return ok;
}

/* (let ((x e1) (y e2)) body...): parallel bindings need temps so that the
 * values of e1/e2 are evaluated before any name is bound; let* is sequential.
 * 'let'/'let*' is consumed by the caller. */
static bool let_form(T *t, ERes *res, bool parallel) {
    name_mark(t->ns);
    if (peek(t->p)->kind != T_LP) {
        fail(t, peek(t->p)->line, "expected binding list after let", "", "");
        return false;
    }
    next(t->p);
    char **names = NULL;
    char **tmps = NULL;
    char **vals = NULL;
    int n = 0, cap = 0;
    while (peek(t->p)->kind != T_RP) {
        if (peek(t->p)->kind != T_LP) {
            fail(t, peek(t->p)->line, "expected a parenthesized binding", "", "");
            goto err;
        }
        next(t->p);
        const STok *bn = peek(t->p);
        if (bn->kind != T_ATOM) {
            fail(t, bn->line, "expected a binding name", "", "");
            goto err;
        }
        if (n >= cap) {
            cap = cap ? cap * 2 : 8;
            names = (char **)realloc(names, (size_t)cap * sizeof(char *));
            tmps = (char **)realloc(tmps, (size_t)cap * sizeof(char *));
            vals = (char **)realloc(vals, (size_t)cap * sizeof(char *));
        }
        names[n] = mangle(t->p, t->ns, bn->text, bn->line, true);
        if (!names[n]) goto err;
        tmps[n] = NULL;
        vals[n] = NULL;
        next(t->p);
        if (parallel) {
            /* capture the value text now (before any name is bound) */
            char *tmp = fresh(t->ns);
            tmps[n] = tmp;
            size_t mark;
            out_mark(t->o, &mark);
            ERes ve;
            if (!expr(t, &ve)) goto err;
            size_t end = t->o->len;
            vals[n] = (char *)malloc(end - mark + 1);
            memcpy(vals[n], t->o->data + mark, end - mark);
            vals[n][end - mark] = '\0';
            t->o->len = mark;
            t->o->data[mark] = '\0';
        } else {
            /* let*: bind and emit sequentially */
            out_str(t->o, "let ");
            out_str(t->o, names[n]);
            out_str(t->o, " = ");
            ERes ve;
            if (!expr(t, &ve)) goto err;
            out_str(t->o, " in ");
        }
        if (peek(t->p)->kind != T_RP) {
            fail(t, peek(t->p)->line, "expected ')' to close binding", "", "");
            goto err;
        }
        next(t->p);
        n++;
    }
    next(t->p); /* ')' closing the binding list */
    if (parallel) {
        for (int i = 0; i < n; i++) {
            out_str(t->o, "let ");
            out_str(t->o, tmps[i]);
            out_str(t->o, " = ");
            out_str(t->o, vals[i]);
            out_str(t->o, " in ");
        }
        for (int i = 0; i < n; i++) {
            out_str(t->o, "let ");
            out_str(t->o, names[i]);
            out_str(t->o, " = ");
            out_str(t->o, tmps[i]);
            out_str(t->o, " in ");
        }
    }
    if (!seq_body(t, res)) goto err;
    for (int i = 0; i < n; i++) {
        free(names[i]);
        free(tmps[i]);
        free(vals[i]);
    }
    free(names);
    free(tmps);
    free(vals);
    name_pop(t->ns);
    return true;
err:
    name_pop(t->ns);
    for (int i = 0; i < n; i++) {
        free(names[i]);
        free(tmps[i]);
        free(vals[i]);
    }
    free(names);
    free(tmps);
    free(vals);
    return false;
}

/* one cond clause list: emit `if c then <body> else <rest>` */
static bool cond_next(T *t, ERes *res) {
    if (peek(t->p)->kind != T_LP) {
        fail(t, peek(t->p)->line, "expected a cond clause", "", "");
        return false;
    }
    next(t->p); /* '(' */
    const STok *c = peek(t->p);
    bool is_else = c->kind == T_ATOM && strcmp(c->text, "else") == 0;
    if (is_else) {
        next(t->p); /* 'else' */
    } else {
        out_str(t->o, "if ");
        ERes ce;
        if (!expr(t, &ce)) return false;
        out_str(t->o, " then ");
    }
    ERes be;
    if (!seq_body(t, &be)) return false;
    if (peek(t->p)->kind != T_RP) {
        fail(t, peek(t->p)->line, "expected ')' to close cond clause", "", "");
        return false;
    }
    next(t->p); /* ')' closing the clause */
    if (is_else) {
        if (peek(t->p)->kind != T_RP) {
            fail(t, peek(t->p)->line, "else clause must be the last cond clause", "", "");
            return false;
        }
        if (res) { res->kind = 2; }
        return true;
    }
    out_str(t->o, " else ");
    if (peek(t->p)->kind == T_RP) {
        out_str(t->o, "()");
        if (res) { res->kind = 2; }
        return true;
    }
    return cond_next(t, res);
}

/* (cond (c1 e1..) (c2 e2..) (else e3..)) -> nested ifs; 'cond' is consumed */
static bool cond_form(T *t, ERes *res) {
    if (peek(t->p)->kind != T_LP) {
        fail(t, peek(t->p)->line, "expected a clause list after cond", "", "");
        return false;
    }
    return cond_next(t, res);
}

/* car/cdr/c[ad]+r: the a/d sequence (positions 1..len-2) is applied to the
 * argument right-to-left, e.g. cadr -> nth(drop(x, 1), 0) */
static bool is_cadr(const char *s) {
    size_t len = strlen(s);
    if (len < 3) return false;
    if (s[0] != 'c' || s[len - 1] != 'r') return false;
    for (size_t i = 1; i + 1 < len; i++)
        if (s[i] != 'a' && s[i] != 'd') return false;
    return true;
}

static bool car_cdr(T *t, const char *s) {
    size_t len = strlen(s);
    for (size_t i = 1; i + 1 < len; i++)
        out_str(t->o, s[i] == 'a' ? "nth(" : "drop(");
    ERes a;
    if (!expr(t, &a)) return false;
    for (size_t i = len - 2; i >= 1; i--)
        out_str(t->o, s[i] == 'a' ? ", 0)" : ", 1)");
    return true;
}

/* true for integer literals: -?[0-9]+ */
static bool is_integer_lit(const char *s) {
    if (!s || !*s) return false;
    if (*s == '-') s++;
    if (!*s) return false;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s)) return false;
    return true;
}

/* translate a parenthesized expression: (head args...) */
static bool paren(T *t, ERes *res) {
    const STok *h = peek(t->p);
    bool ok = false;
    if (h->kind == T_ATOM) {
        const char *s = h->text;
        if (strcmp(s, "define") == 0) {
            fail(t, h->line, "define is only allowed at the top level", "", "");
            return false;
        }
        if (strcmp(s, "lambda") == 0) { next(t->p); return lam(t, res); }
        if (strcmp(s, "if") == 0) {
            next(t->p);
            out_str(t->o, "if ");
            ERes c;
            if (!expr(t, &c)) return false;
            out_str(t->o, " then ");
            ERes a;
            if (!expr(t, &a)) return false;
            out_str(t->o, " else ");
            if (peek(t->p)->kind != T_RP) {
                ERes b;
                if (!expr(t, &b)) return false;
            } else {
                out_str(t->o, "()");
            }
            if (res) { res->kind = 2; res->prec = 0; }
            return true;
        }
        if (strcmp(s, "cond") == 0) { next(t->p); return cond_form(t, res); }
        if (strcmp(s, "let") == 0) { next(t->p); return let_form(t, res, true); }
        if (strcmp(s, "let*") == 0) { next(t->p); return let_form(t, res, false); }
        if (strcmp(s, "begin") == 0) {
            next(t->p);
            ok = seq_body(t, res);
            if (ok && res) { res->kind = 2; }
            return ok;
        }
        if (strcmp(s, "quote") == 0) {
            next(t->p);
            ok = datum(t);
            if (ok && res) { res->kind = 0; res->prec = 0; }
            return ok;
        }
        if (strcmp(s, "and") == 0 || strcmp(s, "or") == 0) {
            next(t->p);
            if (peek(t->p)->kind == T_RP) {
                out_str(t->o, strcmp(s, "and") == 0 ? "true" : "false");
                if (res) { res->kind = 0; res->prec = 0; }
                return true;
            }
            /* in yac `or` binds looser than `and` */
            int prec = strcmp(s, "and") == 0 ? 1 : 0;
            ok = binop_chain(t, strcmp(s, "and") == 0 ? "and" : "or", prec);
            if (ok && res) { res->kind = 1; res->prec = prec; }
            return ok;
        }
        if (strcmp(s, "not") == 0) {
            next(t->p);
            out_str(t->o, "not ");
            ERes a;
            /* yac's `not` is a unary prefix in parse_unary (tightest), so any
             * operator chain operand must be parenthesized; only atomic
             * operands (var, literal, call) may be bare */
            if (!operand(t, &a, 1 << 20, 0)) return false;
            if (res) { res->kind = 2; res->prec = 0; }
            return true;
        }
        if (strcmp(s, "+") == 0 || strcmp(s, "-") == 0 ||
            strcmp(s, "*") == 0 || strcmp(s, "/") == 0 ||
            strcmp(s, "modulo") == 0 || strcmp(s, "remainder") == 0) {
            const char *op;
            int prec;
            bool unary_ok;
            if (strcmp(s, "+") == 0) { op = "+"; prec = 4; unary_ok = false; }
            else if (strcmp(s, "-") == 0) { op = "-"; prec = 4; unary_ok = true; }
            else if (strcmp(s, "*") == 0) { op = "*"; prec = 5; unary_ok = false; }
            else if (strcmp(s, "/") == 0) { op = "/"; prec = 5; unary_ok = false; }
            else { op = "%"; prec = 5; unary_ok = false; }
            next(t->p);
            if (peek(t->p)->kind == T_RP) {
                fail(t, h->line, "'%s' expects at least one argument", s, "");
                return false;
            }
if (unary_ok) {
                /* (- x) is unary negation: 0 - x; the operand is emitted with
                 * right-operand parenthesization rules */
                size_t mark;
                out_mark(t->o, &mark);
                ERes a;
                if (!operand(t, &a, prec, 1)) return false;
                if (peek(t->p)->kind == T_RP) {
                    out_insert_str(t->o, mark, "0 - ");
                    if (res) { res->kind = 1; res->prec = prec; }
                    return true;
                }
                /* multi-arg chain: continue with the emitted left operand */
                while (peek(t->p)->kind != T_RP && peek(t->p)->kind != T_EOF) {
                    out_str(t->o, " ");
                    out_str(t->o, op);
                    out_str(t->o, " ");
                    ERes ri;
                    if (!operand(t, &ri, prec, 1)) return false;
                }
            } else {
                if (!binop_chain(t, op, prec)) return false;
            }
            if (res) { res->kind = 1; res->prec = prec; }
            return true;
        }
        if (strcmp(s, "=") == 0 || strcmp(s, "eq?") == 0 || strcmp(s, "equal?") == 0) {
            next(t->p);
            if (peek(t->p)->kind == T_RP) {
                fail(t, h->line, "'%s' expects at least one argument", s, "");
                return false;
            }
            ok = binop_chain(t, "==", 2);
            if (ok && res) { res->kind = 1; res->prec = 2; }
            return ok;
        }
        if (strcmp(s, "<") == 0 || strcmp(s, "<=") == 0 ||
            strcmp(s, ">") == 0 || strcmp(s, ">=") == 0) {
            const char *op = strcmp(s, "<") == 0 ? "<" : strcmp(s, "<=") == 0 ? "<="
                            : strcmp(s, ">") == 0 ? ">" : ">=";
            next(t->p);
            if (peek(t->p)->kind == T_RP) {
                fail(t, h->line, "'%s' expects at least one argument", s, "");
                return false;
            }
            ok = binop_chain(t, op, 3);
            if (ok && res) { res->kind = 1; res->prec = 3; }
            return ok;
        }
        if (strcmp(s, "display") == 0) {
            next(t->p);
            out_str(t->o, "print ");
            ERes a;
            if (!expr(t, &a)) return false;
            if (res) { res->kind = 2; res->prec = 0; }
            return true;
        }
        if (strcmp(s, "newline") == 0) {
            next(t->p);
            out_str(t->o, "print \"\"");
            if (res) { res->kind = 2; res->prec = 0; }
            return true;
        }
        if (is_cadr(s)) {
            next(t->p);
            ok = car_cdr(t, s);
            if (ok && res) { res->kind = 0; res->prec = 0; }
            return ok;
        }
        if (strcmp(s, "null?") == 0) {
            next(t->p);
            if (!operand(t, res, 2, 1)) return false;
            out_str(t->o, " == []");
            if (res) { res->kind = 1; res->prec = 2; }
            return true;
        }
        if (strcmp(s, "length") == 0 || strcmp(s, "len") == 0) {
            next(t->p);
            out_str(t->o, "len(");
            ERes a;
            if (!expr(t, &a)) return false;
            out_str(t->o, ")");
            if (res) { res->kind = 0; res->prec = 0; }
            return true;
        }
        if (strcmp(s, "cons") == 0 || strcmp(s, "append") == 0 ||
            strcmp(s, "map") == 0 || strcmp(s, "filter") == 0 ||
            strcmp(s, "foldl") == 0 || strcmp(s, "foldr") == 0) {
            next(t->p);
            out_str(t->o, s);
            out_str(t->o, "(");
            if (!app_args(t)) return false;
            out_str(t->o, ")");
            if (res) { res->kind = 0; res->prec = 0; }
            return true;
        }
        if (strcmp(s, "reverse") == 0) {
            next(t->p);
            out_str(t->o, "foldl(fun (a, x) -> cons(x, a), [], ");
            ERes a;
            if (!expr(t, &a)) return false;
            out_str(t->o, ")");
            if (res) { res->kind = 0; res->prec = 0; }
            return true;
        }
        if (strcmp(s, "list") == 0) {
            next(t->p);
            out_str(t->o, "[");
            if (!app_args(t)) return false;
            out_str(t->o, "]");
            if (res) { res->kind = 0; res->prec = 0; }
            return true;
        }
        if (strcmp(s, "apply") == 0) {
            /* (apply f a b) -> f(a, b) */
            next(t->p);
            if (peek(t->p)->kind == T_RP) {
                fail(t, h->line, "'apply' expects a function", "", "");
                return false;
            }
            bool comp = peek(t->p)->kind == T_LP;
            if (comp) {
                /* head is a compound expression; bind it first because yac
                 * cannot chain-call (f(x))(y) */
                char *tmp = fresh(t->ns);
                out_str(t->o, "let ");
                out_str(t->o, tmp);
                out_str(t->o, " = ");
                ERes f;
                if (!expr(t, &f)) { free(tmp); return false; }
                out_str(t->o, " in ");
                out_str(t->o, tmp);
                free(tmp);
            } else {
                ERes f;
                if (!operand(t, &f, 0, 0)) return false;
            }
            out_str(t->o, "(");
            if (!app_args(t)) return false;
            out_str(t->o, ")");
            if (res) { res->kind = 0; res->prec = 0; }
            return true;
        }
        if (strcmp(s, "set!") == 0 || strcmp(s, "letrec") == 0 ||
            strcmp(s, "do") == 0 || strcmp(s, "case") == 0 ||
            strcmp(s, "vector") == 0) {
            fail(t, h->line, "unsupported Scheme form '%s'", s, "");
            return false;
        }
        /* a named function call: (name a b) -> name(a, b) */
        next(t->p);
        char *m = mangle(t->p, t->ns, s, h->line, false);
        if (!m) return false;
        out_str(t->o, m);
        free(m);
        out_str(t->o, "(");
        if (!app_args(t)) return false;
        out_str(t->o, ")");
        if (res) { res->kind = 0; res->prec = 0; }
        return true;
    }
    if (h->kind == T_STR) {
        fail(t, h->line, "cannot call a string value", "", "");
        return false;
    }
    /* general head expression: ((f x) a b). yac cannot chain-call a call
     * result in one expression, so bind the head to a temp first:
     * let _s2y_n = f(x) in _s2y_n(a, b) */
    char *tmp = fresh(t->ns);
    out_str(t->o, "let ");
    out_str(t->o, tmp);
    out_str(t->o, " = ");
    ERes hres;
    if (!expr(t, &hres)) { free(tmp); return false; }
    out_str(t->o, " in ");
    out_str(t->o, tmp);
    free(tmp);
    out_str(t->o, "(");
    if (!app_args(t)) return false;
    out_str(t->o, ")");
    if (res) { res->kind = 0; res->prec = 0; }
    return true;
}

static bool expr(T *t, ERes *res) {
    const STok *tk = peek(t->p);
    if (res) { res->kind = 0; res->prec = 0; }
    switch (tk->kind) {
    case T_LP: {
        next(t->p); /* '(' */
        bool ok = paren(t, res);
        if (!ok) return false;
        if (peek(t->p)->kind != T_RP) {
            fail(t, tk->line, "expected ')'", "", "");
            return false;
        }
        next(t->p); /* ')' */
        return true;
    }
    case T_QUOTE: {
        next(t->p);
        return datum(t);
    }
    case T_ATOM: {
        const char *s = tk->text;
        if (strcmp(s, "#t") == 0) { out_str(t->o, "true"); next(t->p); return true; }
        if (strcmp(s, "#f") == 0) { out_str(t->o, "false"); next(t->p); return true; }
        if (is_integer_lit(s)) {
            out_str(t->o, s);
            next(t->p);
            return true;
        }
        if (isdigit((unsigned char)s[0]) || (s[0] == '-' && isdigit((unsigned char)s[1])) ||
            (s[0] == '.' && isdigit((unsigned char)s[1]))) {
            fail(t, tk->line, "floating-point number '%s' is not supported", s, "");
            return false;
        }
        char *m = mangle(t->p, t->ns, s, tk->line, false);
        if (!m) return false;
        out_str(t->o, m);
        free(m);
        next(t->p);
        return true;
    }
    case T_STR: {
        out_str(t->o, "\"");
        for (const char *s = tk->text; *s; s++) {
            if (*s == '"') out_str(t->o, "\\\"");
            else if (*s == '\\') out_str(t->o, "\\\\");
            else if (*s == '\n') out_str(t->o, "\\n");
            else if (*s == '\t') out_str(t->o, "\\t");
            else out_put(t->o, *s);
        }
        out_str(t->o, "\"");
        next(t->p);
        return true;
    }
    default:
        fail(t, tk->line, "unexpected end of input", "", "");
        return false;
    }
}

/* ---- top level ---- */

/* (define name expr) -> let name = expr
 * (define (f a b) body...) -> let f(a, b) = <body> */
static bool define_form(T *t) {
    next(t->p); /* 'define' */
    if (peek(t->p)->kind == T_LP) {
        /* (define (f a b) body...) */
        next(t->p); /* '(' */
        const STok *n = peek(t->p);
        if (n->kind != T_ATOM) {
            fail(t, n->line, "expected a function name after define (", "", "");
            return false;
        }
        char *m = mangle(t->p, t->ns, n->text, n->line, true);
        if (!m) return false;
        out_str(t->o, "let ");
        out_str(t->o, m);
        free(m);
        out_str(t->o, "(");
        next(t->p); /* name */
        name_mark(t->ns);
        bool first = true;
        while (peek(t->p)->kind != T_RP) {
            const STok *pn = peek(t->p);
            if (pn->kind != T_ATOM) {
                fail(t, pn->line, "expected a parameter name", "", "");
                return false;
            }
            if (!first) out_str(t->o, ", ");
            char *pm = mangle(t->p, t->ns, pn->text, pn->line, true);
            if (!pm) return false;
            out_str(t->o, pm);
            free(pm);
            next(t->p);
            first = false;
        }
        if (first) {
            fail(t, peek(t->p)->line, "zero-argument functions are not supported", "", "");
            return false;
        }
        next(t->p); /* ')' */
        out_str(t->o, ") = ");
        ERes e;
        bool ok = seq_body(t, &e);
        name_pop(t->ns);
        return ok;
    }
    const STok *n = peek(t->p);
    if (n->kind != T_ATOM) {
        fail(t, n->line, "expected a name after define", "", "");
        return false;
    }
    next(t->p);
    char *m = mangle(t->p, t->ns, n->text, n->line, true);
    if (!m) return false;
    out_str(t->o, "let ");
    out_str(t->o, m);
    free(m);
    out_str(t->o, " = ");
    ERes e;
    return expr(t, &e);
}

char *scheme_to_yac(const char *src, char **errmsg) {
    SP p = {0};
    lex(&p, src);
    if (p.err) {
        *errmsg = p.err;
        for (int i = 0; i < p.n; i++) free(p.t[i].text);
        free(p.t);
        return NULL;
    }
    Names ns = {0};
    Out o = {0};
    T t = {&p, &ns, &o};
    while (!p.err && peek(&p)->kind != T_EOF) {
        if (peek(&p)->kind == T_LP && peek2k(&p) == T_ATOM &&
            strcmp(peek2t(&p), "define") == 0) {
            next(&p); /* '(' */
            if (!define_form(&t)) break;
            if (peek(&p)->kind != T_RP) {
                serr(&p, peek(&p)->line, "expected ')' after define", "", "");
                break;
            }
            next(&p); /* ')' */
        } else {
            ERes e;
            if (!expr(&t, &e)) break;
        }
        out_str(&o, ";\n");
    }
    for (int i = 0; i < p.n; i++) free(p.t[i].text);
    free(p.t);
    if (p.err) {
        *errmsg = p.err;
        for (int i = 0; i < ns.n; i++) free(ns.names[i]);
        free(ns.names);
        free(o.data);
        return NULL;
    }
    for (int i = 0; i < ns.n; i++) free(ns.names[i]);
    free(ns.names);
    *errmsg = NULL;
    return o.data ? o.data : strdup("");
}
