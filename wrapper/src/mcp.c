/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#include "mcp.h"

#include "util.h"

#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

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

#define SCAN_FALLBACK 1

#ifndef CJSON_NESTING_LIMIT
#define CJSON_NESTING_LIMIT 1000
#endif

struct scan_cur {
    const unsigned char *p;
    const unsigned char *end;
    int depth;
};

static int scan_peek(const struct scan_cur *c) {
    return (c->p < c->end) ? (int)c->p[0] : -1;
}

static void scan_ws(struct scan_cur *c) {
    while (c->p < c->end && c->p[0] <= 32) {
        c->p++;
    }
}

static int scan_key_is(const unsigned char *s, size_t n, const char *lit) {
    size_t ln = strlen(lit);
    return n == ln && memcmp(s, lit, n) == 0;
}

static int scan_hex_value(const unsigned char *p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int ch = p[i];
        int d;
        if (ch >= '0' && ch <= '9') {
            d = ch - '0';
        } else if (ch >= 'a' && ch <= 'f') {
            d = ch - 'a' + 10;
        } else if (ch >= 'A' && ch <= 'F') {
            d = ch - 'A' + 10;
        } else {
            return -1;
        }
        v = (v << 4) | d;
    }
    return v;
}

/* Advance past a JSON string. Sets *has_esc if any escape was seen.
 * inner/inner_len, when non-NULL, receive the raw slice between the
 * quotes (still escaped if has_esc). */
static int scan_string(struct scan_cur *c,
                       const unsigned char **inner,
                       size_t *inner_len,
                       int *has_esc) {
    if (scan_peek(c) != '"') {
        return 0;
    }
    const unsigned char *start = c->p + 1;
    c->p++;
    int esc = 0;
    while (c->p < c->end) {
        unsigned char ch = c->p[0];
        if (ch == '"') {
            if (inner != NULL) {
                *inner = start;
            }
            if (inner_len != NULL) {
                *inner_len = (size_t)(c->p - start);
            }
            if (has_esc != NULL) {
                *has_esc = esc;
            }
            c->p++;
            return 1;
        }
        if (ch != '\\') {
            c->p++;
            continue;
        }
        esc = 1;
        c->p++;
        if (c->p >= c->end) {
            return 0;
        }
        unsigned char e = c->p[0];
        switch (e) {
        case '"':
        case '\\':
        case '/':
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't':
            c->p++;
            break;
        case 'u': {
            c->p++;
            if ((size_t)(c->end - c->p) < 4) {
                return 0;
            }
            int cp = scan_hex_value(c->p);
            if (cp < 0) {
                return 0;
            }
            c->p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if ((size_t)(c->end - c->p) < 6 ||
                    c->p[0] != '\\' || c->p[1] != 'u') {
                    return 0;
                }
                int lo = scan_hex_value(c->p + 2);
                if (lo < 0xDC00 || lo > 0xDFFF) {
                    return 0;
                }
                c->p += 6;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return 0;
            }
            break;
        }
        default:
            return 0;
        }
    }
    return 0;
}

/* Consume a number the same way cJSON's parse_number does: a run of
 * [0-9+-.eE] that strtod would accept. Requires at least one digit. */
static int scan_number(struct scan_cur *c, int *saw_non_int) {
    if (c->p >= c->end) {
        return 0;
    }
    unsigned char ch = c->p[0];
    if (ch != '-' && (ch < '0' || ch > '9')) {
        return 0;
    }
    int digit = 0;
    int non_int = 0;
    while (c->p < c->end) {
        ch = c->p[0];
        if (ch >= '0' && ch <= '9') {
            digit = 1;
            c->p++;
            continue;
        }
        if (ch == '.' || ch == 'e' || ch == 'E' || ch == '+' || ch == '-') {
            if (ch == '.' || ch == 'e' || ch == 'E') {
                non_int = 1;
            }
            c->p++;
            continue;
        }
        break;
    }
    if (saw_non_int != NULL) {
        *saw_non_int = non_int;
    }
    return digit;
}

static int scan_literal(struct scan_cur *c, const char *lit, size_t n) {
    if ((size_t)(c->end - c->p) < n) {
        return 0;
    }
    if (memcmp(c->p, lit, n) != 0) {
        return 0;
    }
    c->p += n;
    return 1;
}

static int scan_value(struct scan_cur *c);

static int scan_array(struct scan_cur *c) {
    if (scan_peek(c) != '[') {
        return 0;
    }
    if (c->depth >= CJSON_NESTING_LIMIT) {
        return 0;
    }
    c->depth++;
    c->p++;
    scan_ws(c);
    if (scan_peek(c) == ']') {
        c->p++;
        c->depth--;
        return 1;
    }
    for (;;) {
        scan_ws(c);
        if (!scan_value(c)) {
            return 0;
        }
        scan_ws(c);
        if (scan_peek(c) == ',') {
            c->p++;
            continue;
        }
        if (scan_peek(c) == ']') {
            c->p++;
            c->depth--;
            return 1;
        }
        return 0;
    }
}

