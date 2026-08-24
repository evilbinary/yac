#include "config.h"
#include "profile.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct ProfRec {
    const char *name;
    uint64_t ncalls;
    uint64_t incl_ns;
    uint64_t excl_ns;
    struct ProfRec *next;
} ProfRec;

typedef struct {
    const char *name;
    uint64_t t0;
    uint64_t child_ns;
} ProfFrame;

#define NBUCKET 4096

static int enabled;
static char *out_path;
static ProfRec *buckets[NBUCKET];
static int nrec;
static ProfFrame *stack;
static int nstk, cap_stk;

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t hash_name(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

static ProfRec *get_rec(const char *name) {
    uint32_t h;
    ProfRec *p;
    if (!name) name = "lam";
    h = hash_name(name) & (NBUCKET - 1);
    for (p = buckets[h]; p; p = p->next) {
        if (p->name == name || strcmp(p->name, name) == 0) return p;
    }
    p = (ProfRec *)calloc(1, sizeof(ProfRec));
    if (!p) return NULL;
    p->name = name;
    p->next = buckets[h];
    buckets[h] = p;
    nrec++;
    return p;
}

void yac_prof_enable(const char *path) {
    if (!path || !path[0]) return;
    enabled = 1;
    free(out_path);
    out_path = (char *)malloc(strlen(path) + 1);
    if (out_path) memcpy(out_path, path, strlen(path) + 1);
}

int yac_prof_enabled(void) { return enabled; }

void yac_prof_enter(const char *name) {
    ProfRec *r;
    if (!enabled) return;
    if (!name) name = "lam";
    if (nstk >= cap_stk) {
        int ncap = cap_stk ? cap_stk * 2 : 256;
        ProfFrame *n = (ProfFrame *)realloc(stack, (size_t)ncap * sizeof(ProfFrame));
        if (!n) return;
        stack = n;
        cap_stk = ncap;
    }
    r = get_rec(name);
    if (r) r->ncalls++;
    stack[nstk].name = r ? r->name : name;
    stack[nstk].t0 = now_ns();
    stack[nstk].child_ns = 0;
    nstk++;
}

void yac_prof_leave(void) {
    ProfRec *r;
    uint64_t elapsed;
    if (!enabled || nstk <= 0) return;
    nstk--;
    elapsed = now_ns() - stack[nstk].t0;
    r = get_rec(stack[nstk].name);
    if (r) {
        r->incl_ns += elapsed;
        r->excl_ns += elapsed - stack[nstk].child_ns;
    }
    if (nstk > 0) stack[nstk - 1].child_ns += elapsed;
}

static int cmp_excl(const void *a, const void *b) {
    const ProfRec *x = *(const ProfRec *const *)a;
    const ProfRec *y = *(const ProfRec *const *)b;
    if (x->excl_ns < y->excl_ns) return 1;
    if (x->excl_ns > y->excl_ns) return -1;
    return strcmp(x->name, y->name);
}

void yac_prof_dump(void) {
    int i, k;
    FILE *f;
    uint64_t tot_excl = 0;
    ProfRec **arr;
    if (!enabled || !out_path) return;
    while (nstk > 0) yac_prof_leave();
    arr = (ProfRec **)malloc((size_t)nrec * sizeof(ProfRec *));
    if (!arr) return;
    k = 0;
    for (i = 0; i < NBUCKET; i++) {
        ProfRec *p;
        for (p = buckets[i]; p; p = p->next) arr[k++] = p;
    }
    qsort(arr, (size_t)k, sizeof(ProfRec *), cmp_excl);
    f = fopen(out_path, "w");
    if (!f) {
        fprintf(stderr, "yac: cannot write profile %s\n", out_path);
        free(arr);
        return;
    }
    for (i = 0; i < k; i++) tot_excl += arr[i]->excl_ns;
    fprintf(f, "# yac-prof 1\n");
    fprintf(f, "# file=%s\n", out_path);
    fprintf(f, "# nfuncs=%d total_excl_ns=%llu\n", k, (unsigned long long)tot_excl);
    fprintf(f, "# name\tcalls\tincl_ns\texcl_ns\tincl_ms\texcl_ms\tpct_excl\n");
    for (i = 0; i < k; i++) {
        ProfRec *r = arr[i];
        double pct = tot_excl ? (100.0 * (double)r->excl_ns / (double)tot_excl) : 0;
        fprintf(f, "%s\t%llu\t%llu\t%llu\t%llu\t%llu\t%.2f\n",
                r->name,
                (unsigned long long)r->ncalls,
                (unsigned long long)r->incl_ns,
                (unsigned long long)r->excl_ns,
                (unsigned long long)(r->incl_ns / 1000000ull),
                (unsigned long long)(r->excl_ns / 1000000ull),
                pct);
    }
    fclose(f);
    free(arr);
}
