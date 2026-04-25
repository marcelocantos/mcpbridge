/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#include "config.h"

#include "util.h"

#include <cJSON.h>

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ---------- File slurp ---------- */

/* Read the entire contents of `path` into a freshly allocated null-
 * terminated buffer. Returns NULL on error after writing a message to
 * stderr. */
static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "mcpbridge: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "mcpbridge: %s: seek: %s\n", path, strerror(errno));
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        fprintf(stderr, "mcpbridge: %s: tell: %s\n", path, strerror(errno));
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = xmalloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        fprintf(stderr, "mcpbridge: %s: short read\n", path);
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

/* ---------- Tilde expansion ---------- */

char *expand_tilde(const char *p) {
    if (p == NULL) {
        return NULL;
    }
    if (p[0] != '~') {
        return xstrdup(p);
    }
    /* Locate the segment terminator. */
    const char *slash = strchr(p, '/');
    size_t headlen = slash != NULL ? (size_t)(slash - p) : strlen(p);

    const char *home = NULL;
    if (headlen == 1) {
        /* Bare `~`. */
        home = getenv("HOME");
        if (home == NULL || home[0] == '\0') {
            struct passwd *pw = getpwuid(getuid());
            if (pw == NULL) {
                fprintf(stderr,
                        "mcpbridge: cannot resolve `~` (no $HOME, no passwd entry)\n");
                return NULL;
            }
            home = pw->pw_dir;
        }
    } else {
        /* `~user`. */
        char *uname = xmalloc(headlen);
        memcpy(uname, p + 1, headlen - 1);
        uname[headlen - 1] = '\0';
        struct passwd *pw = getpwnam(uname);
        if (pw == NULL) {
            fprintf(stderr, "mcpbridge: ~%s: no such user\n", uname);
            free(uname);
            return NULL;
        }
        home = pw->pw_dir;
        free(uname);
    }

    const char *tail = slash != NULL ? slash : "";
    size_t out_len = strlen(home) + strlen(tail) + 1;
    char *out = xmalloc(out_len);
    snprintf(out, out_len, "%s%s", home, tail);
    return out;
}

/* ---------- URL validation ---------- */

/* Reject any url that isn't plain http:// to a loopback host. Returns
 * 1 if valid, 0 otherwise. On failure, writes a clear error to
 * stderr. */
static int validate_url(const char *url, const char *path) {
    /* Scheme. */
    static const char http[] = "http://";
    if (strncmp(url, http, sizeof(http) - 1) != 0) {
        fprintf(stderr,
                "mcpbridge: %s: url scheme must be http (got %s); https and remote hosts are out of scope for v1\n",
                path, url);
        return 0;
    }
    const char *host_start = url + sizeof(http) - 1;
    /* Host runs until ':' (port), '/' (path), or end. */
    const char *host_end = host_start;
    while (*host_end != '\0' && *host_end != ':' && *host_end != '/') {
        host_end++;
    }
    size_t host_len = (size_t)(host_end - host_start);
    if (host_len == 0) {
        fprintf(stderr, "mcpbridge: %s: url missing host\n", path);
        return 0;
    }
    int ok = 0;
    if (host_len == strlen("localhost") &&
        memcmp(host_start, "localhost", host_len) == 0) {
        ok = 1;
    } else if (host_len == strlen("127.0.0.1") &&
               memcmp(host_start, "127.0.0.1", host_len) == 0) {
        ok = 1;
    } else if (host_len == strlen("::1") &&
               memcmp(host_start, "::1", host_len) == 0) {
        ok = 1;
    }
    if (!ok) {
        char host[64];
        size_t copy = host_len < sizeof(host) - 1 ? host_len : sizeof(host) - 1;
        memcpy(host, host_start, copy);
        host[copy] = '\0';
        fprintf(stderr,
                "mcpbridge: %s: url host must be loopback (localhost / 127.0.0.1 / ::1); got %s\n",
                path, host);
        return 0;
    }
    return 1;
}

/* ---------- Loader ---------- */

static const char migration_hint_v1[] =
    "Schema v1 is no longer supported. Migrate by:\n"
    "  - Setting \"schema\": 2\n"
    "  - Adding \"command\": \"/path/to/binary\" and optional \"args\": [...]\n"
    "    for stdio servers, or \"url\": \"http://localhost:PORT/path\" for HTTP.\n"
    "See docs/config-schema.md.";

