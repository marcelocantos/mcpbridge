/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#include "daemon_client.h"

#include "log.h"
#include "mcp.h"
#include "util.h"

#include "cJSON.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct daemon_client {
    int               fd;
    struct mcp_reader reader; /* accumulates incoming bytes */
    uint64_t          next_seq;
};

/* ---------- helpers ---------- */

static char *dup_or_null(const char *s) {
    return (s == NULL) ? NULL : xstrdup(s);
}

static int write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w > 0) {
            p += (size_t)w;
            n -= (size_t)w;
            continue;
        }
        if (w == -1 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

static int send_envelope(struct daemon_client *dc, cJSON *env) {
    char *data = cJSON_PrintUnformatted(env);
    if (data == NULL) {
        return -1;
    }
    size_t n = strlen(data);
    int rc = 0;
    if (write_all(dc->fd, data, n) != 0) rc = -1;
    if (rc == 0 && write_all(dc->fd, "\n", 1) != 0) rc = -1;
    cJSON_free(data);
    return rc;
}

/* Blocking read of exactly one complete line. Uses poll with a
 * 5-second deadline to avoid a hung daemon wedging the handshake. */
static int read_line_blocking(struct daemon_client *dc,
                              const char **line, size_t *len,
                              int timeout_ms) {
    for (;;) {
        int rc = mcp_reader_pop(&dc->reader, line, len);
        if (rc == MCP_READER_OK) {
            return 0;
        }
        if (rc == MCP_READER_TOO_LONG) {
            log_warn("daemon_client: dropped oversized envelope line");
            continue;
        }

        struct pollfd pfd = { .fd = dc->fd, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        char buf[4096];
        ssize_t r = read(dc->fd, buf, sizeof(buf));
        if (r == 0) {
            errno = ECONNRESET;
            return -1;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        mcp_reader_feed(&dc->reader, buf, (size_t)r);
    }
}

static cJSON *parse_line(const char *line, size_t len) {
    return cJSON_ParseWithLength(line, len);
}

static const char *json_str(const cJSON *obj, const char *key) {
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (f != NULL && cJSON_IsString(f) && f->valuestring != NULL) {
        return f->valuestring;
    }
    return NULL;
}

static uint64_t json_u64(const cJSON *obj, const char *key) {
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (f != NULL && cJSON_IsNumber(f)) {
        if (f->valuedouble < 0) return 0;
        return (uint64_t)f->valuedouble;
    }
    return 0;
}

static int json_bool(const cJSON *obj, const char *key) {
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (f != NULL) && cJSON_IsTrue(f);
}

/* ---------- public ---------- */

struct daemon_client *daemon_client_new(const char *sock_path) {
    if (sock_path == NULL || sock_path[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    size_t plen = strlen(sock_path);
    if (plen >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    memcpy(addr.sun_path, sock_path, plen + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        return NULL;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        int saved = errno;
        close(fd);
        errno = saved;
        return NULL;
    }

    struct daemon_client *dc = xcalloc(1, sizeof(*dc));
    dc->fd       = fd;
    dc->next_seq = 1;
    mcp_reader_init(&dc->reader, 256 * 1024);
    return dc;
}

void daemon_client_free(struct daemon_client *dc) {
    if (dc == NULL) {
        return;
    }
    if (dc->fd >= 0) {
        close(dc->fd);
    }
    mcp_reader_free(&dc->reader);
    free(dc);
}

int daemon_client_fd(const struct daemon_client *dc) {
    return (dc == NULL) ? -1 : dc->fd;
}

int daemon_client_do_handshake(struct daemon_client *dc,
                               const char *wrapper_version,
                               pid_t       wrapper_pid,
                               const char *name,
                               pid_t       child_pid,
                               const char *child_binary) {
    if (dc == NULL || wrapper_version == NULL || name == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* hello */
    cJSON *hello = cJSON_CreateObject();
    cJSON_AddStringToObject(hello, "t", "hello");
    cJSON_AddNumberToObject(hello, "v", DAEMON_CLIENT_PROTOCOL_VERSION);
    cJSON_AddNumberToObject(hello, "seq", (double)(dc->next_seq++));
    cJSON_AddStringToObject(hello, "wrapper_version", wrapper_version);
    cJSON_AddNumberToObject(hello, "pid", (double)wrapper_pid);
    int rc = send_envelope(dc, hello);
    cJSON_Delete(hello);
    if (rc != 0) {
        return -1;
    }

    /* hello_ok */
    const char *line = NULL;
    size_t      llen = 0;
    if (read_line_blocking(dc, &line, &llen, 5000) != 0) {
        return -1;
    }
    cJSON *hello_ok = parse_line(line, llen);
    if (hello_ok == NULL) {
        errno = EPROTO;
        return -1;
    }
    const char *ht = json_str(hello_ok, "t");
    if (ht == NULL || strcmp(ht, "hello_ok") != 0) {
        log_warn("daemon_client: expected hello_ok, got %s",
                 ht != NULL ? ht : "(none)");
        cJSON_Delete(hello_ok);
        errno = EPROTO;
        return -1;
    }
    cJSON_Delete(hello_ok);

    /* register */
    cJSON *reg = cJSON_CreateObject();
    cJSON_AddStringToObject(reg, "t", "register");
    cJSON_AddNumberToObject(reg, "v", DAEMON_CLIENT_PROTOCOL_VERSION);
    cJSON_AddNumberToObject(reg, "seq", (double)(dc->next_seq++));
    cJSON_AddStringToObject(reg, "name", name);
    cJSON_AddNumberToObject(reg, "child_pid", (double)child_pid);
    if (child_binary != NULL) {
        cJSON_AddStringToObject(reg, "child_binary", child_binary);
    }
    rc = send_envelope(dc, reg);
    cJSON_Delete(reg);
    if (rc != 0) {
        return -1;
    }

    /* register_ok */
    if (read_line_blocking(dc, &line, &llen, 5000) != 0) {
        return -1;
    }
    cJSON *register_ok = parse_line(line, llen);
    if (register_ok == NULL) {
        errno = EPROTO;
        return -1;
    }
    const char *rt = json_str(register_ok, "t");
    if (rt == NULL || strcmp(rt, "register_ok") != 0) {
        log_warn("daemon_client: expected register_ok, got %s",
                 rt != NULL ? rt : "(none)");
        cJSON_Delete(register_ok);
        errno = EPROTO;
        return -1;
    }
    cJSON_Delete(register_ok);
    return 0;
}

int daemon_client_try_read(struct daemon_client *dc, struct daemon_event *out) {
    if (dc == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));

    /* First try to pop a complete line from what's already buffered. */
    const char *line = NULL;
    size_t      llen = 0;
    int rc = mcp_reader_pop(&dc->reader, &line, &llen);
    if (rc == MCP_READER_EMPTY) {
        /* Try one non-blocking read in case there are bytes we
         * haven't absorbed yet. */
        char buf[4096];
        ssize_t r;
        do {
            r = read(dc->fd, buf, sizeof(buf));
        } while (r == -1 && errno == EINTR);
        if (r == 0) {
            out->kind = DAEMON_EV_CLOSED;
            return 1;
        }
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                out->kind = DAEMON_EV_NONE;
                return 0;
            }
            return -1;
        }
        mcp_reader_feed(&dc->reader, buf, (size_t)r);
        rc = mcp_reader_pop(&dc->reader, &line, &llen);
        if (rc == MCP_READER_EMPTY) {
            out->kind = DAEMON_EV_NONE;
            return 0;
        }
    }
    if (rc == MCP_READER_TOO_LONG) {
        log_warn("daemon_client: dropping oversized envelope");
        out->kind = DAEMON_EV_NONE;
        return 0;
    }

    /* rc == MCP_READER_OK */
    cJSON *env = parse_line(line, llen);
    if (env == NULL) {
        log_warn("daemon_client: bad envelope JSON");
        out->kind = DAEMON_EV_NONE;
        return 0;
    }
    const char *t = json_str(env, "t");
    if (t == NULL) {
        cJSON_Delete(env);
        out->kind = DAEMON_EV_NONE;
        return 0;
    }

    out->seq     = json_u64(env, "seq");
    out->ack_seq = json_u64(env, "ack_seq");

    if (strcmp(t, "hello_ok") == 0) {
        out->kind = DAEMON_EV_HELLO_OK;
    } else if (strcmp(t, "register_ok") == 0) {
        out->kind         = DAEMON_EV_REGISTER_OK;
        out->config_found = json_bool(env, "config_found");
        out->polling      = json_bool(env, "polling");
    } else if (strcmp(t, "reload") == 0) {
        out->kind   = DAEMON_EV_RELOAD;
        out->name   = dup_or_null(json_str(env, "name"));
        out->reason = dup_or_null(json_str(env, "reason"));
    } else if (strcmp(t, "shutdown") == 0) {
        out->kind   = DAEMON_EV_SHUTDOWN;
        out->reason = dup_or_null(json_str(env, "reason"));
    } else if (strcmp(t, "error") == 0) {
        out->kind   = DAEMON_EV_ERROR;
        out->code   = dup_or_null(json_str(env, "code"));
        out->detail = dup_or_null(json_str(env, "detail"));
    } else {
        log_debug("daemon_client: ignoring unknown envelope type %s", t);
        out->kind = DAEMON_EV_NONE;
    }

    cJSON_Delete(env);
    return 1;
}

void daemon_client_send_deregister(struct daemon_client *dc, const char *reason) {
    if (dc == NULL) {
        return;
    }
    cJSON *env = cJSON_CreateObject();
    cJSON_AddStringToObject(env, "t", "deregister");
    cJSON_AddNumberToObject(env, "v", DAEMON_CLIENT_PROTOCOL_VERSION);
    cJSON_AddNumberToObject(env, "seq", (double)(dc->next_seq++));
    if (reason != NULL) {
        cJSON_AddStringToObject(env, "reason", reason);
    }
    (void)send_envelope(dc, env);
    cJSON_Delete(env);
}

int daemon_client_send_reload_ack(struct daemon_client *dc,
                                  uint64_t ack_seq,
                                  const char *status,
                                  const char *detail) {
    if (dc == NULL || status == NULL) {
        errno = EINVAL;
        return -1;
    }
    cJSON *env = cJSON_CreateObject();
    cJSON_AddStringToObject(env, "t", "reload_ack");
    cJSON_AddNumberToObject(env, "v", DAEMON_CLIENT_PROTOCOL_VERSION);
    cJSON_AddNumberToObject(env, "seq", (double)(dc->next_seq++));
    cJSON_AddNumberToObject(env, "ack_seq", (double)ack_seq);
    cJSON_AddStringToObject(env, "status", status);
    if (detail != NULL) {
        cJSON_AddStringToObject(env, "detail", detail);
    }
    int rc = send_envelope(dc, env);
    cJSON_Delete(env);
    return rc;
}

void daemon_event_free(struct daemon_event *ev) {
    if (ev == NULL) {
        return;
    }
    free(ev->reason);
    free(ev->name);
    free(ev->code);
    free(ev->detail);
    memset(ev, 0, sizeof(*ev));
}