static int scan_object(struct scan_cur *c) {
    if (scan_peek(c) != '{') {
        return 0;
    }
    if (c->depth >= CJSON_NESTING_LIMIT) {
        return 0;
    }
    c->depth++;
    c->p++;
    scan_ws(c);
    if (scan_peek(c) == '}') {
        c->p++;
        c->depth--;
        return 1;
    }
    for (;;) {
        scan_ws(c);
        if (!scan_string(c, NULL, NULL, NULL)) {
            return 0;
        }
        scan_ws(c);
        if (scan_peek(c) != ':') {
            return 0;
        }
        c->p++;
        scan_ws(c);
        if (!scan_value(c)) {
            return 0;
        }
        scan_ws(c);
        if (scan_peek(c) == ',') {
            c->p++;
            continue;
        }
        if (scan_peek(c) == '}') {
            c->p++;
            c->depth--;
            return 1;
        }
        return 0;
    }
}

static int scan_value(struct scan_cur *c) {
    int ch = scan_peek(c);
    if (ch == 'n') {
        return scan_literal(c, "null", 4);
    }
    if (ch == 'f') {
        return scan_literal(c, "false", 5);
    }
    if (ch == 't') {
        return scan_literal(c, "true", 4);
    }
    if (ch == '"') {
        return scan_string(c, NULL, NULL, NULL);
    }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        return scan_number(c, NULL);
    }
    if (ch == '[') {
        return scan_array(c);
    }
    if (ch == '{') {
        return scan_object(c);
    }
    return 0;
}

static int scan_parse_int64(const unsigned char *s, size_t n, int64_t *out) {
    if (n == 0) {
        return 0;
    }
    int neg = 0;
    size_t i = 0;
    if (s[0] == '-') {
        neg = 1;
        i++;
    }
    if (i >= n || s[i] < '0' || s[i] > '9') {
        return 0;
    }
    uint64_t acc = 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
        uint64_t d = (uint64_t)(s[i] - '0');
        if (acc > (UINT64_MAX - d) / 10u) {
            return 0;
        }
        acc = acc * 10u + d;
    }
    if (neg) {
        if (acc > (uint64_t)INT64_MAX + 1u) {
            return 0;
        }
        *out = (acc == (uint64_t)INT64_MAX + 1u)
            ? INT64_MIN
            : -(int64_t)acc;
    } else {
        if (acc > (uint64_t)INT64_MAX) {
            return 0;
        }
        *out = (int64_t)acc;
    }
    return 1;
}

static void scan_skip_bom(struct scan_cur *c) {
    if ((size_t)(c->end - c->p) >= 3 &&
        c->p[0] == 0xEF && c->p[1] == 0xBB && c->p[2] == 0xBF) {
        c->p += 3;
    }
}

static int classify_envelope(int has_id, int has_method,
                             int has_result, int has_error,
                             enum mcp_kind *kind, int *is_error) {
    if (has_method && has_id) {
        *kind = MCP_KIND_REQUEST;
        *is_error = 0;
        return MCP_PARSE_OK;
    }
    if (has_method && !has_id) {
        *kind = MCP_KIND_NOTIFICATION;
        *is_error = 0;
        return MCP_PARSE_OK;
    }
    if (!has_method && has_id && (has_result || has_error)) {
        *kind = MCP_KIND_RESPONSE;
        *is_error = has_error;
        return MCP_PARSE_OK;
    }
    return MCP_PARSE_ERR_INCOHERENT;
}

/* Validating top-level scanner. Returns MCP_PARSE_OK and fills *out
 * without touching cJSON; SCAN_FALLBACK when an escaped id/method or
 * key needs cJSON to unescape; or an MCP_PARSE_ERR_* code. Skipped
 * values are still validated, so a message cJSON would refuse is not
 * forwarded. Duplicate keys are first-wins. */
