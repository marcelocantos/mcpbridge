/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#ifndef MCPBRIDGE_TRANSPORT_STDIO_H
#define MCPBRIDGE_TRANSPORT_STDIO_H

/* Stdio transport: fork a child MCP server and speak newline-
 * delimited JSON-RPC over stdin/stdout.
 *
 * The child's stderr is inherited — its error output lands in the
 * wrapper's own stderr, visible to whoever launched the wrapper.
 * We deliberately do not capture it in v1: filtering/prefixing child
 * errors is a nice-to-have that adds a pipe and a reader we don't
 * otherwise need. */

#include "transport.h"

#include <sys/types.h>

/* Create a stdio transport that will, on start, fork+exec `cmd`
 * with the given NULL-terminated argv. Both pointers must remain
 * valid for the lifetime of the transport (we store them, we do
 * not copy).
 *
 * Returns a heap-allocated transport on success, NULL on allocation
 * failure. Must be started with transport_start() before any I/O.
 * Free with transport_destroy(). */
struct transport *transport_stdio_new(const char *cmd, char *const argv[]);

/* Child pid, or -1 if not started. Used by the event loop's SIGCHLD
 * handler to know which pid to waitpid() on. */
pid_t transport_stdio_pid(const struct transport *t);

/* Non-blocking reap. Called by the event loop after SIGCHLD fires.
 *
 * Returns:
 *   1  — the child has exited; *status (if non-NULL) holds the raw
 *        status from waitpid() (use WIFEXITED / WEXITSTATUS etc.).
 *   0  — the child is still running.
 *  -1  — waitpid() itself failed; errno is set.
 *
 * After a successful reap the transport's internal pid becomes -1
 * and further calls return 0 (still-not-running semantics). */
int transport_stdio_reap(struct transport *t, int *status);

#endif /* MCPBRIDGE_TRANSPORT_STDIO_H */
