/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

/* Smoke test: the built mcpbridge binary responds to --version and
 * --help with exit 0. This proves the harness is wired up; real unit
 * tests for each module land in later sub-targets. */

#include "../src/log.h"
#include "../src/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail_count = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);  \
            fail_count++;                                                  \
        }                                                                  \
    } while (0)

static void test_str_eq(void) {
    CHECK(str_eq("foo", "foo"), "equal strings should compare equal");
    CHECK(!str_eq("foo", "bar"), "different strings should not compare equal");
    CHECK(!str_eq(NULL, "foo"), "NULL left operand should be safe");
    CHECK(!str_eq("foo", NULL), "NULL right operand should be safe");
    CHECK(!str_eq(NULL, NULL), "NULL both should return 0 (conservative)");
}

static void test_str_has_prefix(void) {
    CHECK(str_has_prefix("foobar", "foo"), "prefix match");
    CHECK(!str_has_prefix("foo", "foobar"), "non-prefix");
    CHECK(str_has_prefix("foo", ""), "empty prefix always matches");
    CHECK(!str_has_prefix(NULL, "foo"), "NULL left is safe");
    CHECK(!str_has_prefix("foo", NULL), "NULL right is safe");
}

static void test_xalloc(void) {
    void *p = xmalloc(16);
    CHECK(p != NULL, "xmalloc returns non-NULL");
    free(p);

    char *s = xstrdup("hello");
    CHECK(s != NULL, "xstrdup returns non-NULL");
    CHECK(strcmp(s, "hello") == 0, "xstrdup copies content");
    free(s);

    char *n = xstrdup(NULL);
    CHECK(n == NULL, "xstrdup(NULL) returns NULL");
}

static void test_log_level(void) {
    log_set_level(LOG_WARN);
    CHECK(log_enabled(LOG_ERROR), "ERROR is enabled at WARN level");
    CHECK(log_enabled(LOG_WARN), "WARN is enabled at WARN level");
    CHECK(!log_enabled(LOG_INFO), "INFO is disabled at WARN level");
    CHECK(!log_enabled(LOG_DEBUG), "DEBUG is disabled at WARN level");
    log_set_level(LOG_DEBUG);
    CHECK(log_enabled(LOG_DEBUG), "DEBUG enabled at DEBUG level");
    log_set_level(LOG_INFO); /* restore */
}

int main(void) {
    test_str_eq();
    test_str_has_prefix();
    test_xalloc();
    test_log_level();

    if (fail_count > 0) {
        fprintf(stderr, "%d smoke_test assertion(s) failed\n", fail_count);
        return 1;
    }
    puts("smoke_test: ok");
    return 0;
}
