#ifndef YAC_ANF_H
#define YAC_ANF_H

#include <stdbool.h>

#include "arena.h"
#include "ast.h"
#include "value.h"

/* A-Normal Form core IR.
 *
 * Every non-atomic computation is bound by a let; evaluation order is
 * explicit in the syntax. `call` includes user functions AND primitives
 * (the head atom may resolve to either at run time).
 *
 * Variable references are resolved at normalization time to flat-frame
 * addresses: (depth, slot) means "walk `depth` parent frames from the
 * current frame, then index `slot`". Each lambda carries its frame size
 * (nslots); let-bound names carry their slot in the current frame.
 */

typedef enum {
    N_LET,        /* let name = Atom in body          */
    N_LET_CALL,   /* let name = call(Atom, Atom*) in body */
    N_IF,         /* if Atom then b1 else b2          */
    N_TAIL_CALL,  /* call(Atom, Atom*)  (tail)        */
    N_RETURN,     /* return Atom                      */
    N_LET_CALLCC, /* let name = callcc(Atom) in body  -- rejected by ANF machine */
    N_TAIL_THROW, /* throw Atom Atom                  -- rejected by ANF machine */
} AnfKind;

typedef enum {
    AT_VAR, /* variable reference (flat frame address) */
    AT_LIT, /* literal value                            */
    AT_LAM, /* lambda: params + frame size + ANF body   */
} AtomKind;

typedef struct Anf Anf;

typedef struct Atom {
    AtomKind kind;
    union {
        struct {
            const char *name;
            int depth; /* frame depth from the current frame */
            int slot;  /* slot index in that frame          */
        } var;
        Value lit;
        struct {
            char **params;
            int nparams;
            int nslots; /* frame size: nparams + locals */
            Anf *body;
        } lam;
    } u;
} Atom;

struct Anf {
    AnfKind kind;
    int line;
    union {
        struct {
            const char *name;
            int slot; /* slot of `name` in the current frame */
            Atom atom;
            Anf *body;
        } let;
        struct {
            const char *name;
            int slot;
            Atom head;
            Atom *args;
            int nargs;
            Anf *body;
        } call;
        struct {
            Atom cond;
            Anf *then;
            Anf *els;
        } if_;
        struct {
            Atom head;
            Atom *args;
            int nargs;
        } tailcall;
        Atom ret;
        struct {
            const char *name;
            int slot;
            Atom atom;
            Anf *body;
        } callcc;
        struct {
            Atom k;
            Atom v;
        } tailthrow;
    } u;
};

/* Normalize a whole program AST into ANF. Returns false and sets *errmsg on
 * error (e.g. unbound variable). On success sets *top_nslots to the size of
 * the top-level frame. */
bool ast_to_anf(const Ast *prog, Arena *a, Anf **out, int *top_nslots,
                char **errmsg);

/* IR constructors (used by the CPS->ANF un-conversion too) */
Atom atom_var(const char *name);
Atom atom_var_ds(const char *name, int depth, int slot);
Atom atom_lit(Value v);
Atom atom_lam(char **params, int nparams, int nslots, Anf *body);
Anf *anf_let(Arena *a, const char *name, int slot, Atom atom, Anf *body);
Anf *anf_let_call(Arena *a, const char *name, int slot, Atom head, Atom *args, int nargs, Anf *body);
Anf *anf_if(Arena *a, Atom cond, Anf *then, Anf *els);
Anf *anf_tail_call(Arena *a, Atom head, Atom *args, int nargs);
Anf *anf_ret(Arena *a, Atom atom);
Anf *anf_let_callcc(Arena *a, const char *name, int slot, Atom atom, Anf *body);
Anf *anf_tail_throw(Arena *a, Atom k, Atom v);

void anf_dump(const Anf *node, int depth);

#endif