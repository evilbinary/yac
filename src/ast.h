#ifndef YAC_AST_H
#define YAC_AST_H

#include <stdbool.h>
#include <stdint.h>

#include "arena.h"

typedef struct Ast Ast;

enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_AND, OP_OR,
};

typedef enum {
    A_INT, A_BIG, A_FLOAT, A_STR, A_BOOL, A_UNIT,
    A_VAR,
    A_APP, A_FUN, A_IF, A_LET,
    A_BINOP, A_NOT, A_PRINT, A_CALLCC, A_THROW,
    A_LIST,
} AstKind;

struct Ast {
    AstKind kind;
    int line, col;
    union {
        int64_t ival;
        double fval;
        bool bval;
        char *sval;
        char *name;
        struct { Ast *fn; Ast **args; int nargs; } app;
        struct { char **params; int nparams; Ast *body; } fun;
        struct { Ast *cond, *then, *els; } if_;
        struct { char *name; Ast *bound; Ast *body; } let;
        struct { int op; Ast *lhs, *rhs; } bin;
        Ast *operand;
        struct { Ast *k, *v; } thr;
        struct { Ast **items; int n; } list;
    } u;
};

const char *binop_prim_name(int op);
const char *op_name(int op);

void ast_dump(const Ast *node, int depth);

#endif
