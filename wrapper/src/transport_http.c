/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#include "transport_http.h"

#include "log.h"
#include "util.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* MCP wire-protocol-version we advertise. Tracked by the mcp-go
 * server as the header `MCP-Protocol-Version`. The daemon expects
 * the client to send it on every request once initialize has
 * settled — we just send it unconditionally, which the server
 * tolerates on pre-initialize requests too. */
#define MCP_PROTOCOL_VERSION "2025-11-25"

/* =============================================================
 * URL parsing
 * ============================================================= */

struct http_url {
    char host[256];   /* e.g. "localhost" */
    int  port;        /* e.g. 19419 */
    char path[256];   /* e.g. "/mcp"; never empty (default "/") */
};

/* Return 1 if `host` is a loopback form we accept. We match by name
 * to keep the surface tiny — no DNS, no getaddrinfo for validation
 * (we use getaddrinfo for the actual connect). */
static int is_local_host(const char *host) {
    if (host == NULL) return 0;
    return strcmp(host, "localhost") == 0
        || strcmp(host, "127.0.0.1") == 0
        || strcmp(host, "::1") == 0;
}

/* Parse `http://host[:port]/path`. Returns 0 on success, -1 on
 * error (errno EINVAL). Defaults: port 80 (if omitted), path "/"
 * (if omitted). */
static int http_url_parse(const char *url, struct http_url *out) {
    if (url == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    const char *p = url;
    const char prefix[] = "http://";
    size_t prefix_len = sizeof(prefix) - 1;
    if (strncmp(p, prefix, prefix_len) != 0) {
        log_error("http_url: scheme must be http:// (got %s)", url);
        errno = EINVAL;
        return -1;
    }
    p += prefix_len;

    /* host ends at ':' or '/' or end-of-string */
    const char *host_start = p;
    const char *host_end   = p;
    while (*host_end != '\0' && *host_end != ':' && *host_end != '/') {
        host_end++;
    }
    size_t host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        log_error("http_url: missing or oversized host in %s", url);
        errno = EINVAL;
        return -1;
    }
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    if (!is_local_host(out->host)) {
        log_error("http_url: host %s is not a loopback; only "
                  "localhost / 127.0.0.1 / ::1 are accepted in v1",
                  out->host);
        errno = EINVAL;
        return -1;
    }

    /* port (optional) */
    out->port = 80;
    p = host_end;
    if (*p == ':') {
        p++;
        char *endp = NULL;
        long port = strtol(p, &endp, 10);
        if (endp == p || port <= 0 || port > 65535) {
            log_error("http_url: invalid port in %s", url);
            errno = EINVAL;
            return -1;
        }
        out->port = (int)port;
        p = endp;
    }

    /* path (optional, defaults to "/") */
    if (*p == '\0') {
        out->path[0] = '/';
        out->path[1] = '\0';
    } else if (*p == '/') {
        size_t path_len = strlen(p);
        if (path_len >= sizeof(out->path)) {
            log_error("http_url: oversized path in %s", url);
            errno = EINVAL;
            return -1;
        }
        memcpy(out->path, p, path_len + 1);
    } else {
        log_error("http_url: unexpected character after host in %s",
                  url);
        errno = EINVAL;
        return -1;
    }

    return 0;
}

/* =============================================================
 * TCP connect
 * ============================================================= */

/* Connect a blocking TCP socket to host:port. Returns a connected
 * fd on success, -1 on error (errno set). */
static int tcp_connect(const char *host, int port) {
    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", port);

    struct addrinfo hints = {0};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, portbuf, &hints, &res);
    if (gai != 0) {
        log_error("tcp_connect: getaddrinfo(%s:%d): %s",
                  host, port, gai_strerror(gai));
        errno = EIO;
        return -1;
    }

    int fd = -1;
    int saved_errno = ECONNREFUSED;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            saved_errno = errno;
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        saved_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        errno = saved_errno;
        return -1;
    }
    return fd;
}

/* =============================================================
 * Blocking full-read / full-write helpers
 *
 * The HTTP side is blocking — localhost request/response cycles
 * complete in sub-millisecond time and the event loop can tolerate
 * a brief stall during send(). Non-blocking + poll would add
 * complexity for no measurable win on localhost.
 * ============================================================= */

