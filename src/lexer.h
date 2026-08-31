#ifndef YAC_LEXER_H
#define YAC_LEXER_H

#include <stdbool.h>
#include <stdint.h>

#include "arena.h"

typedef enum {
    TK_EOF, TK_IDENT, TK_INT, TK_FLOAT, TK_STRING,
    TK_KW_LET, TK_KW_IN, TK_KW_FUN, TK_KW_IF, TK_KW_THEN, TK_KW_ELSE,
    TK_KW_NOT, TK_KW_PRINT, TK_KW_CALLCC, TK_KW_THROW, TK_KW_AND, TK_KW_OR,
    TK_KW_PACKAGE, TK_KW_IMPORT, TK_KW_EXPORT,
    TK_LPAREN, TK_RPAREN, TK_COMMA, TK_SEMI, TK_EQ, TK_ARROW,
    TK_LBRACKET, TK_RBRACKET, TK_LBRACE, TK_RBRACE, TK_DOT,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_EQEQ, TK_NEQ, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_TRUE, TK_FALSE, TK_UNIT,
} TokKind;

typedef struct Token {
    TokKind kind;
    int line, col;
    const char *text; /* identifier, string value, or raw big-int text (arena-owned) */
    int64_t ival;
    double fval;
    bool big; /* TK_INT literal does not fit in int64; text holds the digits */
} Token;

typedef struct LexResult {
    Token *toks; /* malloc'd array, free after parsing */
    int n;
    char *error; /* arena-allocated message, NULL if OK */
} LexResult;

LexResult lex_program(const char *src, Arena *a);

const char *tok_kind_name(TokKind k);

#endif
