#ifndef YAC_CONFIG_H
#define YAC_CONFIG_H

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#define YAC_ARENA_BLOCK_SIZE (1u << 20)
#define YAC_GC_THRESHOLD     (1u << 23)
#define YAC_REPL_LINE_MAX    8192
#define YAC_MAX_GLOBALS      256
#define YAC_ERRBUF_SIZE      512

#endif