static int write_all_fd(int fd, const void *buf, size_t n) {
    const char *p = buf;
    size_t left = n;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w > 0) {
            p    += (size_t)w;
            left -= (size_t)w;
            continue;
        }
        if (w == -1 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

/* Read up to n bytes, blocking. Returns bytes read (>0), 0 on EOF,
 * -1 on error. Retries on EINTR. */
static ssize_t read_some(int fd, void *buf, size_t n) {
    ssize_t r;
    do {
        r = read(fd, buf, n);
    } while (r == -1 && errno == EINTR);
    return r;
}

/* =============================================================
 * Response parser
 *
 * Reads HTTP/1.1 response headers into a growable buffer until the
 * CRLF-CRLF terminator, parses the status line and headers, then
 * streams the body. Body delivery depends on Transfer-Encoding and
 * Content-Length — both are supported; anything else (identity with
 * no length, multipart, etc.) is rejected.
 * ============================================================= */

struct http_response {
    int    status;            /* e.g. 200 */
    int    is_sse;            /* 1 if Content-Type: text/event-stream */
    long   content_length;    /* -1 if unknown / chunked */
    int    is_chunked;
    char   session_id[128];   /* captured from MCP-Session-Id header; "" if none */
};

/* Case-insensitive ASCII compare. strcasecmp is in POSIX but not
 * strict C99 — be explicit to avoid portability surprises. */
static int ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Trim leading whitespace in-place (returns a pointer into the
 * argument). Trailing whitespace is zeroed. */
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t'
                  || s[n-1] == '\r' || s[n-1] == '\n')) {
        s[--n] = '\0';
    }
    return s;
}

/* Read from fd until we have CRLF-CRLF in the buffer. Returns the
 * number of bytes consumed (up to and including the terminator) on
 * success, -1 on error/EOF. `*leftover` gets the bytes already read
 * past the terminator (0..n), and they're at buf[consumed..buf_len). */
static int read_until_headers_done(int fd, char *buf, size_t buf_cap,
                                   size_t *buf_len_out,
                                   size_t *header_end_out) {
    size_t have = 0;
    for (;;) {
        if (have >= buf_cap) {
            log_error("http: response headers exceed %zu bytes", buf_cap);
            errno = EMSGSIZE;
            return -1;
        }
        ssize_t r = read_some(fd, buf + have, buf_cap - have);
        if (r == 0) {
            log_error("http: unexpected EOF before end of headers");
            errno = ECONNABORTED;
            return -1;
        }
        if (r < 0) {
            log_error("http: read headers: %s", strerror(errno));
            return -1;
        }
        have += (size_t)r;
        /* Look for CRLF-CRLF. Scan the whole buffer on every
         * iteration — cheap for headers we expect to be under 4 KB. */
        for (size_t i = 3; i < have; i++) {
            if (buf[i-3] == '\r' && buf[i-2] == '\n'
             && buf[i-1] == '\r' && buf[i]   == '\n') {
                *header_end_out = i + 1;
                *buf_len_out    = have;
                return 0;
            }
        }
    }
}

/* Parse the status line + headers, populate out. The header block
 * is nul-terminated in-place (replacing \r with \0). */
static int parse_response_headers(char *hdr, size_t hdr_len,
                                  struct http_response *out) {
    memset(out, 0, sizeof(*out));
    out->content_length = -1;

    /* Split into lines by replacing \r\n with \0\0. */
    size_t i = 0;
    char *line = hdr;
    char *line_start = hdr;
    int line_index = 0;
    while (i + 1 < hdr_len) {
        if (hdr[i] == '\r' && hdr[i+1] == '\n') {
            hdr[i]   = '\0';
            hdr[i+1] = '\0';
            if (line_index == 0) {
                /* "HTTP/1.1 200 OK" */
                char *sp = strchr(line, ' ');
                if (sp == NULL) {
                    log_error("http: malformed status line");
                    errno = EPROTO;
                    return -1;
                }
                out->status = (int)strtol(sp + 1, NULL, 10);
                if (out->status < 100 || out->status > 599) {
                    log_error("http: invalid status %d", out->status);
                    errno = EPROTO;
                    return -1;
                }
            } else if (line[0] != '\0') {
                char *colon = strchr(line, ':');
                if (colon != NULL) {
                    *colon = '\0';
                    char *name  = line;
                    char *value = trim(colon + 1);
                    if (ci_eq(name, "Content-Length")) {
                        char *endp = NULL;
                        long n = strtol(value, &endp, 10);
                        if (endp != value && n >= 0) {
                            out->content_length = n;
                        }
                    } else if (ci_eq(name, "Transfer-Encoding")) {
                        if (strstr(value, "chunked") != NULL) {
                            out->is_chunked = 1;
                        }
                    } else if (ci_eq(name, "Content-Type")) {
                        if (strstr(value, "text/event-stream") != NULL) {
                            out->is_sse = 1;
                        }
                    } else if (ci_eq(name, "MCP-Session-Id")
                            || ci_eq(name, "Mcp-Session-Id")) {
                        snprintf(out->session_id, sizeof(out->session_id),
                                 "%s", value);
                    }
                }
            }
            line_index++;
            i += 2;
            line_start = hdr + i;
            line = line_start;
            continue;
        }
        i++;
    }
    return 0;
}

