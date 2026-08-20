#ifndef YAC_BIGNUM_H
#define YAC_BIGNUM_H

#include <stdbool.h>
#include <stdint.h>

#include "value.h"

/* Bignum arithmetic. All results are freshly allocated GC objects; callers
 * must keep operands alive (rooted) for the duration of the call. */

/* Construct from an int64. */
Bignum *bignum_from_i64(Gc *g, int64_t i);

/* Try to narrow to int64; returns true on success. */
bool bignum_to_i64(const Bignum *b, int64_t *out);

/* result = a + b */
Bignum *bignum_add(Gc *g, const Bignum *a, const Bignum *b);
/* result = a - b */
Bignum *bignum_sub(Gc *g, const Bignum *a, const Bignum *b);
/* result = a * b */
Bignum *bignum_mul(Gc *g, const Bignum *a, const Bignum *b);
/* result = a / b (truncated toward zero); *ok=false on division by zero */
Bignum *bignum_div(Gc *g, const Bignum *a, const Bignum *b, bool *ok);
/* result = a % b (sign follows the dividend); *ok=false on division by zero */
Bignum *bignum_mod(Gc *g, const Bignum *a, const Bignum *b, bool *ok);

/* -1/0/+1 */
int bignum_cmp(const Bignum *a, const Bignum *b);
int bignum_cmp_i64(const Bignum *b, int64_t i);
bool bignum_is_zero(const Bignum *b);
double bignum_to_double(const Bignum *b);

/* decimal string (malloc'd; caller frees) */
char *bignum_to_string(const Bignum *b);
/* parse a decimal string into a GC-allocated bignum; NULL on invalid input */
Bignum *bignum_from_dec(Gc *g, const char *s);
/* parse a decimal string into a preallocated (GC- or arena-) region sized
 * sizeof(Bignum) + ndigits*4; NULL on invalid input */
Bignum *bignum_from_dec_in(GObj *mem, const char *s);
/* parse a decimal string into an arena-resident bignum; NULL on invalid input */
Bignum *bignum_from_dec_arena(Arena *a, const char *s);

#endif
