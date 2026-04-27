/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#ifndef MCPBRIDGE_CONFIG_H
#define MCPBRIDGE_CONFIG_H

/* Per-server config loader (wrapper side).
 *
 * Reads the schema-v2 JSON file pointed at by `mcpbridge connect
 * <path>` and exposes the connection metadata the wrapper needs:
 * name, plus either a stdio command+args or an http url.
 *
 * The daemon-only fields (source/upgrade/check_interval) are
 * ignored here — the daemon validates them on its side, and the
 * wrapper has no business duplicating that work.
 *
 * Authoritative schema reference: docs/config-schema.md. */

enum config_backend {
    CONFIG_BACKEND_NONE  = 0,
    CONFIG_BACKEND_STDIO = 1,
    CONFIG_BACKEND_HTTP  = 2,
};

struct config {
    enum config_backend backend;

    /* Identifier sent to the daemon on registration. Owned by the
     * config; freed by config_free. */
    char *name;

    /* Stdio backend. command is tilde-expanded; args is a NULL-
     * terminated argv-style array (args[0] == basename of command,
     * for execvp convention). All owned by the config. */
    char  *command;     /* /full/expanded/path */
    char **args;        /* NULL-terminated, args[0]=basename(command) */
    int    args_count;  /* convenience: number of non-NULL entries */

    /* Http backend. Owned by the config. */
    char *url;

    /* Per-tool-call timeout (milliseconds). Bounds how long any
     * single tool call may wait for the upstream — including any
     * connect-retry-with-backoff loops triggered by a transient
     * outage. Default 300000 (5 minutes). 0 means "no per-call
     * timeout" (retries until the agent kills the wrapper). The
     * timeout is only consulted while there is an in-flight call
     * — an idle wrapper does no upstream I/O and therefore needs no
     * deadline. See 🎯T7.1. */
    int tool_call_timeout_ms;

    /* Source path the config came from (for diagnostics). Owned. */
    char *source_path;
};

/* Default for tool_call_timeout_ms when the config omits it. */
#define CONFIG_DEFAULT_TOOL_CALL_TIMEOUT_MS 300000

/* config_load reads `path`, parses it as a v2 config, validates the
 * connection shape, and returns a freshly-allocated struct config*.
 * On error returns NULL after writing a one-line message to stderr.
 * The caller frees with config_free. */
struct config *config_load(const char *path);

/* config_free releases a struct config* returned from config_load.
 * Safe on NULL. */
void config_free(struct config *c);

/* expand_tilde resolves a leading `~` or `~user` segment to an
 * absolute path. Returns a freshly-allocated string the caller must
 * free, or NULL on lookup failure. Paths with no leading `~` are
 * duplicated unchanged. */
char *expand_tilde(const char *p);

#endif /* MCPBRIDGE_CONFIG_H */
