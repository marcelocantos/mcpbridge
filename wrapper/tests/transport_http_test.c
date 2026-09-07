/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

/* Integration test for the HTTP transport.
 *
 * Forks a tiny fake MCP server that speaks enough of Streamable
 * HTTP to answer an initialize POST (plain JSON response + MCP-
 * Session-Id header), a follow-up tools/list POST (SSE response
 * carrying a response event AND a notification event before close),
 * and rejects requests that omit the session id after initialize.
 * The parent process drives transport_http against it:
 *
 *   1. transport_send(initialize) -> pump collects initialize result
 *      and the transport captures the session id.
 *   2. transport_send(tools/list) -> pump collects two messages: the
 *      response and the SSE notification, in that order.
 *   3. transport_send(tools/list) against a port with no listener
 *      fails cleanly.
 *
 * A 5s watchdog via SIGALRM aborts the process on hang. */

#include "../src/transport.h"
#include "../src/transport_http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fail_count = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);  \
            fail_count++;                                                  \
        }                                                                  \
    } while (0)

static void install_watchdog(void) {
    struct sigaction sa = {0};
    sa.sa_handler = SIG_DFL;
    sigaction(SIGALRM, &sa, NULL);
    /* Generous: the payload sweep moves several megabytes through
     * loopback. Still short enough to catch a hang. */
    alarm(30);
}

/* ==========================================================
 * Fake server (runs in child process).
 *
 * Binds 127.0.0.1:0, writes the bound port to the supplied fd as
 * a 4-byte big-endian integer, then serves two requests and exits.
 * ========================================================== */

/* Write all n bytes or die. */
static int write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    size_t left = n;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w > 0) { p += w; left -= (size_t)w; continue; }
        if (w == -1 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

/* Read until CRLF-CRLF. Returns header byte count on success (bytes
 * after header go to *trail_len at trail_buf), -1 on error. */
static int read_headers(int fd, char *buf, size_t cap,
                        size_t *total_out, size_t *header_end_out) {
    size_t have = 0;
    for (;;) {
        if (have >= cap) return -1;
        ssize_t r = read(fd, buf + have, cap - have);
        if (r <= 0) return -1;
        have += (size_t)r;
        for (size_t i = 3; i < have; i++) {
            if (buf[i-3] == '\r' && buf[i-2] == '\n'
             && buf[i-1] == '\r' && buf[i]   == '\n') {
                *header_end_out = i + 1;
                *total_out = have;
                return 0;
            }
        }
    }
}

static int parse_content_length(const char *hdr, size_t n) {
    const char *p = hdr;
    const char *end = hdr + n;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (eol == NULL) break;
        size_t line_len = (size_t)(eol - p);
        if (line_len > 15 && strncasecmp(p, "Content-Length:", 15) == 0) {
            return (int)strtol(p + 15, NULL, 10);
        }
        p = eol + 1;
    }
    return -1;
}

/* Check if header block contains `name: value` (case-insensitive
 * on the name, literal on the value). */
static int has_header(const char *hdr, size_t n,
                      const char *name, const char *value) {
    size_t nlen = strlen(name);
    size_t vlen = strlen(value);
    const char *p = hdr;
    const char *end = hdr + n;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (eol == NULL) break;
        size_t line_len = (size_t)(eol - p);
        if (line_len > nlen + 1
         && strncasecmp(p, name, nlen) == 0
         && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            if ((size_t)(eol - v) >= vlen
             && memcmp(v, value, vlen) == 0) {
                return 1;
            }
        }
        p = eol + 1;
    }
    return 0;
}

/* Serve one request. Returns 1 if this was the initialize request,
 * 0 for a follow-up, -1 on error. */
/* When non-zero, the non-initialize response carries an SSE event
 * whose result string is this many bytes. Set before fork_server so
 * the server child inherits it. Used by the payload-ceiling sweep:
 * the wrapper used to assemble SSE events into a fixed 64 KiB buffer
 * and truncate silently past that, which downstream dropped as
 * unparseable JSON — leaving the agent waiting forever. */
static size_t g_payload_bytes = 0;

