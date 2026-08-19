#include "rtio.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ---- writer ---- */

static void put_name(FILE *f, const char *s) {
    fputc('"', f);
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

static void write_lit(Value v, FILE *f) {
    switch (v.tag) {
    case V_INT: fprintf(f, "i %lld", (long long)v.u.i); break;
    case V_FLOAT: fprintf(f, "f %.17g", v.u.f); break;
    case V_BOOL: fprintf(f, "b %d", v.u.b ? 1 : 0); break;
    case V_UNIT: fputs("u", f); break;
    case V_STR: fputs("s ", f); put_name(f, v.u.s->data); break;
    case V_LIST:
        fputs("l {", f);
        for (int i = 0; i < v.u.l->len; i++) {
            fputc(' ', f);
            write_lit(v.u.l->items[i], f);
        }
        fputs(" }", f);
        break;
    default: fprintf(f, "i 0"); break; /* functions/continuations are not IR literals */
    }
}

static void write_atom(const Atom *a, FILE *f) {
    switch (a->kind) {
    case AT_VAR:
        fprintf(f, "var ");
        put_name(f, a->u.var.name);
        fprintf(f, " %d %d", a->u.var.depth, a->u.var.slot);
        break;
    case AT_LIT:
        fputs("lit ", f);
        write_lit(a->u.lit, f);
        break;
    case AT_LAM:
        fprintf(f, "lam %d {", a->u.lam.nslots);
        for (int i = 0; i < a->u.lam.nparams; i++) {
            fputc(' ', f);
            put_name(f, a->u.lam.params[i]);
        }
        fputs(" } ", f);
        anf_write(a->u.lam.body, f);
        break;
    }
}

void anf_write(const Anf *node, FILE *f) {
    switch (node->kind) {
    case N_LET:
        fprintf(f, "(let ");
        put_name(f, node->u.let.name);
        fprintf(f, " %d ", node->u.let.slot);
        write_atom(&node->u.let.atom, f);
        fputc(' ', f);
        anf_write(node->u.let.body, f);
        fputs(")", f);
        break;
    case N_LET_CALL:
        fprintf(f, "(letcall ");
        put_name(f, node->u.call.name);
        fprintf(f, " %d ", node->u.call.slot);
        write_atom(&node->u.call.head, f);
        fputs(" {", f);
        for (int i = 0; i < node->u.call.nargs; i++) {
            fputc(' ', f);
            write_atom(&node->u.call.args[i], f);
        }
        fputs(" } ", f);
        anf_write(node->u.call.body, f);
        fputs(")", f);
        break;
    case N_IF:
        fputs("(if ", f);
        write_atom(&node->u.if_.cond, f);
        fputc(' ', f);
        anf_write(node->u.if_.then, f);
        fputc(' ', f);
        anf_write(node->u.if_.els, f);
        fputs(")", f);
        break;
    case N_TAIL_CALL:
        fputs("(tailcall ", f);
        write_atom(&node->u.tailcall.head, f);
        fputs(" {", f);
        for (int i = 0; i < node->u.tailcall.nargs; i++) {
            fputc(' ', f);
            write_atom(&node->u.tailcall.args[i], f);
        }
        fputs(" })", f);
        break;
    case N_RETURN:
        fputs("(ret ", f);
        write_atom(&node->u.ret, f);
        fputs(")", f);
        break;
    case N_LET_CALLCC:
        fputs("(letcallcc ", f);
        put_name(f, node->u.callcc.name);
        fprintf(f, " %d ", node->u.callcc.slot);
        write_atom(&node->u.callcc.atom, f);
        fputc(' ', f);
        anf_write(node->u.callcc.body, f);
        fputs(")", f);
        break;
    case N_TAIL_THROW:
        fputs("(tailthrow ", f);
        write_atom(&node->u.tailthrow.k, f);
        fputc(' ', f);
        write_atom(&node->u.tailthrow.v, f);
        fputs(")", f);
        break;
    }
}

/* ---- reader ---- */

struct Rd {
    const char *p;
    const char *end;
    Arena *a;
    char *err;
};

Rd *rd_open(const char *data, size_t len, Arena *a) {
    Rd *r = (Rd *)arena_alloc(a, sizeof(Rd));
    r->p = data;
    r->end = data + len;
    r->a = a;
    r->err = NULL;
    return r;
}

void rd_close(Rd *r) { (void)r; }

const char *rd_error(Rd *r) { return r->err; }

void rd_set_error(Rd *r, const char *msg) {
    if (!r->err) r->err = arena_strdup(r->a, msg);
}

Arena *rd_arena(Rd *r) { return r->a; }

static void rd_fail(Rd *r, const char *fmt, ...) {
    if (r->err) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    r->err = arena_strdup(r->a, buf);
}

static void rd_ws(Rd *r) {
    while (r->p < r->end && isspace((unsigned char)*r->p)) r->p++;
}

int rd_peek(Rd *r) {
    rd_ws(r);
    return r->p < r->end ? *r->p : 0;
}

char *rd_word(Rd *r) {
    rd_ws(r);
    const char *s = r->p;
    while (r->p < r->end && !isspace((unsigned char)*r->p) &&
           *r->p != '(' && *r->p != ')' && *r->p != '{' && *r->p != '}') {
        r->p++;
    }
    size_t n = (size_t)(r->p - s);
    char *w = (char *)arena_alloc(r->a, n + 1);
    memcpy(w, s, n);
    w[n] = '\0';
    return w;
}

char *rd_name(Rd *r) {
    rd_ws(r);
    if (r->p >= r->end || *r->p != '"') {
        rd_fail(r, "runtime file: expected a quoted name");
        return "";
    }
    r->p++;
    size_t cap = 32, n = 0;
    char *buf = (char *)arena_alloc(r->a, cap);
    while (r->p < r->end && *r->p != '"') {
        char c = *r->p++;
        if (c == '\\' && r->p < r->end) c = *r->p++;
        if (n + 1 >= cap) {
            cap *= 2;
            char *nb = (char *)arena_alloc(r->a, cap);
            memcpy(nb, buf, n);
            buf = nb;
        }
        buf[n++] = c;
    }
    if (r->p < r->end) r->p++; /* closing quote */
    buf[n] = '\0';
    return buf;
}

void rd_expect(Rd *r, char c) {
    rd_ws(r);
    if (r->p < r->end && *r->p == c) {
        r->p++;
    } else {
        rd_fail(r, "runtime file: expected '%c'", c);
    }
}

static Value rd_lit(Rd *r) {
    char *tag = rd_word(r);
    if (strcmp(tag, "i") == 0) return v_int(strtoll(rd_word(r), NULL, 10));
    if (strcmp(tag, "f") == 0) return v_float(strtod(rd_word(r), NULL));
    if (strcmp(tag, "b") == 0) return v_bool(atoi(rd_word(r)) != 0);
    if (strcmp(tag, "u") == 0) return v_unit();
    if (strcmp(tag, "s") == 0) return v_str(r->a, rd_name(r));
    if (strcmp(tag, "l") == 0) {
        rd_expect(r, '{');
        Value *items = NULL;
        int n = 0, cap = 0;
        while (rd_peek(r) != '}') {
            if (n >= cap) {
                cap = cap ? cap * 2 : 4;
                Value *np = (Value *)arena_alloc(r->a, (size_t)cap * sizeof(Value));
                memcpy(np, items, (size_t)n * sizeof(Value));
                items = np;
            }
            items[n++] = rd_lit(r);
        }
        rd_expect(r, '}');
        return v_list_arena(r->a, items, n);
    }
    rd_fail(r, "runtime file: bad literal tag '%s'", tag);
    return VALUE_NULL;
}

static Anf *rd_node(Rd *r);

bool rd_atom(Rd *r, Atom *out) {
    char *kw = rd_word(r);
    if (strcmp(kw, "var") == 0) {
        char *name = rd_name(r);
        int depth = atoi(rd_word(r));
        int slot = atoi(rd_word(r));
        *out = atom_var_ds(name, depth, slot);
        return true;
    }
    if (strcmp(kw, "lit") == 0) {
        *out = atom_lit(rd_lit(r));
        return true;
    }
    if (strcmp(kw, "lam") == 0) {
        int nslots = atoi(rd_word(r));
        rd_expect(r, '{');
        char **params = NULL;
        int n = 0, cap = 0;
        while (rd_peek(r) == '"') {
            if (n >= cap) {
                cap = cap ? cap * 2 : 4;
                char **np = (char **)arena_alloc(r->a, (size_t)cap * sizeof(char *));
                memcpy(np, params, (size_t)n * sizeof(char *));
                params = np;
            }
            params[n++] = rd_name(r);
        }
        rd_expect(r, '}');
        Anf *body = rd_node(r);
        *out = atom_lam(params, n, nslots, body);
        return true;
    }
    rd_fail(r, "runtime file: bad atom '%s'", kw);
    return false;
}

static Anf *rd_node(Rd *r) {
    rd_expect(r, '(');
    char *kw = rd_word(r);
    Anf *n = NULL;
    if (strcmp(kw, "let") == 0) {
        char *name = rd_name(r);
        int slot = atoi(rd_word(r));
        Atom atom;
        if (!rd_atom(r, &atom)) return NULL;
        Anf *body = rd_node(r);
        n = anf_let(r->a, name, slot, atom, body);
    } else if (strcmp(kw, "letcall") == 0) {
        char *name = rd_name(r);
        int slot = atoi(rd_word(r));
        Atom head;
        if (!rd_atom(r, &head)) return NULL;
        rd_expect(r, '{');
        Atom *args = NULL;
        int na = 0, cap = 0;
        while (rd_peek(r) != '}') {
            Atom a;
            if (!rd_atom(r, &a)) return NULL;
            if (na >= cap) {
                cap = cap ? cap * 2 : 4;
                Atom *np = (Atom *)arena_alloc(r->a, (size_t)cap * sizeof(Atom));
                memcpy(np, args, (size_t)na * sizeof(Atom));
                args = np;
            }
            args[na++] = a;
        }
        rd_expect(r, '}');
        Anf *body = rd_node(r);
        n = anf_let_call(r->a, name, slot, head, args, na, body);
    } else if (strcmp(kw, "if") == 0) {
        Atom cond;
        if (!rd_atom(r, &cond)) return NULL;
        Anf *t = rd_node(r);
        Anf *e = rd_node(r);
        n = anf_if(r->a, cond, t, e);
    } else if (strcmp(kw, "tailcall") == 0) {
        Atom head;
        if (!rd_atom(r, &head)) return NULL;
        rd_expect(r, '{');
        Atom *args = NULL;
        int na = 0, cap = 0;
        while (rd_peek(r) != '}') {
            Atom a;
            if (!rd_atom(r, &a)) return NULL;
            if (na >= cap) {
                cap = cap ? cap * 2 : 4;
                Atom *np = (Atom *)arena_alloc(r->a, (size_t)cap * sizeof(Atom));
                memcpy(np, args, (size_t)na * sizeof(Atom));
                args = np;
            }
            args[na++] = a;
        }
        rd_expect(r, '}');
        n = anf_tail_call(r->a, head, args, na);
    } else if (strcmp(kw, "ret") == 0) {
        Atom atom;
        if (!rd_atom(r, &atom)) return NULL;
        n = anf_ret(r->a, atom);
    } else if (strcmp(kw, "letcallcc") == 0) {
        char *name = rd_name(r);
        int slot = atoi(rd_word(r));
        Atom atom;
        if (!rd_atom(r, &atom)) return NULL;
        Anf *body = rd_node(r);
        n = anf_let_callcc(r->a, name, slot, atom, body);
    } else if (strcmp(kw, "tailthrow") == 0) {
        Atom k;
        Atom v;
        if (!rd_atom(r, &k) || !rd_atom(r, &v)) return NULL;
        n = anf_tail_throw(r->a, k, v);
    } else {
        rd_fail(r, "runtime file: bad node '%s'", kw);
        return NULL;
    }
    rd_expect(r, ')');
    return n;
}

bool anf_read(Rd *r, Anf **out, int *top_nslots) {
    rd_expect(r, '(');
    char *kw = rd_word(r);
    if (rd_error(r) || strcmp(kw, "rt") != 0) {
        if (!rd_error(r)) rd_fail(r, "runtime file: expected (rt ...) header");
        return false;
    }
    int top = atoi(rd_word(r));
    Anf *node = rd_node(r);
    if (rd_error(r)) return false;
    *out = node;
    if (top_nslots) *top_nslots = top;
    return true;
}

bool anf_read_file(const char *path, Arena *a, Anf **out, int *top_nslots,
                   char **errmsg) {
    FILE *f = path ? fopen(path, "rb") : stdin;
    if (!f) {
        if (errmsg) *errmsg = "cannot open runtime file";
        return false;
    }
    char *buf = NULL;
    long n = 0;
    if (path) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = (char *)malloc((size_t)n + 1);
        if (!buf) {
            fclose(f);
            return false;
        }
        size_t got = fread(buf, 1, (size_t)n, f);
        buf[got] = '\0';
        fclose(f);
    } else {
        size_t cap = 4096, len = 0;
        buf = (char *)malloc(cap);
        int c;
        while ((c = fgetc(f)) != EOF) {
            if (len + 1 >= cap) {
                cap *= 2;
                buf = (char *)realloc(buf, cap);
            }
            buf[len++] = (char)c;
        }
        buf[len] = '\0';
        fclose(f);
        n = (long)len;
    }

    Rd *r = rd_open(buf, (size_t)n, a);
    bool ok = anf_read(r, out, top_nslots);
    const char *err = rd_error(r);
    rd_close(r);
    free(buf);
    if (!ok && errmsg) *errmsg = (char *)err;
    return ok;
}