/* =============================================================
 * Chunked transfer decoder
 *
 * Feed raw bytes; emit chunk-body bytes via the callback. Returns
 * 1 when the terminating zero-chunk has been consumed, 0 on
 * progress (more bytes needed), -1 on malformed input.
 * ============================================================= */

enum chunk_state {
    CHUNK_WANT_SIZE,     /* reading the hex length line */
    CHUNK_WANT_DATA,     /* reading `remaining` bytes of chunk data */
    CHUNK_WANT_CRLF,     /* reading the trailing CRLF after chunk data */
    CHUNK_DONE,
};

struct chunk_dec {
    enum chunk_state state;
    long             remaining;
    char             line[64];
    size_t           line_len;
    int              crlf_seen;
};

static void chunk_dec_init(struct chunk_dec *c) {
    memset(c, 0, sizeof(*c));
    c->state = CHUNK_WANT_SIZE;
}

typedef void (*body_sink_fn)(void *ctx, const char *bytes, size_t n);

static int chunk_dec_feed(struct chunk_dec *c,
                          const char *bytes, size_t n,
                          body_sink_fn sink, void *ctx) {
    size_t i = 0;
    while (i < n) {
        switch (c->state) {
        case CHUNK_WANT_SIZE: {
            char b = bytes[i++];
            if (b == '\n') {
                /* End of size line. Parse hex. */
                c->line[c->line_len] = '\0';
                /* Strip any \r. */
                if (c->line_len > 0 && c->line[c->line_len-1] == '\r') {
                    c->line[c->line_len-1] = '\0';
                }
                /* Cut off any chunk extension (after ';'). */
                char *semi = strchr(c->line, ';');
                if (semi != NULL) *semi = '\0';
                char *endp = NULL;
                long size = strtol(c->line, &endp, 16);
                if (endp == c->line || size < 0) {
                    log_error("http: malformed chunk size '%s'", c->line);
                    errno = EPROTO;
                    return -1;
                }
                c->line_len  = 0;
                c->remaining = size;
                if (size == 0) {
                    /* Zero-chunk: body ends here. Trailers would
                     * follow before the final CRLF, but we ignore
                     * them — mark done and return. The server sends
                     * Connection: close so any remaining bytes get
                     * swallowed by the read-loop's subsequent EOF. */
                    c->state = CHUNK_DONE;
                    return 1;
                }
                c->state = CHUNK_WANT_DATA;
                break;
            }
            if (c->line_len + 1 >= sizeof(c->line)) {
                log_error("http: chunk size line too long");
                errno = EPROTO;
                return -1;
            }
            c->line[c->line_len++] = b;
            break;
        }
        case CHUNK_WANT_DATA: {
            size_t avail = n - i;
            size_t take  = avail < (size_t)c->remaining
                         ? avail : (size_t)c->remaining;
            if (sink != NULL && take > 0) {
                sink(ctx, bytes + i, take);
            }
            i            += take;
            c->remaining -= (long)take;
            if (c->remaining == 0) {
                c->state     = CHUNK_WANT_CRLF;
                c->crlf_seen = 0;
            }
            break;
        }
        case CHUNK_WANT_CRLF: {
            char b = bytes[i++];
            if (b == '\r') {
                c->crlf_seen = 1;
                break;
            }
            if (b == '\n' && c->crlf_seen) {
                c->state     = CHUNK_WANT_SIZE;
                c->crlf_seen = 0;
                break;
            }
            log_error("http: expected CRLF after chunk data");
            errno = EPROTO;
            return -1;
        }
        case CHUNK_DONE:
            /* Leftover bytes after the terminator are unexpected but
             * benign; we've already returned 1. */
            return 1;
        }
    }
    return c->state == CHUNK_DONE ? 1 : 0;
}

