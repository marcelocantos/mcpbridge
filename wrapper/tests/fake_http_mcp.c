/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

/* fake_http_mcp — a tiny MCP HTTP server used by the wrapper's e2e
 * tests. Handles the bare minimum of MCP Streamable HTTP:
 *
 *   POST /mcp with body `initialize`  → plain JSON response + MCP-
 *                                        Session-Id header.
 *   POST /mcp with body `tools/list`  → chunked SSE with one data
 *                                        event carrying the result.
 *   POST /mcp with anything else       → chunked SSE with a generic
 *                                        JSON-RPC result echoing the
 *                                        request id.
 *
 * Options:
 *   --port P     bind 127.0.0.1:P. Required.
 *   -v           verbose (stderr).
 *
 * After `listen` succeeds, the bound port is printed to stdout so a
 * test harness can read it back when P=0. Exits on SIGTERM / SIGINT. */

#include "cJSON.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static volatile sig_atomic_t stopping = 0;
static volatile sig_atomic_t rotate_session = 0;
static int verbose = 0;

/* Current session id. Initialised on startup; SIGUSR1 rotates it,
 * which simulates an upstream restart that wiped its session table.
 * Requests bearing the previous session id then receive 400 — the
 * canonical session-loss shape — and the wrapper must transparently
 * re-initialise. */
static char current_session_id[64] = "sess-fake-1";

/* When set (via --fail-5xx-method), any request whose JSON-RPC method
 * equals this string is answered with HTTP 503 Service Unavailable —
 * the "backend is restarting / transiently unhealthy" shape. Used by
 * the per-request-failure survival e2e (Fable-5 F5). */
static char fail_5xx_method[64] = "";

static void handle_sig(int s) {
    (void)s;
    stopping = 1;
}

static void handle_rotate(int s) {
    (void)s;
    rotate_session = 1;
}

static void rotate_session_now(void) {
    static int gen = 1;
    gen++;
    snprintf(current_session_id, sizeof(current_session_id),
             "sess-fake-%d", gen);
    rotate_session = 0;
}

/* Stderr-verbose logger. Plain function to avoid the GNU-only
 * `,##__VA_ARGS__` macro idiom. */
__attribute__((format(printf, 1, 2)))
static void logv(const char *fmt, ...) {
    if (!verbose) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "fake_http_mcp: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---------------- IO helpers ---------------- */

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

/* Extract the MCP-Session-Id header value, if any, into out (cap-1
 * bytes max, NUL-terminated). out is empty if the header is absent.
 * Header parsing is line-based and case-insensitive on the name. */
static void parse_session_id(const char *hdr, size_t n,
                             char *out, size_t cap) {
    out[0] = '\0';
    const char *p = hdr;
    const char *end = hdr + n;
    const char key[] = "mcp-session-id:";
    size_t klen = sizeof(key) - 1;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (eol == NULL) break;
        if ((size_t)(eol - p) > klen
         && strncasecmp(p, key, klen) == 0) {
            const char *v = p + klen;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            const char *vend = eol;
            while (vend > v
                && (vend[-1] == '\r' || vend[-1] == ' '
                 || vend[-1] == '\t' || vend[-1] == '\n')) {
                vend--;
            }
            size_t vlen = (size_t)(vend - v);
            if (vlen >= cap) vlen = cap - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return;
        }
        p = eol + 1;
    }
}

/* Read until CRLF-CRLF; returns 0 success, -1 on error/EOF.
 * *total_out is bytes read; *hend_out is the byte index just past
 * the terminator. */
static int read_headers(int fd, char *buf, size_t cap,
                        size_t *total_out, size_t *hend_out) {
    size_t have = 0;
    for (;;) {
        if (have >= cap) return -1;
        ssize_t r = read(fd, buf + have, cap - have);
        if (r <= 0) return -1;
        have += (size_t)r;
        for (size_t i = 3; i < have; i++) {
            if (buf[i-3] == '\r' && buf[i-2] == '\n'
             && buf[i-1] == '\r' && buf[i]   == '\n') {
                *hend_out  = i + 1;
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
        if ((size_t)(eol - p) > 15
         && strncasecmp(p, "Content-Length:", 15) == 0) {
            return (int)strtol(p + 15, NULL, 10);
        }
        p = eol + 1;
    }
    return -1;
}

/* ---------------- response builders ---------------- */

static void send_initialize_response(int cfd, cJSON *req_id) {
    /* Build result object. */
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", "2025-11-25");
    cJSON *caps = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "capabilities", caps);
    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", "fake-http-mcp");
    cJSON_AddStringToObject(info, "version", "1.0.0");
    cJSON_AddItemToObject(result, "serverInfo", info);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id",
                          req_id != NULL
                            ? cJSON_Duplicate(req_id, 1)
                            : cJSON_CreateNumber(1));
    cJSON_AddItemToObject(resp, "result", result);

    char *body = cJSON_PrintUnformatted(resp);
    size_t blen = body != NULL ? strlen(body) : 0;

    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "MCP-Session-Id: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        current_session_id,
        blen);
    write_all(cfd, hdr, (size_t)n);
    if (body != NULL && blen > 0) {
        write_all(cfd, body, blen);
    }
    free(body);
    cJSON_Delete(resp);
}

