/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#include "mcp.h"

#include "util.h"

#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---------- Id ---------- */

void mcp_id_free(struct mcp_id *id) {
    if (id == NULL) {
        return;
    }
    if (id->tag == MCP_ID_STRING) {
        free(id->v.s);
    }
    id->tag = MCP_ID_NONE;
    id->v.i = 0;
}

int mcp_id_eq(const struct mcp_id *a, const struct mcp_id *b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    if (a->tag != b->tag) {
        return 0;
    }
    switch (a->tag) {
    case MCP_ID_NONE:
        return 1;
    case MCP_ID_INT:
        return a->v.i == b->v.i;
    case MCP_ID_STRING:
        return strcmp(a->v.s, b->v.s) == 0;
    }
    return 0;
}

/* Extract the id field from a JSON object into our tagged union.
 *
 * Returns 0 on success (even if id is absent — tag becomes NONE),
 * MCP_PARSE_ERR_BAD_ID if the field exists but is neither int nor
 * string (we deliberately reject floats and null). */
static int extract_id(const cJSON *obj, struct mcp_id *out) {
    out->tag = MCP_ID_NONE;
    out->v.i = 0;

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(obj, "id");
    if (id == NULL) {
        return 0;
    }
    if (cJSON_IsString(id) && id->valuestring != NULL) {
        out->tag = MCP_ID_STRING;
        out->v.s = xstrdup(id->valuestring);
        return 0;
    }
    if (cJSON_IsNumber(id)) {
        double d = id->valuedouble;
        if (d != (double)(int64_t)d) {
            return MCP_PARSE_ERR_BAD_ID;
        }
        out->tag = MCP_ID_INT;
        out->v.i = (int64_t)d;
        return 0;
    }
    return MCP_PARSE_ERR_BAD_ID;
}

/* ---------- Message ---------- */

int mcp_msg_parse(const char *bytes, size_t len, struct mcp_msg *out) {
    if (out == NULL) {
        return MCP_PARSE_ERR_JSON;
    }
    memset(out, 0, sizeof(*out));

    if (bytes == NULL || len == 0) {
        return MCP_PARSE_ERR_JSON;
    }

    cJSON *root = cJSON_ParseWithLength(bytes, len);
    if (root == NULL) {
        return MCP_PARSE_ERR_JSON;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return MCP_PARSE_ERR_NOT_OBJECT;
    }

    int rc = extract_id(root, &out->id);
    if (rc != 0) {
        cJSON_Delete(root);
        return rc;
    }

    const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    if (method != NULL) {
        if (!cJSON_IsString(method) || method->valuestring == NULL) {
            mcp_id_free(&out->id);
            cJSON_Delete(root);
            return MCP_PARSE_ERR_BAD_METHOD;
        }
        out->method = xstrdup(method->valuestring);
    }

    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    const cJSON *error  = cJSON_GetObjectItemCaseSensitive(root, "error");

    /* Classify. JSON-RPC 2.0 shapes:
     *   request:      method + id
     *   notification: method, no id
     *   response:     id + (result or error), no method
     * Anything else is incoherent. */
    if (out->method != NULL && out->id.tag != MCP_ID_NONE) {
        out->kind = MCP_KIND_REQUEST;
    } else if (out->method != NULL && out->id.tag == MCP_ID_NONE) {
        out->kind = MCP_KIND_NOTIFICATION;
    } else if (out->method == NULL && out->id.tag != MCP_ID_NONE &&
               (result != NULL || error != NULL)) {
        out->kind = MCP_KIND_RESPONSE;
        out->is_error = (error != NULL);
    } else {
        mcp_msg_free(out);
        cJSON_Delete(root);
        return MCP_PARSE_ERR_INCOHERENT;
    }

    /* Retain the raw bytes for verbatim forwarding. */
    out->raw = xmalloc(len);
    memcpy(out->raw, bytes, len);
    out->raw_len = len;

    cJSON_Delete(root);
    return MCP_PARSE_OK;
}

void mcp_msg_free(struct mcp_msg *m) {
    if (m == NULL) {
        return;
    }
    mcp_id_free(&m->id);
    free(m->method);
    free(m->raw);
    memset(m, 0, sizeof(*m));
}

int mcp_msg_is_request(const struct mcp_msg *m, const char *method) {
    if (m == NULL || method == NULL) {
        return 0;
    }
    return m->kind == MCP_KIND_REQUEST &&
           m->method != NULL &&
           strcmp(m->method, method) == 0;
}

int mcp_msg_is_notification(const struct mcp_msg *m, const char *method) {
    if (m == NULL || method == NULL) {
        return 0;
    }
    return m->kind == MCP_KIND_NOTIFICATION &&
           m->method != NULL &&
           strcmp(m->method, method) == 0;
}