/* The fixed text around the payload in the large-response body. */
static const char big_prefix[] =
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"text\":\"";
static const char big_suffix[] = "\"}}";

/* Length of the JSON message the server emits for a given payload. */
static size_t big_msg_len(size_t payload) {
    return (sizeof(big_prefix) - 1) + payload + (sizeof(big_suffix) - 1);
}

/* Write one HTTP chunk with the given body bytes. */
static int write_chunk(int fd, const char *bytes, size_t n) {
    char hdr[32];
    int hn = snprintf(hdr, sizeof(hdr), "%zx\r\n", n);
    if (write_all(fd, hdr, (size_t)hn) != 0) return -1;
    if (write_all(fd, bytes, n) != 0) return -1;
    return write_all(fd, "\r\n", 2);
}

/* Emit a chunked SSE response whose single data event carries a
 * result string of g_payload_bytes bytes. Deliberately split across
 * many small chunks so the chunked decoder and the SSE parser both
 * see the payload arrive in pieces. */
static void serve_big(int client_fd) {
    const char resp_hdr[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "\r\n";
    write_all(client_fd, resp_hdr, sizeof(resp_hdr) - 1);

    char head[128];
    int hn = snprintf(head, sizeof(head), "data: %s", big_prefix);
    write_chunk(client_fd, head, (size_t)hn);

    char filler[8192];
    memset(filler, 'x', sizeof(filler));
    size_t left = g_payload_bytes;
    while (left > 0) {
        size_t take = left < sizeof(filler) ? left : sizeof(filler);
        if (write_chunk(client_fd, filler, take) != 0) return;
        left -= take;
    }

    char tail[32];
    int tn = snprintf(tail, sizeof(tail), "%s\n\n", big_suffix);
    write_chunk(client_fd, tail, (size_t)tn);
    write_all(client_fd, "0\r\n\r\n", 5);
}

static int serve_one(int client_fd, int is_first) {
    char hdr_buf[4096];
    size_t total = 0, hdr_end = 0;
    if (read_headers(client_fd, hdr_buf, sizeof(hdr_buf),
                     &total, &hdr_end) != 0) {
        return -1;
    }
    int clen = parse_content_length(hdr_buf, hdr_end);
    if (clen < 0) clen = 0;

    /* Consume (and discard) body. We don't inspect it beyond the
     * method-name sniff below. */
    char body[8192];
    int body_have = (int)(total - hdr_end);
    if (body_have > 0) {
        memmove(body, hdr_buf + hdr_end, (size_t)body_have);
    }
    while (body_have < clen) {
        ssize_t r = read(client_fd, body + body_have,
                         (size_t)(clen - body_have));
        if (r <= 0) return -1;
        body_have += (int)r;
    }
    body[body_have < (int)sizeof(body) ? body_have : (int)sizeof(body) - 1] = '\0';

    int is_initialize = (strstr(body, "\"initialize\"") != NULL);

    /* After the first (initialize) request, every subsequent request
     * MUST carry an MCP-Session-Id header. Verify; if missing, 400. */
    if (!is_first) {
        if (!has_header(hdr_buf, hdr_end, "MCP-Session-Id", "sess-abc")) {
            const char err[] =
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Length: 0\r\n\r\n";
            write_all(client_fd, err, sizeof(err) - 1);
            return 0;
        }
    }

    if (is_initialize) {
        /* Plain JSON response + session id. */
        const char json_body[] =
            "{\"jsonrpc\":\"2.0\",\"id\":1,"
            "\"result\":{\"protocolVersion\":\"2025-11-25\"}}";
        char resp[512];
        int n = snprintf(resp, sizeof(resp),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "MCP-Session-Id: sess-abc\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n%s",
            sizeof(json_body) - 1, json_body);
        write_all(client_fd, resp, (size_t)n);
        return 1;
    }

    if (g_payload_bytes > 0) {
        serve_big(client_fd);
        return 0;
    }

    /* tools/list (or any follow-up) → SSE body with one response and
     * one notification, both chunked so the test exercises chunked
     * dechunking. */
    const char resp_hdr[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "\r\n";
    write_all(client_fd, resp_hdr, sizeof(resp_hdr) - 1);

    /* Chunk 1: response event. */
    const char evt1[] =
        "data: {\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[]}}\n\n";
    char chunk[256];
    int cn = snprintf(chunk, sizeof(chunk),
                      "%zx\r\n%s\r\n", sizeof(evt1) - 1, evt1);
    write_all(client_fd, chunk, (size_t)cn);

    /* Chunk 2: notification event. */
    const char evt2[] =
        "data: {\"jsonrpc\":\"2.0\","
        "\"method\":\"notifications/tools/list_changed\"}\n\n";
    cn = snprintf(chunk, sizeof(chunk),
                  "%zx\r\n%s\r\n", sizeof(evt2) - 1, evt2);
    write_all(client_fd, chunk, (size_t)cn);

    /* Terminator. */
    const char term[] = "0\r\n\r\n";
    write_all(client_fd, term, sizeof(term) - 1);
    return 0;
}

/* Child entry: binds, reports port, serves N requests, exits. */
static void run_server(int report_fd, int request_count) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) _exit(10);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    /* 127.0.0.1 — spelled out rather than via INADDR_LOOPBACK, which
     * needs platform-specific feature-test macros beyond
     * _POSIX_C_SOURCE=200809L. */
    addr.sin_addr.s_addr = htonl(0x7F000001);
    addr.sin_port        = 0;
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) _exit(11);
    if (listen(srv, 1) != 0) _exit(12);

    socklen_t alen = sizeof(addr);
    if (getsockname(srv, (struct sockaddr *)&addr, &alen) != 0) _exit(13);
    uint16_t port_net = addr.sin_port; /* network order */
    uint32_t port_be  = (uint32_t)port_net; /* 16-bit in 32-bit slot */
    write_all(report_fd, &port_be, sizeof(port_be));

    int is_first = 1;
    for (int i = 0; i < request_count; i++) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) _exit(14);
        serve_one(c, is_first);
        is_first = 0;
        close(c);
    }
    close(srv);
    _exit(0);
}

