#include "config.h"
#include "bignum.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"

/* ---- magnitude helpers (base-2^32 little-endian arrays) ---- */

static int mag_normlen(const uint32_t *d, int n) {
    while (n > 0 && d[n - 1] == 0) n--;
    return n;
}

/* -1/0/+1 */
static int mag_cmp(const uint32_t *a, int na, const uint32_t *b, int nb) {
    if (na != nb) return na < nb ? -1 : 1;
    for (int i = na - 1; i >= 0; i--)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* out must have room for max(na,nb)+1; returns significant length */
static int mag_add(const uint32_t *a, int na, const uint32_t *b, int nb,
                   uint32_t *out) {
    int n = na > nb ? na : nb;
    uint64_t carry = 0;
    for (int i = 0; i < n; i++) {
        uint64_t s = carry + (i < na ? a[i] : 0) + (i < nb ? b[i] : 0);
        out[i] = (uint32_t)s;
        carry = s >> 32;
    }
    if (carry) out[n++] = (uint32_t)carry;
    return n;
}

/* out may equal a; requires a >= b; returns significant length */
static int mag_sub(const uint32_t *a, int na, const uint32_t *b, int nb,
                   uint32_t *out) {
    int64_t borrow = 0;
    for (int i = 0; i < na; i++) {
        int64_t d = (int64_t)a[i] - borrow - (i < nb ? b[i] : 0);
        if (d < 0) { d += (int64_t)1 << 32; borrow = 1; }
        else borrow = 0;
        out[i] = (uint32_t)d;
    }
    return mag_normlen(out, na);
}

/* out must have room for na+nb; returns significant length */
static int mag_mul(const uint32_t *a, int na, const uint32_t *b, int nb,
                   uint32_t *out) {
    memset(out, 0, (size_t)(na + nb) * sizeof(uint32_t));
    for (int i = 0; i < na; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < nb; j++) {
            uint64_t p = (uint64_t)a[i] * b[j] + out[i + j] + carry;
            out[i + j] = (uint32_t)p;
            carry = p >> 32;
        }
        int k = nb;
        while (carry) {
            uint64_t s = (uint64_t)out[i + k] + carry;
            out[i + k] = (uint32_t)s;
            carry = s >> 32;
            k++;
        }
    }
    return mag_normlen(out, na + nb);
}

static int mag_bitlen(const uint32_t *a, int n) {
    uint32_t top = a[n - 1];
    int bits = 0;
    while (top) { top >>= 1; bits++; }
    return (n - 1) * 32 + bits;
}

static bool mag_getbit(const uint32_t *a, int bit) {
    return (a[bit >> 5] >> (bit & 31)) & 1;
}

/* q = a / b, r = a % b (bit-by-bit restoring division).
 * q must have room for na digits; r for nb+1. Assumes a,b > 0. */
static void mag_divmod(const uint32_t *a, int na, const uint32_t *b, int nb,
                       uint32_t *q, int *nq, uint32_t *r, int *nr) {
    int blen = mag_bitlen(a, na);
    memset(q, 0, (size_t)na * sizeof(uint32_t));
    memset(r, 0, (size_t)(nb + 1) * sizeof(uint32_t));
    int rn = 0;
    for (int i = blen - 1; i >= 0; i--) {
        /* r <<= 1 */
        if (rn > 0) {
            uint32_t carry = 0;
            for (int j = 0; j < rn; j++) {
                uint32_t nx = r[j] >> 31;
                r[j] = (r[j] << 1) | carry;
                carry = nx;
            }
            if (carry) r[rn++] = carry;
        }
        /* r |= bit */
        if (mag_getbit(a, i)) {
            r[0] |= 1u;
            if (rn == 0) rn = 1;
        }
        if (rn > 0 && mag_cmp(r, rn, b, nb) >= 0) {
            rn = mag_sub(r, rn, b, nb, r);
            q[i >> 5] |= (1u << (i & 31));
        }
    }
    *nq = mag_normlen(q, na);
    *nr = rn;
}

/* ---- constructors ---- */

static Bignum *bignum_from_mag(Gc *g, const uint32_t *d, int n, int sign) {
    n = mag_normlen(d, n);
    if (n == 0) {
        Bignum *z = gc_new_bignum(g, 1);
        z->sign = 0;
        z->digits[0] = 0;
        return z;
    }
    Bignum *b = gc_new_bignum(g, n);
    b->sign = sign;
    b->ndigits = n;
    memcpy(b->digits, d, (size_t)n * sizeof(uint32_t));
    return b;
}

