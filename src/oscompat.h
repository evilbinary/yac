#ifndef YAC_OSCOMPAT_H
#define YAC_OSCOMPAT_H

#include <stddef.h>
#include <time.h>

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

int yac_clock_gettime(int clk, struct timespec *ts);
struct tm *yac_localtime_r(const time_t *t, struct tm *out);

/* Same shell as C system(3). Returns the process exit code, or -1 if invoke failed. */
int yac_system(const char *cmd);

/* Run cmd via that same shell; feed stdin; capture stdout/stderr (malloc, caller frees).
 * Returns 0 on success. On failure *errmsg is a static string and out/err are NULL. */
int yac_popen_capture(const char *cmd, const char *in, size_t in_len,
                      int *rc, char **out, size_t *out_n, char **err, size_t *err_n,
                      const char **errmsg);

#endif