/* ==========================================================
 * Parent-side: launches server, runs transport, collects
 * ========================================================== */

/* Fork a server that handles `request_count` requests and return
 * its port plus pid. */
static int fork_server(int request_count, pid_t *child_out) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(pipefd[0]);
        run_server(pipefd[1], request_count);
        _exit(99);
    }
    close(pipefd[1]);
    uint32_t port_be = 0;
    if (read(pipefd[0], &port_be, sizeof(port_be)) != sizeof(port_be)) {
        close(pipefd[0]);
        return -1;
    }
    close(pipefd[0]);
    *child_out = pid;
    return (int)ntohs((uint16_t)port_be);
}

struct collector {
    char   buf[8192];
    size_t lens[8];
    size_t count;
    size_t total;
};

static void collect(void *vctx, const char *line, size_t len) {
    struct collector *c = vctx;
    if (c->count >= sizeof(c->lens) / sizeof(c->lens[0])) return;
    size_t room = sizeof(c->buf) - c->total;
    size_t take = len < room ? len : room;
    memcpy(c->buf + c->total, line, take);
    c->lens[c->count++] = take;
    c->total += take;
}

/* Pump until `*count` reaches want_msgs or the poll times out.
 * `count` points at the collector's own tally, so this works for any
 * collector shape. */
static int pump_until(struct transport *t,
                      transport_on_message on_msg, void *ctx,
                      const size_t *count,
                      size_t want_msgs, int timeout_ms) {
    int fd = transport_poll_fd(t);
    while (*count < want_msgs) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr == 0) break;
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        int r = transport_pump(t, on_msg, ctx);
        if (r < 0) return -1;
    }
    return 0;
}

/* Verify a buffer contains substring `needle`. */
static int contains(const char *buf, size_t len, const char *needle) {
    size_t nlen = strlen(needle);
    if (len < nlen) return 0;
    for (size_t i = 0; i + nlen <= len; i++) {
        if (memcmp(buf + i, needle, nlen) == 0) return 1;
    }
    return 0;
}