static int scan_envelope(const char *bytes, size_t len, struct mcp_msg *out) {
    struct scan_cur c = {
        .p = (const unsigned char *)bytes,
        .end = (const unsigned char *)bytes + len,
        .depth = 0,
    };
    scan_skip_bom(&c);
    scan_ws(&c);
    if (scan_peek(&c) != '{') {
        if (scan_value(&c)) {
            return MCP_PARSE_ERR_NOT_OBJECT;
        }
        return MCP_PARSE_ERR_JSON;
    }

    int seen_id = 0, seen_method = 0, seen_result = 0, seen_error = 0;
    int has_id = 0, has_method = 0, has_result = 0, has_error = 0;
    enum mcp_id_tag id_tag = MCP_ID_NONE;
    int64_t id_i = 0;
    const unsigned char *id_s = NULL;
    size_t id_slen = 0;
    const unsigned char *method = NULL;
    size_t method_len = 0;
    int fallback = 0;

    if (c.depth >= CJSON_NESTING_LIMIT) {
        return MCP_PARSE_ERR_JSON;
    }
    c.depth++;
    c.p++;
    scan_ws(&c);
    if (scan_peek(&c) == '}') {
        c.p++;
        return MCP_PARSE_ERR_INCOHERENT;
    }

    for (;;) {
        scan_ws(&c);
        const unsigned char *key = NULL;
        size_t klen = 0;
        int key_esc = 0;
        if (!scan_string(&c, &key, &klen, &key_esc)) {
            return MCP_PARSE_ERR_JSON;
        }
        if (key_esc) {
            fallback = 1;
        }
        scan_ws(&c);
        if (scan_peek(&c) != ':') {
            return MCP_PARSE_ERR_JSON;
        }
        c.p++;
        scan_ws(&c);

        int take = 0;
        int is_id = !key_esc && scan_key_is(key, klen, "id");
        int is_method = !key_esc && scan_key_is(key, klen, "method");
        int is_result = !key_esc && scan_key_is(key, klen, "result");
        int is_error = !key_esc && scan_key_is(key, klen, "error");

        if (is_id && !seen_id) {
            seen_id = 1;
            take = 1;
        } else if (is_method && !seen_method) {
            seen_method = 1;
            take = 1;
        } else if (is_result && !seen_result) {
            seen_result = 1;
            has_result = 1;
        } else if (is_error && !seen_error) {
            seen_error = 1;
            has_error = 1;
        }

        if (take && is_id) {
            int ch = scan_peek(&c);
            if (ch == '"') {
                int esc = 0;
                if (!scan_string(&c, &id_s, &id_slen, &esc)) {
                    return MCP_PARSE_ERR_JSON;
                }
                if (esc) {
                    fallback = 1;
                } else {
                    has_id = 1;
                    id_tag = MCP_ID_STRING;
                }
            } else if (ch == '-' || (ch >= '0' && ch <= '9')) {
                const unsigned char *nstart = c.p;
                int non_int = 0;
                if (!scan_number(&c, &non_int)) {
                    return MCP_PARSE_ERR_JSON;
                }
                if (non_int) {
                    /* Float or scientific: cJSON decides BAD_ID vs int. */
                    fallback = 1;
                } else if (!scan_parse_int64(nstart, (size_t)(c.p - nstart), &id_i)) {
                    fallback = 1;
                } else {
                    has_id = 1;
                    id_tag = MCP_ID_INT;
                }
            } else {
                if (!scan_value(&c)) {
                    return MCP_PARSE_ERR_JSON;
                }
                return MCP_PARSE_ERR_BAD_ID;
            }
        } else if (take && is_method) {
            int ch = scan_peek(&c);
            if (ch == '"') {
                int esc = 0;
                if (!scan_string(&c, &method, &method_len, &esc)) {
                    return MCP_PARSE_ERR_JSON;
                }
                if (esc) {
                    fallback = 1;
                } else {
                    has_method = 1;
                }
            } else {
                if (!scan_value(&c)) {
                    return MCP_PARSE_ERR_JSON;
                }
                return MCP_PARSE_ERR_BAD_METHOD;
            }
        } else if (!scan_value(&c)) {
            return MCP_PARSE_ERR_JSON;
        }

        scan_ws(&c);
        if (scan_peek(&c) == ',') {
            c.p++;
            continue;
        }
        if (scan_peek(&c) == '}') {
            c.p++;
            break;
        }
        return MCP_PARSE_ERR_JSON;
    }

    if (fallback) {
        return SCAN_FALLBACK;
    }

    enum mcp_kind kind = MCP_KIND_UNKNOWN;
    int is_error = 0;
    int rc = classify_envelope(has_id, has_method, has_result, has_error,
                               &kind, &is_error);
    if (rc != MCP_PARSE_OK) {
        return rc;
    }

    memset(out, 0, sizeof(*out));
    out->kind = kind;
    out->is_error = is_error;
    if (id_tag == MCP_ID_INT) {
        out->id.tag = MCP_ID_INT;
        out->id.v.i = id_i;
    } else if (id_tag == MCP_ID_STRING) {
        out->id.tag = MCP_ID_STRING;
        out->id.v.s = xmalloc(id_slen + 1);
        memcpy(out->id.v.s, id_s, id_slen);
        out->id.v.s[id_slen] = '\0';
    }
    if (has_method) {
        out->method = xmalloc(method_len + 1);
        memcpy(out->method, method, method_len);
        out->method[method_len] = '\0';
    }
    out->raw = xmalloc(len);
    memcpy(out->raw, bytes, len);
    out->raw_len = len;
    return MCP_PARSE_OK;
}

static int parse_via_cjson(const char *bytes, size_t len, struct mcp_msg *out) {
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

    out->raw = xmalloc(len);
    memcpy(out->raw, bytes, len);
    out->raw_len = len;

    cJSON_Delete(root);
    return MCP_PARSE_OK;
}

int mcp_msg_parse(const char *bytes, size_t len, struct mcp_msg *out) {
    if (out == NULL) {
        return MCP_PARSE_ERR_JSON;
    }
    memset(out, 0, sizeof(*out));

    if (bytes == NULL || len == 0) {
        return MCP_PARSE_ERR_JSON;
    }

    int rc = scan_envelope(bytes, len, out);
    if (rc == SCAN_FALLBACK) {
        memset(out, 0, sizeof(*out));
        return parse_via_cjson(bytes, len, out);
    }
    return rc;
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
