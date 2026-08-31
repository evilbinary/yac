#include "lexer.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *tok_kind_name(TokKind k) {
    switch (k) {
    case TK_EOF: return "end of file";
    case TK_IDENT: return "identifier";
    case TK_INT: return "integer";
    case TK_FLOAT: return "float";
    case TK_STRING: return "string";
    case TK_KW_LET: return "'let'";
    case TK_KW_IN: return "'in'";
    case TK_KW_FUN: return "'fun'";
    case TK_KW_IF: return "'if'";
    case TK_KW_THEN: return "'then'";
    case TK_KW_ELSE: return "'else'";
    case TK_KW_NOT: return "'not'";
    case TK_KW_PRINT: return "'print'";
    case TK_KW_CALLCC: return "'callcc'";
    case TK_KW_THROW: return "'throw'";
    case TK_KW_AND: return "'and'";
    case TK_KW_OR: return "'or'";
    case TK_KW_PACKAGE: return "'package'";
    case TK_KW_IMPORT: return "'import'";
    case TK_KW_EXPORT: return "'export'";
    case TK_LPAREN: return "'('";
    case TK_RPAREN: return "')'";
    case TK_LBRACKET: return "'['";
    case TK_RBRACKET: return "']'";
    case TK_LBRACE: return "'{'";
    case TK_RBRACE: return "'}'";
    case TK_DOT: return "'.'";
    case TK_COMMA: return "','";
    case TK_SEMI: return "';'";
    case TK_EQ: return "'='";
    case TK_ARROW: return "'->'";
    case TK_PLUS: return "'+'";
    case TK_MINUS: return "'-'";
    case TK_STAR: return "'*'";
    case TK_SLASH: return "'/'";
    case TK_PERCENT: return "'%'";
    case TK_EQEQ: return "'=='";
    case TK_NEQ: return "'!='";
    case TK_LT: return "'<'";
    case TK_LE: return "'<='";
    case TK_GT: return "'>'";
    case TK_GE: return "'>='";
    case TK_TRUE: return "'true'";
    case TK_FALSE: return "'false'";
    case TK_UNIT: return "'()'";
    }
    return "?";
}

static const struct {
    const char *word;
    TokKind kind;
} KW[] = {
    {"let", TK_KW_LET}, {"in", TK_KW_IN}, {"fun", TK_KW_FUN},
    {"if", TK_KW_IF}, {"then", TK_KW_THEN}, {"else", TK_KW_ELSE},
    {"not", TK_KW_NOT}, {"print", TK_KW_PRINT}, {"callcc", TK_KW_CALLCC},
    {"throw", TK_KW_THROW}, {"and", TK_KW_AND}, {"or", TK_KW_OR},
    {"package", TK_KW_PACKAGE}, {"import", TK_KW_IMPORT}, {"export", TK_KW_EXPORT},
    {"true", TK_TRUE}, {"false", TK_FALSE},
};

static TokKind lookup_kw(const char *s) {
    for (size_t i = 0; i < sizeof(KW) / sizeof(KW[0]); i++) {
        if (strcmp(KW[i].word, s) == 0) return KW[i].kind;
    }
    return TK_IDENT;
}

typedef struct {
    const char *src;
    size_t pos;
    int line, col;
} Lexer;

static void tok_push(Token **toks, int *n, int *cap, Token t) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *toks = (Token *)realloc(*toks, (size_t)(*cap) * sizeof(Token));
    }
    (*toks)[(*n)++] = t;
}

