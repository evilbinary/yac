#include "arena.h"

#include <stdlib.h>
#include <string.h>

void arena_init(Arena *a, size_t block_size) {
    a->head = NULL;
    a->block_size = block_size > 0 ? block_size : 64 * 1024;
}

static ArenaBlock *new_block(Arena *a, size_t min_cap) {
    size_t cap = a->block_size;
    if (cap < min_cap) cap = min_cap;
    ArenaBlock *b = (ArenaBlock *)malloc(sizeof(ArenaBlock) + cap);
    if (!b) {
        /* allocation failure: not much we can do in an interpreter */
        exit(1);
    }
    b->next = a->head;
    b->cap = cap;
    b->used = 0;
    a->head = b;
    return b;
}

void *arena_alloc(Arena *a, size_t size) {
    size = (size + 15) & ~(size_t)15;
    ArenaBlock *b = a->head;
    if (!b || b->used + size > b->cap) b = new_block(a, size);
    void *p = b->data + b->used;
    b->used += size;
    return p;
}

char *arena_strdup(Arena *a, const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)arena_alloc(a, n);
    memcpy(p, s, n);
    return p;
}

void arena_free_all(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
}
