/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

/* fake_echo — a deterministic child for the stdio transport tests.
 *
 * Reads bytes from stdin with raw read(2) and writes them back with
 * raw write(2). No libc buffering, no line semantics, no fancy
 * shutdown — EOF on stdin causes a clean exit(0). Any read/write
 * error exits non-zero.
 *
 * Intentionally minimal: the test asserts that the wrapper's stdio
 * transport can round-trip bytes through a real child process, not
 * that it handles complex child behaviour. */

#include <stdlib.h>
#include <unistd.h>

int main(void) {
    char buf[4096];
    for (;;) {
        ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
        if (r == 0) {
            return 0; /* clean EOF */
        }
        if (r < 0) {
            return 1;
        }
        ssize_t written = 0;
        while (written < r) {
            ssize_t w = write(STDOUT_FILENO, buf + written, (size_t)(r - written));
            if (w <= 0) {
                return 2;
            }
            written += w;
        }
    }
}