/* Write one chunked data frame with the JSON in `body` wrapped as an
 * SSE `data:` event. */
static void write_sse_chunk(int cfd, const char *json_body) {
    size_t jlen = strlen(json_body);
    size_t frame_cap = jlen + 32;
    char *frame = malloc(frame_cap);
    int fn = snprintf(frame, frame_cap, "data: %s\n\n", json_body);
    char hex[32];
    int hn = snprintf(hex, sizeof(hex), "%zx\r\n", (size_t)fn);
    write_all(cfd, hex, (size_t)hn);
    write_all(cfd, frame, (size_t)fn);
    write_all(cfd, "\r\n", 2);
    free(frame);
}

static void send_sse_response(int cfd, const char *method, cJSON *req_id) {
    const char hdr[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "\r\n";
    write_all(cfd, hdr, sizeof(hdr) - 1);

    cJSON *result = cJSON_CreateObject();
    if (strcmp(method, "tools/list") == 0) {
        cJSON *tools = cJSON_CreateArray();
        cJSON_AddItemToObject(result, "tools", tools);
    } else if (strcmp(method, "prompts/list") == 0) {
        cJSON *p = cJSON_CreateArray();
        cJSON_AddItemToObject(result, "prompts", p);
    } else if (strcmp(method, "resources/list") == 0) {
        cJSON *r = cJSON_CreateArray();
        cJSON_AddItemToObject(result, "resources", r);
    } else {
        cJSON_AddStringToObject(result, "method", method);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id",
                          req_id != NULL
                            ? cJSON_Duplicate(req_id, 1)
                            : cJSON_CreateNumber(0));
    cJSON_AddItemToObject(resp, "result", result);

    char *body = cJSON_PrintUnformatted(resp);
    if (body != NULL) {
        write_sse_chunk(cfd, body);
    }
    free(body);
    cJSON_Delete(resp);

    /* Terminator. */
    write_all(cfd, "0\r\n\r\n", 5);
}

/* ---------------- connection handler ---------------- */

static void serve_one(int cfd) {
    char hbuf[4096];
    size_t total = 0, hend = 0;
    if (read_headers(cfd, hbuf, sizeof(hbuf), &total, &hend) != 0) {
        logv("headers read failed");
        return;
    }
    int clen = parse_content_length(hbuf, hend);
    if (clen < 0) clen = 0;

    char incoming_sid[64];
    parse_session_id(hbuf, hend, incoming_sid, sizeof(incoming_sid));

    char *body = malloc((size_t)clen + 1);
    int have = (int)(total - hend);
    if (have > 0) {
        memcpy(body, hbuf + hend, (size_t)have);
    }
    while (have < clen) {
        ssize_t r = read(cfd, body + have, (size_t)(clen - have));
        if (r <= 0) { free(body); return; }
        have += (int)r;
    }
    body[clen] = '\0';

    cJSON *parsed = cJSON_Parse(body);
    free(body);
    if (parsed == NULL) {
        const char bad[] = "HTTP/1.1 400 Bad Request\r\n"
                           "Content-Length: 0\r\n\r\n";
        write_all(cfd, bad, sizeof(bad) - 1);
        return;
    }

    cJSON *method_node = cJSON_GetObjectItemCaseSensitive(parsed, "method");
    cJSON *id_node     = cJSON_GetObjectItemCaseSensitive(parsed, "id");
    const char *method = (method_node != NULL && cJSON_IsString(method_node))
                       ? method_node->valuestring : "";

    logv("request: method=%s", method);

    /* Forced per-request 5xx: simulate a transiently-unhealthy backend
     * for a chosen method. The transport stays usable for the next
     * request, so the wrapper must surface an error for THIS call and
     * survive rather than tearing down the whole session (Fable-5 F5). */
    if (fail_5xx_method[0] != '\0' && strcmp(method, fail_5xx_method) == 0) {
        logv("response: 503 (forced for method %s)", method);
        const char err_body[] = "Service Unavailable";
        char hdr[256];
        int hn = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 503 Service Unavailable\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n",
            sizeof(err_body) - 1);
        write_all(cfd, hdr, (size_t)hn);
        write_all(cfd, err_body, sizeof(err_body) - 1);
        cJSON_Delete(parsed);
        return;
    }

    /* Reject any non-initialize request whose session id doesn't
     * match the current one. This is the canonical session-loss
     * shape — what mcp-go's session middleware does after a restart.
     * SIGUSR1 to this process rotates current_session_id, which from
     * the wrapper's perspective is indistinguishable from a real
     * upstream restart. */
    if (strcmp(method, "initialize") != 0
     && incoming_sid[0] != '\0'
     && strcmp(incoming_sid, current_session_id) != 0) {
        logv("response: 400 (session id %s does not match current %s)",
             incoming_sid, current_session_id);
        const char err_body[] = "Bad Request: Invalid session ID";
        char hdr[256];
        int hn = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n",
            sizeof(err_body) - 1);
        write_all(cfd, hdr, (size_t)hn);
        write_all(cfd, err_body, sizeof(err_body) - 1);
        cJSON_Delete(parsed);
        return;
    }

    if (strcmp(method, "initialize") == 0) {
        logv("response: initialize (200 OK + session id %s)",
             current_session_id);
        send_initialize_response(cfd, id_node);
    } else if (method[0] == '\0') {
        logv("response: no method (202)");
        const char accepted[] = "HTTP/1.1 202 Accepted\r\n"
                                "Content-Length: 0\r\n\r\n";
        write_all(cfd, accepted, sizeof(accepted) - 1);
    } else if (strncmp(method, "notifications/", 14) == 0) {
        logv("response: %s (202)", method);
        const char accepted[] = "HTTP/1.1 202 Accepted\r\n"
                                "Content-Length: 0\r\n\r\n";
        write_all(cfd, accepted, sizeof(accepted) - 1);
    } else {
        logv("response: %s (200 SSE)", method);
        send_sse_response(cfd, method, id_node);
    }

    cJSON_Delete(parsed);
}

