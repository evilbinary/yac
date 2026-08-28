#include "ast.h"

#include <stdio.h>

const char *binop_prim_name(int op) {
    switch (op) {
    case OP_ADD: return "+";
    case OP_SUB: return "-";
    case OP_MUL: return "*";
    case OP_DIV: return "/";
    case OP_MOD: return "%";
    case OP_EQ: return "==";
    case OP_NE: return "!=";
    case OP_LT: return "<";
    case OP_LE: return "<=";
    case OP_GT: return ">";
    case OP_GE: return ">=";
    case OP_AND: return "and";
    case OP_OR: return "or";
    }
    return "?";
}

const char *op_name(int op) {
    switch (op) {
    case OP_ADD: return "add";
    case OP_SUB: return "sub";
    case OP_MUL: return "mul";
    case OP_DIV: return "div";
    case OP_MOD: return "mod";
    case OP_EQ: return "eq";
    case OP_NE: return "ne";
    case OP_LT: return "lt";
    case OP_LE: return "le";
    case OP_GT: return "gt";
    case OP_GE: return "ge";
    case OP_AND: return "and";
    case OP_OR: return "or";
    }
    return "?";
}

static void indent(int depth) {
    for (int i = 0; i < depth; i++) printf("  ");
}

void ast_dump(const Ast *n, int depth) {
    if (!n) { indent(depth); printf("<null>\n"); return; }
    indent(depth);
    switch (n->kind) {
    case A_INT: printf("int %lld\n", (long long)n->u.ival); break;
    case A_BIG: printf("big %s\n", n->u.sval); break;
    case A_FLOAT: printf("float %g\n", n->u.fval); break;
    case A_STR: printf("str \"%s\"\n", n->u.sval); break;
    case A_BOOL: printf("bool %s\n", n->u.bval ? "true" : "false"); break;
    case A_UNIT: printf("unit\n"); break;
    case A_VAR: printf("var %s\n", n->u.name); break;
    case A_APP:
        printf("app\n");
        ast_dump(n->u.app.fn, depth + 1);
        for (int i = 0; i < n->u.app.nargs; i++) ast_dump(n->u.app.args[i], depth + 1);
        break;
    case A_FUN:
        printf("fun (");
        for (int i = 0; i < n->u.fun.nparams; i++) printf("%s%s", i ? ", " : "", n->u.fun.params[i]);
        printf(")\n");
        ast_dump(n->u.fun.body, depth + 1);
        break;
    case A_IF:
        printf("if\n");
        ast_dump(n->u.if_.cond, depth + 1);
        ast_dump(n->u.if_.then, depth + 1);
        ast_dump(n->u.if_.els, depth + 1);
        break;
    case A_LET:
        printf("let %s =\n", n->u.let.name);
        ast_dump(n->u.let.bound, depth + 1);
        indent(depth); printf("in\n");
        ast_dump(n->u.let.body, depth + 1);
        break;
    case A_BINOP:
        printf("binop %s\n", op_name(n->u.bin.op));
        ast_dump(n->u.bin.lhs, depth + 1);
        ast_dump(n->u.bin.rhs, depth + 1);
        break;
    case A_NOT: printf("not\n"); ast_dump(n->u.operand, depth + 1); break;
    case A_PRINT:
        printf("print\n");
        ast_dump(n->u.print.val, depth + 1);
        if (n->u.print.nl) ast_dump(n->u.print.nl, depth + 1);
        break;
    case A_CALLCC: printf("callcc\n"); ast_dump(n->u.operand, depth + 1); break;
    case A_THROW: printf("throw\n"); ast_dump(n->u.thr.k, depth + 1); ast_dump(n->u.thr.v, depth + 1); break;
    case A_LIST:
        printf("list\n");
        for (int i = 0; i < n->u.list.n; i++) ast_dump(n->u.list.items[i], depth + 1);
        break;
    }
}
