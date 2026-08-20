#ifndef YAC_SCHEME_H
#define YAC_SCHEME_H

/* Translate a Scheme subset source into yac source text. Returns a
 * malloc'd NUL-terminated string on success; on failure returns NULL and
 * sets *errmsg (also malloc'd). The returned text must be freed. */
char *scheme_to_yac(const char *src, char **errmsg);

#endif