/* ---------- Streaming line reader ----------
 *
 * Invariants:
 *   - Live bytes are buf[head .. len).
 *   - head <= len <= cap <= max (modulo growth).
 *   - `dropping` is set only while we are discarding an oversized
 *     line and have not yet seen its terminating newline.
 *   - `overflow_pending` is set exactly when dropping has just ended
 *     and the caller has not yet observed the resulting TOO_LONG.
 *   - A popped line pointer points into buf[head_at_pop .. ] and
 *     remains valid until the next call that compacts (feed may
 *     compact; pop never does).
 */

#define MCP_READER_INITIAL_CAP 4096u

/* `max` is the maximum line length (excluding the terminating '\n').
 * Internally we need one extra byte of buffer so memchr can find the
 * newline at the tail of a max-length line. */
static inline size_t reader_hard_cap(const struct mcp_reader *r) {
    return r->max + 1;
}

void mcp_reader_init(struct mcp_reader *r, size_t max) {
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof(*r));
    r->max = (max == 0) ? MCP_LINE_MAX_DEFAULT : max;
    size_t hard = r->max + 1;
    r->cap = MCP_READER_INITIAL_CAP;
    if (r->cap > hard) {
        r->cap = hard;
    }
    r->buf = xmalloc(r->cap);
}

void mcp_reader_free(struct mcp_reader *r) {
    if (r == NULL) {
        return;
    }
    free(r->buf);
    memset(r, 0, sizeof(*r));
}

/* Live byte count. */
static inline size_t reader_live(const struct mcp_reader *r) {
    return r->len - r->head;
}

/* Last-newline search. POSIX has no memrchr; hand-roll one. */
static const char *last_newline(const char *p, size_t n) {
    while (n > 0) {
        n--;
        if (p[n] == '\n') {
            return p + n;
        }
    }
    return NULL;
}

/* Compact the buffer: slide live bytes down to offset 0. Invalidates
 * any outstanding popped-line pointers. Only feed() calls this. */
static void reader_compact(struct mcp_reader *r) {
    if (r->head == 0) {
        return;
    }
    size_t live = reader_live(r);
    if (live > 0) {
        memmove(r->buf, r->buf + r->head, live);
    }
    r->head = 0;
    r->len  = live;
}

/* Ensure there is room for `need` more bytes at the tail, growing
 * or compacting as necessary. Returns 1 on success, 0 if the grow
 * would exceed the hard cap (the caller must then enter dropping
 * mode). */
static int reader_reserve(struct mcp_reader *r, size_t need) {
    /* Room at the current tail. */
    if (r->len + need <= r->cap) {
        return 1;
    }
    const size_t hard = reader_hard_cap(r);
    const size_t live = r->len - r->head;
    /* Compacting might give us enough room without growing. */
    if (r->head > 0 && live + need <= r->cap) {
        reader_compact(r);
        return 1;
    }
    /* Would the grown buffer even fit in the hard cap? */
    const size_t required = live + need;
    if (required > hard) {
        return 0;
    }
    /* Grow. Start from current cap (bumped to the lazy initial if
     * still tiny), double until required fits, but never exceed hard. */
    size_t want = r->cap;
    if (want < MCP_READER_INITIAL_CAP) {
        want = MCP_READER_INITIAL_CAP;
    }
    if (want > hard) {
        want = hard;
    }
    while (want < required) {
        /* Loop cannot fail because required <= hard and want rises
         * monotonically toward hard; the check is defensive. */
        if (want >= hard) {
            return 0;
        }
        size_t next = want * 2;
        if (next < want || next > hard) {
            next = hard;
        }
        want = next;
    }
    reader_compact(r);
    if (want != r->cap) {
        r->buf = xrealloc(r->buf, want);
        r->cap = want;
    }
    return 1;
}

