#ifndef YAC_PARSER_H
#define YAC_PARSER_H

#include "arena.h"
#include "ast.h"
#include "lexer.h"

typedef struct ParseResult {
    Ast *program; /* single AST: chain of top-level lets + final expr */
    char *error;  /* arena-allocated, NULL if OK */
} ParseResult;

ParseResult parse_program(const Token *toks, int n, Arena *a);

/* --pkg DIR[,DIR...]. 0 on success; -1 if already set or empty segment. */
int yac_pkg_set(const char *csv);

#endif