/* =============================================================
 * SSE parser
 *
 * Feed bytes from the (already-dechunked) body; emit one callback
 * per SSE event when the terminating blank line arrives. We track
 * `data:` only — other fields (event:, id:, retry:) are ignored in
 * v1 since MCP uses unnamed events with bare data payloads.
 * ============================================================= */

struct sse_parser {
    char   data[65536];  /* accumulated data field for the current event */
    size_t data_len;
    char   line[65536];  /* current line being accumulated */
    size_t line_len;
    int    had_line;     /* 1 once we've seen at least one non-empty line */
};

static void sse_parser_init(struct sse_parser *s) {
    s->data_len = 0;
    s->line_len = 0;
    s->had_line = 0;
}

/* Handle one complete line (the terminating LF has been stripped
 * and any trailing CR removed). */
static void sse_handle_line(struct sse_parser *s,
                            const char *line, size_t len,
                            transport_on_message on_msg, void *ctx) {
    if (len == 0) {
        /* Blank line: event terminator. Emit the accumulated data,
         * then reset. We only emit if we saw at least one data line;
         * pure empty events (keepalives) are discarded. */
        if (s->data_len > 0) {
            if (on_msg != NULL) {
                on_msg(ctx, s->data, s->data_len);
            }
            s->data_len = 0;
        }
        s->had_line = 0;
        return;
    }
    s->had_line = 1;
    /* Skip comments (lines starting with ':'). */
    if (line[0] == ':') {
        return;
    }
    /* Parse `field[:[ value]]`. */
    const char *colon = memchr(line, ':', len);
    const char *value;
    size_t      value_len;
    size_t      name_len;
    if (colon == NULL) {
        /* Whole line is the field name, empty value. */
        value     = "";
        value_len = 0;
        name_len  = len;
    } else {
        name_len  = (size_t)(colon - line);
        value     = colon + 1;
        value_len = len - name_len - 1;
        /* SSE: leading space is stripped if present. */
        if (value_len > 0 && value[0] == ' ') {
            value++;
            value_len--;
        }
    }
    /* Only handle the `data` field in v1. */
    if (name_len == 4 && memcmp(line, "data", 4) == 0) {
        /* Append value with a newline separator between lines (per
         * the SSE spec: multi-line data fields are joined with \n,
         * but the trailing \n after the last data line is NOT kept).
         * We approximate this by prefixing a \n before every data
         * line after the first within the same event. */
        if (s->data_len > 0) {
            if (s->data_len + 1 < sizeof(s->data)) {
                s->data[s->data_len++] = '\n';
            }
        }
        size_t room = sizeof(s->data) - s->data_len;
        size_t take = value_len < room ? value_len : room;
        memcpy(s->data + s->data_len, value, take);
        s->data_len += take;
    }
}

static void sse_parser_feed(struct sse_parser *s,
                            const char *bytes, size_t n,
                            transport_on_message on_msg, void *ctx) {
    for (size_t i = 0; i < n; i++) {
        char b = bytes[i];
        if (b == '\n') {
            /* End of line. Strip trailing \r if present. */
            size_t len = s->line_len;
            if (len > 0 && s->line[len-1] == '\r') {
                len--;
            }
            sse_handle_line(s, s->line, len, on_msg, ctx);
            s->line_len = 0;
            continue;
        }
        if (s->line_len + 1 >= sizeof(s->line)) {
            /* Line too long — drop it by continuing to consume bytes
             * until the next \n, then resetting. We log once. */
            if (s->line_len == sizeof(s->line) - 1) {
                log_warn("sse: dropping oversized line");
            }
            s->line_len = sizeof(s->line) - 1;
            continue;
        }
        s->line[s->line_len++] = b;
    }
}

/* =============================================================
 * Inbound message queue
 *
 * Messages parsed from POST responses are held in a singly-linked
 * list until pump() drains them. Each node owns its bytes.
 * ============================================================= */