void mcp_reader_feed(struct mcp_reader *r, const char *bytes, size_t n) {
    if (r == NULL || bytes == NULL || n == 0) {
        return;
    }

    /* Process one line at a time (or one tail chunk if no newline is
     * present yet). This matters because a single feed can contain
     * many short lines plus one oversized one — we want to commit the
     * short ones successfully instead of giving up on the whole chunk. */
    while (n > 0) {
        if (r->dropping) {
            const void *nlp = memchr(bytes, '\n', n);
            if (nlp == NULL) {
                /* Still dropping; keep waiting for a newline. */
                return;
            }
            size_t consumed = (size_t)((const char *)nlp - bytes) + 1;
            bytes += consumed;
            n     -= consumed;
            r->dropping         = 0;
            r->overflow_pending = 1;
            continue;
        }

        const void *nlp = memchr(bytes, '\n', n);
        const size_t chunk = (nlp != NULL)
            ? ((size_t)((const char *)nlp - bytes) + 1)
            : n;

        if (!reader_reserve(r, chunk)) {
            /* This line is too long. Discard only the in-progress
             * tail from the buffer — any complete lines ahead of it
             * are still valid and poppable. */
            const size_t live = reader_live(r);
            if (live > 0) {
                const char *last = last_newline(r->buf + r->head, live);
                if (last != NULL) {
                    /* Truncate to just-past the last complete line. */
                    r->len = (size_t)(last - r->buf) + 1;
                } else {
                    /* No complete lines buffered — the whole live
                     * range is part of the oversized line. */
                    r->len = r->head;
                }
            }
            r->dropping = 1;
            if (nlp != NULL) {
                /* The terminating newline is in this feed — consume
                 * through it and signal the overflow for the next
                 * pop. */
                bytes += chunk;
                n     -= chunk;
                r->dropping         = 0;
                r->overflow_pending = 1;
            } else {
                /* Tail has no newline yet; stay dropping. */
                return;
            }
            continue;
        }

        memcpy(r->buf + r->len, bytes, chunk);
        r->len += chunk;
        bytes  += chunk;
        n      -= chunk;
    }
}

int mcp_reader_pop(struct mcp_reader *r, const char **line, size_t *len) {
    if (r == NULL || line == NULL || len == NULL) {
        return MCP_READER_EMPTY;
    }

    /* Drain complete lines before signalling pending overflows. This
     * departs slightly from strict wire chronology — a TOO_LONG is
     * reported only after all lines already buffered at the time of
     * the overflow have been popped — but it guarantees the much
     * more important property that no valid line is ever lost or
     * delayed by an unrelated oversized neighbour. */
    size_t live = reader_live(r);
    if (live > 0) {
        const char *start = r->buf + r->head;
        const void *nlp = memchr(start, '\n', live);
        if (nlp != NULL) {
            size_t line_len = (size_t)((const char *)nlp - start);
            *line = start;
            *len  = line_len;
            r->head += line_len + 1;
            if (r->head == r->len) {
                /* Opportunistic reset to avoid pointless compactions
                 * later. Does not invalidate *line: we're only
                 * zeroing indices, not touching memory. */
                r->head = r->len = 0;
            }
            return MCP_READER_OK;
        }
    }

    if (r->overflow_pending) {
        r->overflow_pending = 0;
        return MCP_READER_TOO_LONG;
    }

    return MCP_READER_EMPTY;
}

/* ---------- Message construction ---------- */

char *mcp_build_list_changed(const char *kind, size_t *out_len) {
    /* Three canonical namespaces. Hard-coded so we never generate a
     * malformed envelope and never have to reason about format
     * strings. Adding new kinds is one more case label. */
    static const char tools_msg[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}\n";
    static const char prompts_msg[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/prompts/list_changed\"}\n";
    static const char resources_msg[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/resources/list_changed\"}\n";

    const char *src = NULL;
    size_t len = 0;
    if (kind == NULL) {
        return NULL;
    }
    if (strcmp(kind, "tools") == 0) {
        src = tools_msg;
        len = sizeof(tools_msg) - 1; /* exclude terminating NUL */
    } else if (strcmp(kind, "prompts") == 0) {
        src = prompts_msg;
        len = sizeof(prompts_msg) - 1;
    } else if (strcmp(kind, "resources") == 0) {
        src = resources_msg;
        len = sizeof(resources_msg) - 1;
    } else {
        return NULL;
    }

    char *buf = xmalloc(len);
    memcpy(buf, src, len);
    if (out_len != NULL) {
        *out_len = len;
    }
    return buf;
}

char *mcp_build_error_response(const struct mcp_id *id,
                               int code,
                               const char *message,
                               cJSON *data,
                               size_t *out_len) {
    if (id == NULL || message == NULL) {
        cJSON_Delete(data);
        return NULL;
    }
    if (id->tag == MCP_ID_NONE) {
        /* Notifications have no id; there is no valid error response
         * to send back. The caller is expected to drop the message
         * and log instead. */
        cJSON_Delete(data);
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        cJSON_Delete(data);
        return NULL;
    }
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    if (id->tag == MCP_ID_INT) {
        cJSON_AddNumberToObject(root, "id", (double)id->v.i);
    } else {
        cJSON_AddStringToObject(root, "id", id->v.s);
    }
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", (double)code);
    cJSON_AddStringToObject(err, "message", message);
    if (data != NULL) {
        cJSON_AddItemToObject(err, "data", data);
    }
    cJSON_AddItemToObject(root, "error", err);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) return NULL;

    size_t jlen = strlen(json);
    char *buf = xmalloc(jlen + 1);
    memcpy(buf, json, jlen);
    buf[jlen] = '\n';
    free(json);
    if (out_len != NULL) {
        *out_len = jlen + 1;
    }
    return buf;
}