Bignum *bignum_from_i64(Gc *g, int64_t i) {
    if (i == 0) {
        Bignum *z = gc_new_bignum(g, 1);
        z->sign = 0;
        z->digits[0] = 0;
        return z;
    }
    uint64_t mag = i < 0 ? (uint64_t)(-(i + 1)) + 1 : (uint64_t)i;
    Bignum *b = gc_new_bignum(g, 2);
    b->sign = i < 0 ? -1 : 1;
    b->ndigits = (mag >> 32) ? 2 : 1;
    b->digits[0] = (uint32_t)mag;
    b->digits[1] = (uint32_t)(mag >> 32);
    return b;
}

bool bignum_to_i64(const Bignum *b, int64_t *out) {
    if (b->sign == 0) { *out = 0; return true; }
    if (b->ndigits > 2) return false;
    uint64_t mag = b->digits[0];
    if (b->ndigits == 2) mag |= (uint64_t)b->digits[1] << 32;
    if (b->sign > 0) {
        if (mag > (uint64_t)INT64_MAX) return false;
        *out = (int64_t)mag;
        return true;
    }
    if (mag == (uint64_t)INT64_MAX + 1) { *out = INT64_MIN; return true; }
    if (mag > (uint64_t)INT64_MAX + 1) return false;
    *out = -(int64_t)mag;
    return true;
}

int bignum_cmp(const Bignum *a, const Bignum *b) {
    if (a->sign != b->sign) return a->sign < b->sign ? -1 : 1;
    if (a->sign == 0) return 0;
    int c = mag_cmp(a->digits, a->ndigits, b->digits, b->ndigits);
    return a->sign > 0 ? c : -c;
}

int bignum_cmp_i64(const Bignum *b, int64_t i) {
    if (i == 0) return b->sign == 0 ? 0 : (b->sign > 0 ? 1 : -1);
    uint64_t mag = i < 0 ? (uint64_t)(-(i + 1)) + 1 : (uint64_t)i;
    int isign = i < 0 ? -1 : 1;
    if (b->sign != isign) return b->sign < isign ? -1 : 1;
    if (b->sign == 0) return isign > 0 ? -1 : 1;
    uint32_t id[2];
    id[0] = (uint32_t)mag;
    id[1] = (uint32_t)(mag >> 32);
    int in = (mag >> 32) ? 2 : 1;
    int c = mag_cmp(b->digits, b->ndigits, id, in);
    return b->sign > 0 ? c : -c;
}

bool bignum_is_zero(const Bignum *b) {
    return b->sign == 0;
}

double bignum_to_double(const Bignum *b) {
    double d = 0.0;
    for (int i = b->ndigits - 1; i >= 0; i--)
        d = d * 4294967296.0 + b->digits[i];
    return b->sign < 0 ? -d : d;
}

Bignum *bignum_add(Gc *g, const Bignum *a, const Bignum *b) {
    if (a->sign == 0) return bignum_from_mag(g, b->digits, b->ndigits, b->sign);
    if (b->sign == 0) return bignum_from_mag(g, a->digits, a->ndigits, a->sign);
    int n = (a->ndigits > b->ndigits ? a->ndigits : b->ndigits) + 1;
    uint32_t *tmp = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    int tn, sign;
    if (a->sign == b->sign) {
        tn = mag_add(a->digits, a->ndigits, b->digits, b->ndigits, tmp);
        sign = a->sign;
    } else {
        int c = mag_cmp(a->digits, a->ndigits, b->digits, b->ndigits);
        if (c == 0) { free(tmp); Bignum *z = gc_new_bignum(g, 1); z->sign = 0; z->digits[0] = 0; return z; }
        if (c > 0) { tn = mag_sub(a->digits, a->ndigits, b->digits, b->ndigits, tmp); sign = a->sign; }
        else { tn = mag_sub(b->digits, b->ndigits, a->digits, a->ndigits, tmp); sign = b->sign; }
    }
    Bignum *r = bignum_from_mag(g, tmp, tn, sign);
    free(tmp);
    return r;
}

Bignum *bignum_sub(Gc *g, const Bignum *a, const Bignum *b) {
    if (b->sign == 0) return bignum_from_mag(g, a->digits, a->ndigits, a->sign);
    /* a - b == a + (-b): negate b's sign and reuse addition */
    Bignum *nb = bignum_from_mag(g, b->digits, b->ndigits, -b->sign);
    return bignum_add(g, a, nb);
}

Bignum *bignum_mul(Gc *g, const Bignum *a, const Bignum *b) {
    if (a->sign == 0 || b->sign == 0) {
        Bignum *z = gc_new_bignum(g, 1);
        z->sign = 0;
        z->digits[0] = 0;
        return z;
    }
    int n = a->ndigits + b->ndigits;
    uint32_t *tmp = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    int tn = mag_mul(a->digits, a->ndigits, b->digits, b->ndigits, tmp);
    Bignum *r = bignum_from_mag(g, tmp, tn, a->sign * b->sign);
    free(tmp);
    return r;
}