static void test_round_trip(void) {
    pid_t child = 0;
    int port = fork_server(2, &child);
    CHECK(port > 0, "server started and reported port");
    if (port <= 0) return;

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/mcp", port);

    struct transport *t = transport_http_new(url);
    CHECK(t != NULL, "transport created");

    int rc = transport_start(t);
    CHECK(rc == 0, "transport started");
    CHECK(transport_poll_fd(t) >= 0, "poll_fd set (self-pipe)");

    /* 1. initialize (plain JSON response). */
    const char init_msg[] =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{}}\n";
    rc = transport_send(t, init_msg, sizeof(init_msg) - 1);
    CHECK(rc == 0, "initialize sent");

    struct collector coll = {0};
    rc = pump_until(t, collect, &coll, &coll.count, 1, 2000);
    CHECK(rc == 0, "pump succeeded");
    CHECK(coll.count == 1, "one message collected for initialize");
    CHECK(contains(coll.buf, coll.total, "\"result\""),
          "initialize response contains a result");
    CHECK(contains(coll.buf, coll.total, "2025-11-25"),
          "initialize response echoes protocol version");

    /* 2. tools/list (SSE chunked response: response + notification). */
    memset(&coll, 0, sizeof(coll));
    const char list_msg[] =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n";
    rc = transport_send(t, list_msg, sizeof(list_msg) - 1);
    CHECK(rc == 0, "tools/list sent (session id propagated)");

    rc = pump_until(t, collect, &coll, &coll.count, 2, 2000);
    CHECK(rc == 0, "pump succeeded for tools/list");
    CHECK(coll.count == 2, "two messages collected (response + notification)");

    /* The response came first per send order; the notification
     * followed. Both messages end up concatenated into coll.buf; we
     * just verify both payloads are present somewhere in the buffer. */
    CHECK(contains(coll.buf, coll.total, "\"tools\":[]"),
          "tools/list response present");
    CHECK(contains(coll.buf, coll.total, "list_changed"),
          "list_changed notification present");

    rc = transport_stop(t);
    CHECK(rc == 0, "transport stopped");
    transport_destroy(t);

    int status = 0;
    waitpid(child, &status, 0);
}

/* Records only the shape of each delivered message, so the sweep can
 * run at megabyte sizes without a megabyte-sized buffer. */
struct big_collector {
    size_t count;
    size_t len;        /* length of the first message */
    int    head_ok;    /* first message starts with the expected prefix */
    int    tail_ok;    /* ...and ends with the expected suffix */
};

static void collect_big(void *vctx, const char *line, size_t len) {
    struct big_collector *c = vctx;
    if (c->count++ > 0) return;
    c->len = len;
    c->head_ok = len >= sizeof(big_prefix) - 1
              && memcmp(line, big_prefix, sizeof(big_prefix) - 1) == 0;
    c->tail_ok = len >= sizeof(big_suffix) - 1
              && memcmp(line + len - (sizeof(big_suffix) - 1),
                        big_suffix, sizeof(big_suffix) - 1) == 0;
}

/* Drive one request/response pair at the given payload size and
 * report what came back. Returns the transport_send result for the
 * second (large) request; *out is filled from the pump. */
static int drive_payload(size_t payload, struct big_collector *out,
                         int *send_errno) {
    g_payload_bytes = payload;
    pid_t child = 0;
    int port = fork_server(2, &child);
    if (port <= 0) {
        g_payload_bytes = 0;
        return -2;
    }

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/mcp", port);
    struct transport *t = transport_http_new(url);
    transport_start(t);

    const char init_msg[] =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{}}\n";
    transport_send(t, init_msg, sizeof(init_msg) - 1);
    struct collector coll = {0};
    pump_until(t, collect, &coll, &coll.count, 1, 2000);

    const char list_msg[] =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n";
    errno = 0;
    int rc = transport_send(t, list_msg, sizeof(list_msg) - 1);
    *send_errno = errno;
    if (rc == 0) {
        pump_until(t, collect_big, out, &out->count, 1, 2000);
    }

    transport_stop(t);
    transport_destroy(t);
    int status = 0;
    waitpid(child, &status, 0);
    g_payload_bytes = 0;
    return rc;
}