/* ---------------- main ---------------- */

static int parse_port(const char *s) {
    char *endp = NULL;
    long n = strtol(s, &endp, 10);
    if (endp == s || n < 0 || n > 65535) return -1;
    return (int)n;
}

int main(int argc, char **argv) {
    int requested_port = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            requested_port = parse_port(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
            continue;
        }
        if (strcmp(argv[i], "--fail-5xx-method") == 0 && i + 1 < argc) {
            snprintf(fail_5xx_method, sizeof(fail_5xx_method), "%s",
                     argv[++i]);
            continue;
        }
        fprintf(stderr, "fake_http_mcp: unknown arg: %s\n", argv[i]);
        return 2;
    }
    if (requested_port < 0) {
        fprintf(stderr, "fake_http_mcp: --port is required\n");
        return 2;
    }

    struct sigaction sa = {0};
    sa.sa_handler = handle_sig;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    struct sigaction sr = {0};
    sr.sa_handler = handle_rotate;
    sigaction(SIGUSR1, &sr, NULL);
    struct sigaction ign = {0};
    ign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ign, NULL);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001); /* 127.0.0.1 */
    addr.sin_port        = htons((uint16_t)requested_port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind"); return 1;
    }
    if (listen(srv, 4) != 0) { perror("listen"); return 1; }

    socklen_t alen = sizeof(addr);
    getsockname(srv, (struct sockaddr *)&addr, &alen);
    int bound = ntohs(addr.sin_port);
    printf("%d\n", bound);
    fflush(stdout);
    logv("listening on 127.0.0.1:%d", bound);

    while (!stopping) {
        if (rotate_session) {
            rotate_session_now();
            logv("session id rotated to %s (SIGUSR1)",
                 current_session_id);
        }
        int cfd = accept(srv, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (stopping) break;
            perror("accept");
            continue;
        }
        serve_one(cfd);
        close(cfd);
    }
    close(srv);
    logv("stopped");
    return 0;
}