LexResult lex_program(const char *src, Arena *a) {
    LexResult res = {0};
    Token *toks = NULL;
    int n = 0, cap = 0;
    Lexer lx = {src, 0, 1, 1};
    char errbuf[512];

    for (;;) {
        /* skip whitespace and comments */
        for (;;) {
            char c = src[lx.pos];
            if (c == ' ' || c == '\t' || c == '\r') { lx.pos++; lx.col++; }
            else if (c == '\n') { lx.pos++; lx.line++; lx.col = 1; }
            else if (c == '-' && src[lx.pos + 1] == '-') {
                while (src[lx.pos] && src[lx.pos] != '\n') { lx.pos++; lx.col++; }
            } else if (c == '/' && src[lx.pos + 1] == '*') {
                /* Double-star block comments: open with slash-star-star (not
                 * slash-star-star-slash, which is an empty nested comment).
                 * They close only on star-star-slash, so a glob like src/star
                 * or interp/star-slash.yac can appear in the body. */
                if (src[lx.pos + 2] == '*' && src[lx.pos + 3] && src[lx.pos + 3] != '/') {
                    lx.pos += 3; lx.col += 3;
                    int closed = 0;
                    while (src[lx.pos]) {
                        if (src[lx.pos] == '*' && src[lx.pos + 1] == '*' && src[lx.pos + 2] == '/') {
                            lx.pos += 3; lx.col += 3;
                            closed = 1;
                            break;
                        }
                        if (src[lx.pos] == '\n') { lx.pos++; lx.line++; lx.col = 1; }
                        else { lx.pos++; lx.col++; }
                    }
                    if (!closed) {
                        snprintf(errbuf, sizeof(errbuf), "%d:%d: unterminated block comment", lx.line, lx.col);
                        goto err;
                    }
                } else {
                    lx.pos += 2; lx.col += 2;
                    int depth = 1;
                    while (src[lx.pos] && depth > 0) {
                        /* slash-star-dot is a path glob (rt then slash-star-dot yac), not a nest. */
                        if (src[lx.pos] == '/' && src[lx.pos + 1] == '*' && src[lx.pos + 2] == '.') {
                            lx.pos++; lx.col++;
                        } else if (src[lx.pos] == '/' && src[lx.pos + 1] == '*') {
                            depth++; lx.pos += 2; lx.col += 2;
                        } else if (src[lx.pos] == '*' && src[lx.pos + 1] == '/') { depth--; lx.pos += 2; lx.col += 2; }
                        else if (src[lx.pos] == '\n') { lx.pos++; lx.line++; lx.col = 1; }
                        else { lx.pos++; lx.col++; }
                    }
                    if (depth > 0) {
                        snprintf(errbuf, sizeof(errbuf), "%d:%d: unterminated block comment", lx.line, lx.col);
                        goto err;
                    }
                }
            } else break;
        }

        char c = src[lx.pos];
        if (!c) break;

        int tline = lx.line, tcol = lx.col;
        Token t = {0};
        t.line = tline;
        t.col = tcol;

        /* identifiers: ASCII letters/digits/_ plus any non-ASCII byte, so
         * UTF-8 names (e.g. Chinese identifiers) are accepted */
        if (isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80) {
            size_t start = lx.pos;
            while (isalnum((unsigned char)src[lx.pos]) || src[lx.pos] == '_' ||
                   (unsigned char)src[lx.pos] >= 0x80) {
                lx.pos++;
                lx.col++;
            }
            size_t len = lx.pos - start;
            char *word = (char *)arena_alloc(a, len + 1);
            memcpy(word, src + start, len);
            word[len] = '\0';
            t.text = word;
            t.kind = lookup_kw(word);
            tok_push(&toks, &n, &cap, t);
        } else if (isdigit((unsigned char)c)) {
            size_t start = lx.pos;
            while (isdigit((unsigned char)src[lx.pos])) { lx.pos++; lx.col++; }
            bool is_float = false;
            if (src[lx.pos] == '.' && isdigit((unsigned char)src[lx.pos + 1])) {
                is_float = true;
                lx.pos++; lx.col++;
                while (isdigit((unsigned char)src[lx.pos])) { lx.pos++; lx.col++; }
            }
            /* scientific notation: 1e10, 2.5e-3, 1E+5 */
            if ((src[lx.pos] == 'e' || src[lx.pos] == 'E')) {
                size_t save = lx.pos;
                lx.pos++; lx.col++;
                if (src[lx.pos] == '+' || src[lx.pos] == '-') { lx.pos++; lx.col++; }
                if (isdigit((unsigned char)src[lx.pos])) {
                    is_float = true;
                    while (isdigit((unsigned char)src[lx.pos])) { lx.pos++; lx.col++; }
                } else {
                    lx.pos = save; /* not an exponent; leave it for the caller */
                }
            }
            size_t len = lx.pos - start;
            char *num = (char *)malloc(len + 1);
            memcpy(num, src + start, len);
            num[len] = '\0';
            if (is_float) {
                t.kind = TK_FLOAT;
                t.fval = strtod(num, NULL);
            } else {
                t.kind = TK_INT;
                t.ival = strtoll(num, NULL, 10);
                /* a literal that overflows int64 becomes a bignum literal;
                 * keep the full digit text (arena-owned) */
                errno = 0;
                strtoll(num, NULL, 10);
                if (errno == ERANGE) {
                    t.big = true;
                    t.text = arena_strdup(a, num);
                    t.ival = 0;
                }
            }
            free(num);
            tok_push(&toks, &n, &cap, t);
        } else if (c == '"') {
            lx.pos++; lx.col++;
            size_t len = 0, bcap = 16;
            char *buf = (char *)malloc(bcap);
            while (src[lx.pos] && src[lx.pos] != '"') {
                char ch = src[lx.pos];
                if (ch == '\\') {
                    lx.pos++; lx.col++;
                    char esc = src[lx.pos];
                    switch (esc) {
                    case 'n': ch = '\n'; break;
                    case 't': ch = '\t'; break;
                    case '\\': ch = '\\'; break;
                    case '"': ch = '"'; break;
                    case '0': ch = '\0'; break;
                    default:
                        snprintf(errbuf, sizeof(errbuf), "%d:%d: bad escape sequence '\\%c'", lx.line, lx.col, esc);
                        free(buf);
                        goto err;
                    }
                    lx.pos++; lx.col++;
                } else if (ch == '\n') {
                    free(buf);
                    snprintf(errbuf, sizeof(errbuf), "%d:%d: unterminated string literal", lx.line, lx.col);
                    goto err;
                } else {
                    lx.pos++; lx.col++;
                }
                if (len + 1 >= bcap) { bcap *= 2; buf = (char *)realloc(buf, bcap); }
                buf[len++] = ch;
            }
            if (src[lx.pos] != '"') {
                free(buf);
                snprintf(errbuf, sizeof(errbuf), "%d:%d: unterminated string literal", lx.line, lx.col);
                goto err;
            }
            lx.pos++; lx.col++;
            buf[len] = '\0';
            t.kind = TK_STRING;
            t.text = arena_strdup(a, buf);
            free(buf);
            tok_push(&toks, &n, &cap, t);
        } else if (c == '(' && src[lx.pos + 1] == ')') {
            lx.pos += 2; lx.col += 2;
            t.kind = TK_UNIT;
            tok_push(&toks, &n, &cap, t);
        } else if (c == '(') { lx.pos++; lx.col++; t.kind = TK_LPAREN; tok_push(&toks, &n, &cap, t); }
        else if (c == ')') { lx.pos++; lx.col++; t.kind = TK_RPAREN; tok_push(&toks, &n, &cap, t); }
        else if (c == '[') { lx.pos++; lx.col++; t.kind = TK_LBRACKET; tok_push(&toks, &n, &cap, t); }
        else if (c == ']') { lx.pos++; lx.col++; t.kind = TK_RBRACKET; tok_push(&toks, &n, &cap, t); }
        else if (c == '{') { lx.pos++; lx.col++; t.kind = TK_LBRACE; tok_push(&toks, &n, &cap, t); }
        else if (c == '}') { lx.pos++; lx.col++; t.kind = TK_RBRACE; tok_push(&toks, &n, &cap, t); }
        else if (c == '.') { lx.pos++; lx.col++; t.kind = TK_DOT; tok_push(&toks, &n, &cap, t); }
        else if (c == ',') { lx.pos++; lx.col++; t.kind = TK_COMMA; tok_push(&toks, &n, &cap, t); }
        else if (c == ';') { lx.pos++; lx.col++; t.kind = TK_SEMI; tok_push(&toks, &n, &cap, t); }
        else if (c == '=' && src[lx.pos + 1] == '=') { lx.pos += 2; lx.col += 2; t.kind = TK_EQEQ; tok_push(&toks, &n, &cap, t); }
        else if (c == '=' && src[lx.pos + 1] == '>') { lx.pos += 2; lx.col += 2; t.kind = TK_ARROW; tok_push(&toks, &n, &cap, t); }
        else if (c == '=') { lx.pos++; lx.col++; t.kind = TK_EQ; tok_push(&toks, &n, &cap, t); }
        else if (c == '!' && src[lx.pos + 1] == '=') { lx.pos += 2; lx.col += 2; t.kind = TK_NEQ; tok_push(&toks, &n, &cap, t); }
        else if (c == '<' && src[lx.pos + 1] == '=') { lx.pos += 2; lx.col += 2; t.kind = TK_LE; tok_push(&toks, &n, &cap, t); }
        else if (c == '<') { lx.pos++; lx.col++; t.kind = TK_LT; tok_push(&toks, &n, &cap, t); }
        else if (c == '>' && src[lx.pos + 1] == '=') { lx.pos += 2; lx.col += 2; t.kind = TK_GE; tok_push(&toks, &n, &cap, t); }
        else if (c == '>') { lx.pos++; lx.col++; t.kind = TK_GT; tok_push(&toks, &n, &cap, t); }
        else if (c == '+') { lx.pos++; lx.col++; t.kind = TK_PLUS; tok_push(&toks, &n, &cap, t); }
        else if (c == '-' && src[lx.pos + 1] == '>') { lx.pos += 2; lx.col += 2; t.kind = TK_ARROW; tok_push(&toks, &n, &cap, t); }
        else if (c == '-') { lx.pos++; lx.col++; t.kind = TK_MINUS; tok_push(&toks, &n, &cap, t); }
        else if (c == '*') { lx.pos++; lx.col++; t.kind = TK_STAR; tok_push(&toks, &n, &cap, t); }
        else if (c == '/') { lx.pos++; lx.col++; t.kind = TK_SLASH; tok_push(&toks, &n, &cap, t); }
        else if (c == '%') { lx.pos++; lx.col++; t.kind = TK_PERCENT; tok_push(&toks, &n, &cap, t); }
        else {
            snprintf(errbuf, sizeof(errbuf), "%d:%d: unexpected character '%c'", lx.line, lx.col, c);
            goto err;
        }
    }

    Token e = {0};
    e.kind = TK_EOF;
    e.line = lx.line;
    e.col = lx.col;
    tok_push(&toks, &n, &cap, e);

    res.toks = toks;
    res.n = n;
    return res;

err:
    free(toks);
    res.toks = NULL;
    res.n = 0;
    res.error = arena_strdup(a, errbuf);
    return res;
}