static Bignum *bignum_divmod(Gc *g, const Bignum *a, const Bignum *b, bool *ok,
                             bool want_rem) {
    if (b->sign == 0) { *ok = false; return NULL; }
    *ok = true;
    if (a->sign == 0) {
        Bignum *z = gc_new_bignum(g, 1);
        z->sign = 0;
        z->digits[0] = 0;
        return z;
    }
    int na = a->ndigits, nb = b->ndigits;
    uint32_t *q = (uint32_t *)malloc((size_t)na * sizeof(uint32_t));
    uint32_t *r = (uint32_t *)malloc((size_t)(nb + 1) * sizeof(uint32_t));
    int nq, nr;
    mag_divmod(a->digits, na, b->digits, nb, q, &nq, r, &nr);
    Bignum *res;
    if (want_rem)
        res = bignum_from_mag(g, r, nr, a->sign);
    else
        res = bignum_from_mag(g, q, nq, a->sign * b->sign);
    free(q);
    free(r);
    return res;
}

Bignum *bignum_div(Gc *g, const Bignum *a, const Bignum *b, bool *ok) {
    return bignum_divmod(g, a, b, ok, false);
}

Bignum *bignum_mod(Gc *g, const Bignum *a, const Bignum *b, bool *ok) {
    return bignum_divmod(g, a, b, ok, true);
}

/* ---- decimal I/O ---- */

char *bignum_to_string(const Bignum *b) {
    if (b->sign == 0) return strdup("0");
    int na = b->ndigits;
    uint32_t *dig = (uint32_t *)malloc((size_t)na * sizeof(uint32_t));
    memcpy(dig, b->digits, (size_t)na * sizeof(uint32_t));
    int ngrp = na + 2;
    char(*groups)[10] = (char(*)[10])malloc((size_t)ngrp * sizeof(char[10]));
    int ng = 0;
    while (na > 1 || dig[0] != 0) {
        /* divide by 10^9 */
        uint64_t acc = 0;
        for (int i = na - 1; i >= 0; i--) {
            acc = (acc << 32) | dig[i];
            dig[i] = (uint32_t)(acc / 1000000000u);
            acc %= 1000000000u;
        }
        snprintf(groups[ng++], 10, "%09u", (uint32_t)acc);
        na = mag_normlen(dig, na);
    }
    size_t total = (size_t)ng * 9 + 2;
    char *buf = (char *)malloc(total);
    size_t pos = 0;
    if (b->sign < 0) buf[pos++] = '-';
    char *top = groups[ng - 1];
    char *t = top;
    while (*t == '0' && t < top + 9) t++;
    if (t == top + 9) buf[pos++] = '0';
    else
        while (t < top + 9) buf[pos++] = *t++;
    for (int i = ng - 2; i >= 0; i--) {
        memcpy(buf + pos, groups[i], 9);
        pos += 9;
    }
    buf[pos] = '\0';
    free(groups);
    free(dig);
    return buf;
}

/* Build a bignum from a decimal string into a freshly allocated GObj
 * region (either GC- or arena-allocated). Returns NULL on invalid input. */
Bignum *bignum_from_dec_in(GObj *mem, const char *s) {
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    if (!*s) return NULL;
    size_t len = strlen(s);
    int cap = (int)(len / 3) + 2;
    uint32_t *dig = (uint32_t *)calloc((size_t)cap, sizeof(uint32_t));
    int n = 1; /* dig[0] = 0 */
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') { free(dig); return NULL; }
        uint32_t d = (uint32_t)(*p - '0');
        uint64_t carry = d;
        for (int i = 0; i < n; i++) {
            uint64_t v = (uint64_t)dig[i] * 10 + carry;
            dig[i] = (uint32_t)v;
            carry = v >> 32;
        }
        if (carry) {
            if (n >= cap) { cap *= 2; dig = (uint32_t *)realloc(dig, (size_t)cap * sizeof(uint32_t)); }
            dig[n++] = (uint32_t)carry;
        }
    }
    Bignum *b = (Bignum *)mem;
    b->sign = 0;
    b->ndigits = 1;
    b->digits[0] = 0;
    if (n > 1 || dig[0] != 0) {
        b->ndigits = n;
        memcpy(b->digits, dig, (size_t)n * sizeof(uint32_t));
        b->sign = sign;
    }
    free(dig);
    return b;
}

Bignum *bignum_from_dec(Gc *g, const char *s) {
    size_t cap = strlen(s) / 3 + 3;
    Bignum *b = gc_new_bignum(g, (int)cap);
    return bignum_from_dec_in((GObj *)b, s);
}

Bignum *bignum_from_dec_arena(Arena *a, const char *s) {
    size_t cap = strlen(s) / 3 + 3;
    Bignum *b = (Bignum *)arena_alloc(a, sizeof(Bignum) + cap * sizeof(uint32_t));
    memset(b, 0, sizeof(Bignum));
    b->hdr.kind = G_BIGNUM;
    return bignum_from_dec_in((GObj *)b, s);
}
