/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int str_eq(const char *a, const char *b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    return strcmp(a, b) == 0;
}

int str_has_prefix(const char *s, const char *prefix) {
    if (s == NULL || prefix == NULL) {
        return 0;
    }
    size_t pn = strlen(prefix);
    return strncmp(s, prefix, pn) == 0;
}

static void die_oom(size_t n) {
    fprintf(stderr, "mcpbridge: out of memory (allocating %zu bytes)\n", n);
    abort();
}

void *xmalloc(size_t n) {
    if (n == 0) {
        n = 1;
    }
    void *p = malloc(n);
    if (p == NULL) {
        die_oom(n);
    }
    return p;
}

void *xcalloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) {
        nmemb = 1;
        size = 1;
    }
    void *p = calloc(nmemb, size);
    if (p == NULL) {
        die_oom(nmemb * size);
    }
    return p;
}

void *xrealloc(void *p, size_t n) {
    if (n == 0) {
        n = 1;
    }
    void *q = realloc(p, n);
    if (q == NULL) {
        die_oom(n);
    }
    return q;
}

char *xstrdup(const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}