void config_free(struct config *c) {
    if (c == NULL) {
        return;
    }
    free(c->name);
    free(c->command);
    if (c->args != NULL) {
        for (int i = 0; c->args[i] != NULL; i++) {
            free(c->args[i]);
        }
        free(c->args);
    }
    free(c->url);
    free(c->source_path);
    free(c);
}

/* basename helper that doesn't mutate its argument and returns a
 * freshly-allocated string. */
static char *basename_dup(const char *path) {
    const char *slash = strrchr(path, '/');
    return xstrdup(slash != NULL ? slash + 1 : path);
}

struct config *config_load(const char *path) {
    char *body = slurp(path);
    if (body == NULL) {
        return NULL;
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        const char *err = cJSON_GetErrorPtr();
        fprintf(stderr, "mcpbridge: %s: invalid JSON%s%s\n",
                path,
                err != NULL ? ": near " : "",
                err != NULL ? err : "");
        free(body);
        return NULL;
    }
    free(body);

    struct config *c = xcalloc(1, sizeof(*c));
    c->source_path = xstrdup(path);

    cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (!cJSON_IsNumber(schema)) {
        fprintf(stderr, "mcpbridge: %s: missing or non-numeric \"schema\"\n", path);
        goto fail;
    }
    int schema_n = schema->valueint;
    if (schema_n == 1) {
        fprintf(stderr, "mcpbridge: %s: schema v1 is not supported.\n%s\n",
                path, migration_hint_v1);
        goto fail;
    }
    if (schema_n != 2) {
        fprintf(stderr, "mcpbridge: %s: unsupported schema version %d (this build speaks 2)\n",
                path, schema_n);
        goto fail;
    }

    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(name) || name->valuestring[0] == '\0') {
        fprintf(stderr, "mcpbridge: %s: missing or empty \"name\"\n", path);
        goto fail;
    }
    c->name = xstrdup(name->valuestring);

    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "command");
    cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
    int has_cmd = cJSON_IsString(cmd) && cmd->valuestring[0] != '\0';
    int has_url = cJSON_IsString(url) && url->valuestring[0] != '\0';

    if (has_cmd && has_url) {
        fprintf(stderr,
                "mcpbridge: %s: \"command\" and \"url\" are mutually exclusive\n",
                path);
        goto fail;
    }
    if (!has_cmd && !has_url) {
        fprintf(stderr,
                "mcpbridge: %s: exactly one of \"command\" or \"url\" is required\n",
                path);
        goto fail;
    }

    if (has_cmd) {
        c->backend = CONFIG_BACKEND_STDIO;
        c->command = expand_tilde(cmd->valuestring);
        if (c->command == NULL) {
            goto fail;
        }

        /* Build args[]: args[0] is basename of command, then any
         * entries from the JSON args array (which must be strings).
         * Terminated by NULL for execvp convention. */
        cJSON *args_arr = cJSON_GetObjectItemCaseSensitive(root, "args");
        int nargs = 0;
        if (cJSON_IsArray(args_arr)) {
            nargs = cJSON_GetArraySize(args_arr);
        } else if (args_arr != NULL && !cJSON_IsNull(args_arr)) {
            fprintf(stderr, "mcpbridge: %s: \"args\" must be an array of strings\n",
                    path);
            goto fail;
        }
        c->args = xcalloc((size_t)nargs + 2, sizeof(char *));
        c->args[0] = basename_dup(c->command);
        for (int i = 0; i < nargs; i++) {
            cJSON *item = cJSON_GetArrayItem(args_arr, i);
            if (!cJSON_IsString(item)) {
                fprintf(stderr, "mcpbridge: %s: \"args\"[%d] is not a string\n",
                        path, i);
                goto fail;
            }
            c->args[1 + i] = xstrdup(item->valuestring);
        }
        c->args[1 + nargs] = NULL;
        c->args_count = 1 + nargs;
    } else {
        c->backend = CONFIG_BACKEND_HTTP;
        if (!validate_url(url->valuestring, path)) {
            goto fail;
        }
        c->url = xstrdup(url->valuestring);
    }

    cJSON_Delete(root);
    return c;

fail:
    cJSON_Delete(root);
    config_free(c);
    return NULL;
}
