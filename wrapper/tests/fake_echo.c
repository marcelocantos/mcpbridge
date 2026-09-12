/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

/* fake_echo — a deterministic child for the stdio transport tests.
 *
 * Default: reads bytes from stdin with raw read(2) and writes them
 * back with raw write(2). No libc buffering, no line semantics.
 * EOF on stdin causes a clean exit(0). Any read/write error exits
 * non-zero.
 *
 * `--emit-bytes N`: write N bytes plus a framing newline to stdout
 * first, then enter the echo loop. Used by the over-cap test so the
 * parent only pumps — sending 4 MiB through a pipe whose other end
 * is not yet being read deadlocks both sides. */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_all(const void *buf, size_t n) {
    const char *p = buf;
    size_t left = n;
    while (left > 0) {
        ssize_t w = write(STDOUT_FILENO, p, left);
        if (w <= 0) {
            return -1;
        }
        p    += (size_t)w;
        left -= (size_t)w;
    }
    return 0;
}

static int emit_bytes(size_t n) {
    char buf[4096];
    memset(buf, 'A', sizeof(buf));
    while (n > 0) {
        size_t chunk = n < sizeof(buf) ? n : sizeof(buf);
        if (write_all(buf, chunk) != 0) {
            return -1;
        }
        n -= chunk;
    }
    return write_all("\n", 1);
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--emit-bytes") == 0) {
        char *end = NULL;
        unsigned long n = strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0') {
            return 3;
        }
        if (emit_bytes((size_t)n) != 0) {
            return 2;
        }
    }

    char buf[4096];
    for (;;) {
        ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
        if (r == 0) {
            return 0; /* clean EOF */
        }
        if (r < 0) {
            return 1;
        }
        if (write_all(buf, (size_t)r) != 0) {
            return 2;
        }
    }
}