struct msg_node {
    struct msg_node *next;
    size_t           len;
    char             bytes[];
};

struct msg_queue {
    struct msg_node *head;
    struct msg_node *tail;
};

static void msg_queue_push(struct msg_queue *q, const char *bytes, size_t n) {
    struct msg_node *node = xcalloc(1, sizeof(*node) + n);
    node->len = n;
    memcpy(node->bytes, bytes, n);
    if (q->tail != NULL) {
        q->tail->next = node;
    } else {
        q->head = node;
    }
    q->tail = node;
}

static struct msg_node *msg_queue_pop(struct msg_queue *q) {
    struct msg_node *node = q->head;
    if (node == NULL) return NULL;
    q->head = node->next;
    if (q->head == NULL) q->tail = NULL;
    node->next = NULL;
    return node;
}

static void msg_queue_clear(struct msg_queue *q) {
    struct msg_node *node = q->head;
    while (node != NULL) {
        struct msg_node *next = node->next;
        free(node);
        node = next;
    }
    q->head = q->tail = NULL;
}

/* =============================================================
 * transport_http: self and vtable
 * ============================================================= */

struct http_self {
    struct http_url   url;
    char              session_id[128];   /* "" until initialize response */
    int               started;

    /* Self-pipe used to wake poll() when messages have been queued. */
    int               wake_fds[2];       /* [0]=read, [1]=write */

    /* Inbound messages from POST responses, awaiting pump(). */
    struct msg_queue  queue;
};

/* Append one parsed message to the transport's inbound queue and
 * wake the event loop via the self-pipe. Safe to call from within
 * the SSE/chunked body parsing. */
static void http_enqueue(struct http_self *h, const char *bytes, size_t n) {
    if (n == 0) return;
    msg_queue_push(&h->queue, bytes, n);
    /* Non-blocking best-effort: if the pipe is full, the poll is
     * already ready to fire so the byte is redundant anyway. */
    if (h->wake_fds[1] >= 0) {
        char b = 'x';
        ssize_t w;
        do {
            w = write(h->wake_fds[1], &b, 1);
        } while (w == -1 && errno == EINTR);
        (void)w;
    }
}

/* Adapter: the SSE parser emits each complete data-event body via
 * transport_on_message. We rebind that to the queue instead,
 * because pump() delivers to the real on_msg later. */
static void sse_to_queue(void *vctx, const char *line, size_t len) {
    struct http_self *h = vctx;
    http_enqueue(h, line, len);
}

/* Adapter for the chunked body sink: feed raw chunk-body bytes into
 * either the SSE parser or directly accumulate into a buffer (for
 * plain application/json responses). */
struct body_ctx {
    struct http_self  *h;
    int                is_sse;
    struct sse_parser  sse;
    char              *json_buf;
    size_t             json_len;
    size_t             json_cap;
};

static void body_accept(void *vctx, const char *bytes, size_t n) {
    struct body_ctx *b = vctx;
    if (b->is_sse) {
        sse_parser_feed(&b->sse, bytes, n, sse_to_queue, b->h);
        return;
    }
    /* Accumulate plain JSON body for single-shot delivery. */
    if (b->json_len + n > b->json_cap) {
        size_t new_cap = b->json_cap == 0 ? 4096 : b->json_cap * 2;
        while (new_cap < b->json_len + n) new_cap *= 2;
        b->json_buf = xrealloc(b->json_buf, new_cap);
        b->json_cap = new_cap;
    }
    memcpy(b->json_buf + b->json_len, bytes, n);
    b->json_len += n;
}

/* Read the body of a response into the body_ctx, honouring
 * Transfer-Encoding and Content-Length. Returns 0 on success,
 * -1 on error. `initial` + `initial_len` are any bytes already read
 * past the header terminator. */
