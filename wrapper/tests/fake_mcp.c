/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

/* fake_mcp — a tiny MCP server used by the wrapper's e2e tests.
 *
 * Handles only the bare minimum: initialize, notifications/initialized,
 * tools/list, tools/call, and ping. Everything else gets a JSON-RPC
 * method-not-found error response. Exits cleanly on stdin EOF.
 *
 * This intentionally uses cJSON (which is already vendored and linked
 * into all the test binaries) rather than hand-rolling a JSON parser,
 * because the fake must behave like a real MCP server — including
 * echoing back whatever JSON-RPC id the client sent, which is tedious
 * to do by string manipulation and easy to get subtly wrong. */

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Write one line to stdout, terminated with '\n', and flush. */
static void emit(cJSON *obj) {
    char *out = cJSON_PrintUnformatted(obj);
    if (out == NULL) {
        return;
    }
    fputs(out, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    cJSON_free(out);
}

/* Build a response envelope with the given id and a result object.
 * Takes ownership of `result`. Returns a new cJSON object the caller
 * must cJSON_Delete. */
static cJSON *make_response(const cJSON *id, cJSON *result) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id != NULL) {
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    }
    cJSON_AddItemToObject(resp, "result", result);
    return resp;
}

static cJSON *make_error(const cJSON *id, int code, const char *message) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id != NULL) {
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    }
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(resp, "error", err);
    return resp;
}

/* Handlers return a freshly-allocated response object, or NULL if
 * the incoming message is a notification (no response expected). */

static cJSON *handle_initialize(const cJSON *id) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
    cJSON *server_info = cJSON_CreateObject();
    cJSON_AddStringToObject(server_info, "name", "fake_mcp");
    cJSON_AddStringToObject(server_info, "version", "1.0.0");
    cJSON_AddItemToObject(result, "serverInfo", server_info);
    cJSON *caps = cJSON_CreateObject();
    cJSON *tools_cap = cJSON_CreateObject();
    cJSON_AddBoolToObject(tools_cap, "listChanged", 0);
    cJSON_AddItemToObject(caps, "tools", tools_cap);
    cJSON_AddItemToObject(result, "capabilities", caps);
    return make_response(id, result);
}

static cJSON *handle_tools_list(const cJSON *id) {
    cJSON *result = cJSON_CreateObject();
    cJSON *tools = cJSON_CreateArray();
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", "echo");
    cJSON_AddStringToObject(tool, "description", "Echo a string back.");
    cJSON *schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    cJSON_AddItemToObject(tool, "inputSchema", schema);
    cJSON_AddItemToArray(tools, tool);
    cJSON_AddItemToObject(result, "tools", tools);
    return make_response(id, result);
}

/* FAKE_MCP_CALL_DELAY_MS makes tools/call take a while, so a test can
 * land a reload while a request is genuinely in flight. Blocking the
 * read loop is the point: a real single-threaded MCP server busy in a
 * tool call is not reading stdin either. */
static void delay_if_asked(void) {
    const char *ms = getenv("FAKE_MCP_CALL_DELAY_MS");
    if (ms == NULL) {
        return;
    }
    long v = strtol(ms, NULL, 10);
    if (v <= 0) {
        return;
    }
    struct timespec ts;
    ts.tv_sec  = v / 1000;
    ts.tv_nsec = (v % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static cJSON *handle_tools_call(const cJSON *id) {
    delay_if_asked();
    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *chunk = cJSON_CreateObject();
    cJSON_AddStringToObject(chunk, "type", "text");
    cJSON_AddStringToObject(chunk, "text", "ok");
    cJSON_AddItemToArray(content, chunk);
    cJSON_AddItemToObject(result, "content", content);
    cJSON_AddBoolToObject(result, "isError", 0);
    return make_response(id, result);
}

static cJSON *handle_ping(const cJSON *id) {
    cJSON *result = cJSON_CreateObject();
    return make_response(id, result);
}

static void handle_line(const char *line, size_t len) {
    cJSON *root = cJSON_ParseWithLength(line, len);
    if (root == NULL || !cJSON_IsObject(root)) {
        /* Malformed. We can't respond (no id) — silently drop. */
        cJSON_Delete(root);
        return;
    }
    const cJSON *id     = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    if (method == NULL || !cJSON_IsString(method)) {
        /* Probably a response — we don't send requests, so ignore. */
        cJSON_Delete(root);
        return;
    }

    const char *m = method->valuestring;
    cJSON *resp = NULL;

    if (strcmp(m, "initialize") == 0) {
        resp = handle_initialize(id);
    } else if (strcmp(m, "notifications/initialized") == 0) {
        /* Notification — no response. */
    } else if (strcmp(m, "tools/list") == 0) {
        resp = handle_tools_list(id);
    } else if (strcmp(m, "tools/call") == 0) {
        resp = handle_tools_call(id);
    } else if (strcmp(m, "ping") == 0) {
        resp = handle_ping(id);
    } else {
        if (id != NULL) {
            resp = make_error(id, -32601, "method not found");
        }
    }

    if (resp != NULL) {
        emit(resp);
        cJSON_Delete(resp);
    }
    cJSON_Delete(root);
}

int main(void) {
    /* Line-buffered stdio so our writes flush promptly — critical
     * for the wrapper event loop to see our responses before the
     * test pipeline closes stdin. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    size_t cap = 4096;
    size_t len = 0;
    char  *buf = malloc(cap);
    if (buf == NULL) {
        return 1;
    }

    char chunk[4096];
    for (;;) {
        ssize_t r = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (r == 0) {
            break;
        }
        if (r < 0) {
            free(buf);
            return 1;
        }
        /* Append to line buffer. */
        if (len + (size_t)r + 1 > cap) {
            while (len + (size_t)r + 1 > cap) {
                cap *= 2;
            }
            char *nb = realloc(buf, cap);
            if (nb == NULL) {
                free(buf);
                return 1;
            }
            buf = nb;
        }
        memcpy(buf + len, chunk, (size_t)r);
        len += (size_t)r;

        /* Extract complete lines. */
        for (;;) {
            char *nl = memchr(buf, '\n', len);
            if (nl == NULL) {
                break;
            }
            size_t line_len = (size_t)(nl - buf);
            handle_line(buf, line_len);
            size_t consumed = line_len + 1;
            memmove(buf, buf + consumed, len - consumed);
            len -= consumed;
        }
    }

    free(buf);
    return 0;
}
