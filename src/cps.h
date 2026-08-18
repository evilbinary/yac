#ifndef YAC_CPS_H
#define YAC_CPS_H

#include <stdbool.h>

#include "arena.h"
#include "anf.h"
#include "value.h"

/* CPS core IR.
 *
 * Evaluation order is encoded as continuation-passing: every function takes
 * an extra continuation parameter, and functions never "return" -- they call
 * the continuation with the result. A continuation is just a value, which is
 * what makes callcc/throw cheap here.
 */

typedef enum {
    CV_VAR,  /* variable reference */
    CV_LIT,  /* literal value      */
    CV_PRIM, /* primitive name     */
    CV_FUN,  /* lambda: params + continuation param + body */
    CV_CONT, /* continuation closure: kappa(x). body        */
} CValKind;

typedef struct CVal CVal;
typedef struct CExp CExp;

struct CVal {
    CValKind kind;
    union {
        struct { const char *name; } var;
        Value lit;
        struct { const char *name; } prim;
        struct {
            char **params; /* user params, continuation param is the LAST one */
            int nparams;
            CExp *body;
        } fun;
        struct {
            const char *param;
            CExp *body;
        } cont;
    } u;
};

typedef enum {
    CE_LET,   /* let name = CVal in body                       */
    CE_CALL,  /* call head args* (args[last] is continuation)  */
    CE_THROW, /* throw k v : apply continuation k to value v   */
    CE_IF,    /* if cond then then else else                   */
    CE_HALT,  /* halt v : program result                       */
} CExpKind;

struct CExp {
    CExpKind kind;
    int line;
    union {
        struct {
            const char *name;
            CVal val;
            CExp *body;
        } let;
        struct {
            CVal head;
            CVal *args;
            int nargs;
        } call;
        struct {
            CVal k;
            CVal v;
        } throw_;
        struct {
            CVal cond;
            CExp *then;
            CExp *els;
        } if_;
        struct {
            CVal v;
        } halt;
    } u;
};

/* Convert an ANF program into CPS. Returns false and sets *errmsg on error. */
bool anf_to_cps(const Anf *anf, Arena *a, CExp **out, char **errmsg);

/* Peephole simplifications over CPS IR (semantics-preserving):
 *  - constant folding of pure primitive calls with literal arguments,
 *  - eta-reduction:  kappa(x). call k x  ==>  k
 *                    lambda(x*,k). call f x* k  ==>  f
 * Returns a freshly built (arena) expression. */
CExp *cps_simplify(const CExp *prog, Arena *a);

void cps_dump(const CExp *node, int depth);

#endif