/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#ifndef MCPBRIDGE_UTIL_H
#define MCPBRIDGE_UTIL_H

#include <stddef.h>

/* Program version string, set at build time via -DMCPBRIDGE_VERSION=... */
#ifndef MCPBRIDGE_VERSION
#define MCPBRIDGE_VERSION "0.0.0-dev"
#endif

/* array_len expands to the element count of a C array (compile-time). */
#define array_len(a) (sizeof(a) / sizeof((a)[0]))

/* String equality helper. Safe on NULL operands (returns 0). */
int str_eq(const char *a, const char *b);

/* Prefix test. Safe on NULL operands (returns 0). */
int str_has_prefix(const char *s, const char *prefix);

/* xmalloc/xcalloc/xrealloc/xstrdup: allocate or abort with a clear error.
 * We use these for long-lived structures where recovery from OOM is
 * neither possible nor worth the complexity. */
void *xmalloc(size_t n);
void *xcalloc(size_t nmemb, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

#endif /* MCPBRIDGE_UTIL_H */
