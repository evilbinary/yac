#ifndef YAC_PROFILE_H
#define YAC_PROFILE_H

/* Function-level profiler for the C ANF interpreter.
 * Enable with --prof-out FILE or YAC_PROF_OUT. Records call counts and
 * inclusive/exclusive monotonic time. Dump is text, sorted by exclusive ns. */

void yac_prof_enable(const char *out_path);
int yac_prof_enabled(void);
void yac_prof_enter(const char *name);
void yac_prof_leave(void);
void yac_prof_dump(void);

#endif