/* A response of any size up to the cap crosses the transport whole.
 *
 * The sizes bracket the old fixed 64 KiB SSE buffer: 32 KiB passed
 * before this test existed, everything at or above 64 KiB was
 * silently truncated. */
static void test_payload_sizes(void) {
    static const size_t sizes[] = {
        1024u,
        32u * 1024u,
        64u * 1024u,
        200u * 1024u,
        1024u * 1024u,
    };

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        struct big_collector got = {0};
        int send_errno = 0;
        int rc = drive_payload(sizes[i], &got, &send_errno);

        char msg[128];
        snprintf(msg, sizeof(msg),
                 "%zu-byte payload: send succeeded", sizes[i]);
        CHECK(rc == 0, msg);
        snprintf(msg, sizeof(msg),
                 "%zu-byte payload: exactly one message delivered",
                 sizes[i]);
        CHECK(got.count == 1, msg);
        snprintf(msg, sizeof(msg),
                 "%zu-byte payload: delivered whole (%zu of %zu bytes)",
                 sizes[i], got.len, big_msg_len(sizes[i]));
        CHECK(got.len == big_msg_len(sizes[i]), msg);
        snprintf(msg, sizeof(msg),
                 "%zu-byte payload: message intact end to end", sizes[i]);
        CHECK(got.head_ok && got.tail_ok, msg);
    }
}

/* Past the cap the request fails loudly. It must not hang, and it
 * must not deliver a truncated message that downstream would drop. */
static void test_payload_over_cap(void) {
    struct big_collector got = {0};
    int send_errno = 0;
    int rc = drive_payload(5u * 1024u * 1024u, &got, &send_errno);

    CHECK(rc < 0, "over-cap response fails the send rather than hanging");
    CHECK(send_errno == EPROTO,
          "over-cap response reports EPROTO so dispatch can answer the "
          "agent with an error");
    CHECK(got.count == 0, "no truncated message is delivered");
}

static void test_bad_url(void) {
    struct transport *t;

    t = transport_http_new("ftp://localhost:1/x");
    CHECK(t == NULL, "non-http scheme rejected");

    t = transport_http_new("http://example.com/mcp");
    CHECK(t == NULL, "non-loopback host rejected");

    t = transport_http_new("http://localhost:99999/mcp");
    CHECK(t == NULL, "out-of-range port rejected");

    t = transport_http_new("http://localhost");
    CHECK(t != NULL, "default path accepted");
    if (t) transport_destroy(t);
}

static void test_connection_refused(void) {
    /* Port 1 is privileged and reliably unbound on dev machines;
     * a connect attempt gets ECONNREFUSED. Under 🎯T7.1 the
     * transport retries refused connects with backoff up to the
     * per-call timeout — so we set a short timeout here and expect
     * the send to fail with ETIMEDOUT after at least one backoff
     * round, not immediately. */
    struct transport *t = transport_http_new("http://127.0.0.1:1/mcp");
    CHECK(t != NULL, "transport created");
    transport_http_set_call_timeout(t, 200);
    int rc = transport_start(t);
    CHECK(rc == 0, "transport_start succeeds (no connection yet)");

    const char msg[] = "{\"jsonrpc\":\"2.0\",\"id\":1,"
                       "\"method\":\"initialize\"}\n";
    errno = 0;
    rc = transport_send(t, msg, sizeof(msg) - 1);
    CHECK(rc < 0, "send returns error when server is unreachable");
    CHECK(errno == ETIMEDOUT,
          "send fails with ETIMEDOUT once the per-call deadline expires");

    transport_stop(t);
    transport_destroy(t);
}

int main(void) {
    install_watchdog();

    /* Reap children on SIGCHLD defaults; we explicitly waitpid. */
    signal(SIGPIPE, SIG_IGN);

    test_bad_url();
    test_connection_refused();
    test_round_trip();
    test_payload_sizes();
    test_payload_over_cap();

    if (fail_count > 0) {
        fprintf(stderr, "%d transport_http_test assertion(s) failed\n",
                fail_count);
        return 1;
    }
    puts("transport_http_test: ok");
    return 0;
}