static int read_response_body(int fd,
                              const struct http_response *resp,
                              const char *initial, size_t initial_len,
                              struct body_ctx *b) {
    if (resp->is_chunked) {
        struct chunk_dec dec;
        chunk_dec_init(&dec);
        if (initial_len > 0) {
            int r = chunk_dec_feed(&dec, initial, initial_len,
                                   body_accept, b);
            if (r < 0) return -1;
            if (r == 1) return 0;
        }
        char buf[8192];
        for (;;) {
            ssize_t r = read_some(fd, buf, sizeof(buf));
            if (r == 0) {
                log_error("http: unexpected EOF mid-chunk");
                errno = ECONNABORTED;
                return -1;
            }
            if (r < 0) {
                log_error("http: body read: %s", strerror(errno));
                return -1;
            }
            int done = chunk_dec_feed(&dec, buf, (size_t)r,
                                      body_accept, b);
            if (done < 0) return -1;
            if (done == 1) return 0;
        }
    }

    if (resp->content_length >= 0) {
        long need = resp->content_length;
        if (initial_len > 0) {
            size_t take = initial_len < (size_t)need
                        ? initial_len : (size_t)need;
            body_accept(b, initial, take);
            need -= (long)take;
        }
        char buf[8192];
        while (need > 0) {
            size_t want = (size_t)(need < (long)sizeof(buf)
                                  ? need : (long)sizeof(buf));
            ssize_t r = read_some(fd, buf, want);
            if (r == 0) {
                log_error("http: unexpected EOF (body short by %ld)", need);
                errno = ECONNABORTED;
                return -1;
            }
            if (r < 0) {
                log_error("http: body read: %s", strerror(errno));
                return -1;
            }
            body_accept(b, buf, (size_t)r);
            need -= r;
        }
        return 0;
    }

    /* No Content-Length and not chunked. Read until EOF — only
     * valid with Connection: close, but we don't parse that header
     * explicitly: we treat EOF as end-of-body. Acceptable for
     * localhost servers that close the connection on streaming
     * responses. */
    if (initial_len > 0) {
        body_accept(b, initial, initial_len);
    }
    char buf[8192];
    for (;;) {
        ssize_t r = read_some(fd, buf, sizeof(buf));
        if (r == 0) return 0;
        if (r < 0) {
            log_error("http: body read: %s", strerror(errno));
            return -1;
        }
        body_accept(b, buf, (size_t)r);
    }
}

/* Build and write the POST request, then read and parse the
 * response. Queues any inbound MCP messages into h->queue and
 * captures the session id. Returns 0 on success, -1 on error. */
