#ifndef YAC_ARENA_H
#define YAC_ARENA_H

#include <stddef.h>

/* Bump allocator: everything allocated from an arena is freed together.
 * Used for all interpreter allocations during M1 (no GC yet). */

typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t cap;
    size_t used;
    _Alignas(16) unsigned char data[];
} ArenaBlock;

typedef struct Arena {
    ArenaBlock *head;
    size_t block_size;
} Arena;

void arena_init(Arena *a, size_t block_size);
void *arena_alloc(Arena *a, size_t size);
char *arena_strdup(Arena *a, const char *s);
void arena_free_all(Arena *a);

#endif