static int http_post_once(struct http_self *h,
                          const void *body_bytes, size_t body_len) {
    int fd = tcp_connect(h->url.host, h->url.port);
    if (fd < 0) {
        return -1;
    }

    /* Build request. */
    char header[1024];
    int n = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Accept: application/json, text/event-stream\r\n"
        "MCP-Protocol-Version: %s\r\n"
        "%s%s%s"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        h->url.path,
        h->url.host, h->url.port,
        MCP_PROTOCOL_VERSION,
        h->session_id[0] ? "MCP-Session-Id: " : "",
        h->session_id[0] ? h->session_id : "",
        h->session_id[0] ? "\r\n" : "",
        body_len);
    if (n <= 0 || (size_t)n >= sizeof(header)) {
        log_error("http: request header overflow (n=%d)", n);
        close(fd);
        errno = EMSGSIZE;
        return -1;
    }

    if (write_all_fd(fd, header, (size_t)n) != 0) {
        log_error("http: write request header: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (body_len > 0 && write_all_fd(fd, body_bytes, body_len) != 0) {
        log_error("http: write request body: %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* Read response headers. */
    char  hdr_buf[4096];
    size_t buf_len       = 0;
    size_t header_end    = 0;
    if (read_until_headers_done(fd, hdr_buf, sizeof(hdr_buf),
                                &buf_len, &header_end) != 0) {
        close(fd);
        return -1;
    }

    struct http_response resp;
    if (parse_response_headers(hdr_buf, header_end, &resp) != 0) {
        close(fd);
        return -1;
    }
    if (resp.status < 200 || resp.status >= 300) {
        log_error("http: non-2xx status %d from POST %s",
                  resp.status, h->url.path);
        close(fd);
        errno = EPROTO;
        return -1;
    }

    /* Capture session id if this was initialize (or any response
     * carrying one — we don't try to be clever about when). */
    if (resp.session_id[0] != '\0') {
        snprintf(h->session_id, sizeof(h->session_id), "%s",
                 resp.session_id);
        log_debug("http: session id captured (%s)", h->session_id);
    }

    /* Stream the body into the queue. */
    struct body_ctx body = {0};
    body.h      = h;
    body.is_sse = resp.is_sse;
    if (body.is_sse) {
        sse_parser_init(&body.sse);
    }

    const char *initial     = hdr_buf + header_end;
    size_t      initial_len = buf_len - header_end;

    int rc = read_response_body(fd, &resp, initial, initial_len, &body);
    close(fd);

    if (rc == 0 && !body.is_sse && body.json_len > 0) {
        /* Plain JSON response: a single MCP message. Strip any
         * trailing newline (mcp_reader_pop expects the body without
         * framing) though JSON usually has none. */
        size_t m = body.json_len;
        while (m > 0 && (body.json_buf[m-1] == '\n'
                      || body.json_buf[m-1] == '\r')) {
            m--;
        }
        http_enqueue(h, body.json_buf, m);
    }
    free(body.json_buf);
    return rc;
}

static int http_start(void *vself) {
    struct http_self *h = vself;
    if (h->started) {
        errno = EALREADY;
        return -1;
    }
    if (pipe(h->wake_fds) != 0) {
        log_error("http_start: pipe: %s", strerror(errno));
        return -1;
    }
    /* Non-blocking on the read end so pump() can drain safely. */
    int flags = fcntl(h->wake_fds[0], F_GETFL, 0);
    if (flags >= 0) {
        fcntl(h->wake_fds[0], F_SETFL, flags | O_NONBLOCK);
    }
    h->started = 1;
    h->session_id[0] = '\0';
    log_debug("http_start: target=http://%s:%d%s",
              h->url.host, h->url.port, h->url.path);
    return 0;
}

static int http_stop(void *vself) {
    struct http_self *h = vself;
    if (h->wake_fds[0] >= 0) { close(h->wake_fds[0]); h->wake_fds[0] = -1; }
    if (h->wake_fds[1] >= 0) { close(h->wake_fds[1]); h->wake_fds[1] = -1; }
    msg_queue_clear(&h->queue);
    h->started = 0;
    h->session_id[0] = '\0';
    return 0;
}

static int http_poll_fd(void *vself) {
    struct http_self *h = vself;
    return h->wake_fds[0];
}

/* Drain the wake byte(s) from the pipe so poll() doesn't spin
 * trivially on an empty queue next iteration. */
static void drain_wake(struct http_self *h) {
    if (h->wake_fds[0] < 0) return;
    char buf[64];
    for (;;) {
        ssize_t r = read(h->wake_fds[0], buf, sizeof(buf));
        if (r > 0) continue;
        break; /* EAGAIN or EOF — both mean nothing more to drain. */
    }
}

static int http_pump(void *vself,
                     transport_on_message on_msg, void *ctx) {
    struct http_self *h = vself;
    drain_wake(h);
    for (;;) {
        struct msg_node *node = msg_queue_pop(&h->queue);
        if (node == NULL) break;
        if (on_msg != NULL) {
            on_msg(ctx, node->bytes, node->len);
        }
        free(node);
    }
    return 0;
}

static int http_send(void *vself, const void *bytes, size_t n) {
    struct http_self *h = vself;
    if (!h->started) {
        errno = EBADF;
        return -1;
    }
    /* Strip a single trailing newline: the wrapper frames outbound
     * messages with a newline (matching the stdio contract), but
     * HTTP bodies carry no framing. */
    size_t body_len = n;
    const char *body = bytes;
    if (body_len > 0 && body[body_len-1] == '\n') {
        body_len--;
    }
    return http_post_once(h, body, body_len);
}

static void http_destroy(void *vself) {
    struct http_self *h = vself;
    if (h == NULL) return;
    http_stop(h);
    free(h);
}

static const struct transport_ops http_ops = {
    .start   = http_start,
    .stop    = http_stop,
    .poll_fd = http_poll_fd,
    .pump    = http_pump,
    .send    = http_send,
    .destroy = http_destroy,
};

/* =============================================================
 * Public constructor
 * ============================================================= */

struct transport *transport_http_new(const char *url) {
    if (url == NULL) {
        errno = EINVAL;
        return NULL;
    }
    struct http_self *h = xcalloc(1, sizeof(*h));
    if (http_url_parse(url, &h->url) != 0) {
        int saved = errno;
        free(h);
        errno = saved;
        return NULL;
    }
    h->wake_fds[0] = -1;
    h->wake_fds[1] = -1;

    struct transport *t = xcalloc(1, sizeof(*t));
    t->ops  = &http_ops;
    t->self = h;
    return t;
}
