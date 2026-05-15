/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

/* Unit tests for wrapper/src/mcp.c. Links directly against the module
 * sources and cJSON; no framework. Each failing assertion increments
 * fail_count and the test exits non-zero at the end. */

#include "../src/mcp.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail_count = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);  \
            fail_count++;                                                  \
        }                                                                  \
    } while (0)

#define STR_AND_LEN(s) (s), (sizeof(s) - 1)

/* ---------- mcp_id ---------- */

static void test_id_equality(void) {
    struct mcp_id a = {MCP_ID_INT, {.i = 42}};
    struct mcp_id b = {MCP_ID_INT, {.i = 42}};
    struct mcp_id c = {MCP_ID_INT, {.i = 43}};
    CHECK(mcp_id_eq(&a, &b), "equal int ids");
    CHECK(!mcp_id_eq(&a, &c), "different int ids");

    struct mcp_id s1 = {MCP_ID_STRING, {.s = (char *)"hello"}};
    struct mcp_id s2 = {MCP_ID_STRING, {.s = (char *)"hello"}};
    struct mcp_id s3 = {MCP_ID_STRING, {.s = (char *)"world"}};
    CHECK(mcp_id_eq(&s1, &s2), "equal string ids");
    CHECK(!mcp_id_eq(&s1, &s3), "different string ids");

    CHECK(!mcp_id_eq(&a, &s1), "different tags never equal");

    struct mcp_id none1 = {MCP_ID_NONE, {.i = 0}};
    struct mcp_id none2 = {MCP_ID_NONE, {.i = 0}};
    CHECK(mcp_id_eq(&none1, &none2), "NONE equals NONE");
    CHECK(!mcp_id_eq(&none1, &a), "NONE does not equal INT");
}

/* ---------- mcp_msg_parse ---------- */

static void test_parse_initialize_request(void) {
    const char *bytes =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"protocolVersion\":\"2024-11-05\","
        "\"capabilities\":{},\"clientInfo\":{\"name\":\"x\",\"version\":\"y\"}}}";

    struct mcp_msg m = {0};
    int rc = mcp_msg_parse(bytes, strlen(bytes), &m);
    CHECK(rc == MCP_PARSE_OK, "initialize request parses");
    CHECK(m.kind == MCP_KIND_REQUEST, "kind is REQUEST");
    CHECK(m.id.tag == MCP_ID_INT, "id is int");
    CHECK(m.id.v.i == 1, "id value is 1");
    CHECK(m.method != NULL && strcmp(m.method, "initialize") == 0,
          "method is initialize");
    CHECK(m.raw != NULL && m.raw_len == strlen(bytes), "raw retained");
    CHECK(mcp_msg_is_request(&m, "initialize"), "is_request helper works");
    CHECK(!mcp_msg_is_request(&m, "other"), "is_request rejects other method");
    mcp_msg_free(&m);
}

static void test_parse_initialize_response(void) {
    const char *bytes =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":"
        "\"2024-11-05\",\"serverInfo\":{\"name\":\"mnemo\",\"version\":\"0.4.2\"},"
        "\"capabilities\":{\"tools\":{\"listChanged\":true}}}}";

    struct mcp_msg m = {0};
    int rc = mcp_msg_parse(bytes, strlen(bytes), &m);
    CHECK(rc == MCP_PARSE_OK, "initialize response parses");
    CHECK(m.kind == MCP_KIND_RESPONSE, "kind is RESPONSE");
    CHECK(m.id.tag == MCP_ID_INT && m.id.v.i == 1, "response id");
    CHECK(m.method == NULL, "response has no method");
    mcp_msg_free(&m);
}

static void test_parse_tools_list_request(void) {
    const char *bytes =
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\"}";

    struct mcp_msg m = {0};
    CHECK(mcp_msg_parse(bytes, strlen(bytes), &m) == MCP_PARSE_OK, "parse");
    CHECK(m.kind == MCP_KIND_REQUEST, "tools/list is a request");
    CHECK(mcp_msg_is_request(&m, "tools/list"), "is_request tools/list");
    mcp_msg_free(&m);
}

static void test_parse_tools_call_request(void) {
    const char *bytes =
        "{\"jsonrpc\":\"2.0\",\"id\":\"call-abc\",\"method\":\"tools/call\","
        "\"params\":{\"name\":\"foo\",\"arguments\":{\"x\":1}}}";

    struct mcp_msg m = {0};
    CHECK(mcp_msg_parse(bytes, strlen(bytes), &m) == MCP_PARSE_OK, "parse");
    CHECK(m.kind == MCP_KIND_REQUEST, "tools/call is a request");
    CHECK(m.id.tag == MCP_ID_STRING, "id is string");
    CHECK(strcmp(m.id.v.s, "call-abc") == 0, "id value matches");
    CHECK(mcp_msg_is_request(&m, "tools/call"), "is_request tools/call");
    mcp_msg_free(&m);
}

static void test_parse_notification_initialized(void) {
    const char *bytes =
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";

    struct mcp_msg m = {0};
    CHECK(mcp_msg_parse(bytes, strlen(bytes), &m) == MCP_PARSE_OK, "parse");
    CHECK(m.kind == MCP_KIND_NOTIFICATION, "is notification");
    CHECK(m.id.tag == MCP_ID_NONE, "no id");
    CHECK(mcp_msg_is_notification(&m, "notifications/initialized"),
          "is_notification helper works");
    mcp_msg_free(&m);
}

static void test_parse_rejects_malformed_json(void) {
    const char *bytes = "{not json";
    struct mcp_msg m = {0};
    int rc = mcp_msg_parse(bytes, strlen(bytes), &m);
    CHECK(rc == MCP_PARSE_ERR_JSON, "malformed JSON rejected");
    CHECK(m.raw == NULL, "no raw retained on failure");
}

static void test_parse_rejects_non_object(void) {
    const char *bytes = "[1,2,3]";
    struct mcp_msg m = {0};
    int rc = mcp_msg_parse(bytes, strlen(bytes), &m);
    CHECK(rc == MCP_PARSE_ERR_NOT_OBJECT, "array top-level rejected");
}

static void test_parse_rejects_float_id(void) {
    const char *bytes =
        "{\"jsonrpc\":\"2.0\",\"id\":1.5,\"method\":\"x\"}";
    struct mcp_msg m = {0};
    int rc = mcp_msg_parse(bytes, strlen(bytes), &m);
    CHECK(rc == MCP_PARSE_ERR_BAD_ID, "float id rejected");
}

static void test_parse_rejects_non_string_method(void) {
    const char *bytes =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":42}";
    struct mcp_msg m = {0};
    int rc = mcp_msg_parse(bytes, strlen(bytes), &m);
    CHECK(rc == MCP_PARSE_ERR_BAD_METHOD, "numeric method rejected");
}

static void test_parse_rejects_incoherent(void) {
    /* method present but no id, and also no result/error — wait, that
     * is valid (it's a notification). Truly incoherent is: id with no
     * method and no result/error. */
    const char *bytes = "{\"jsonrpc\":\"2.0\",\"id\":1}";
    struct mcp_msg m = {0};
    int rc = mcp_msg_parse(bytes, strlen(bytes), &m);
    CHECK(rc == MCP_PARSE_ERR_INCOHERENT, "id without method/result rejected");
}

/* ---------- mcp_reader ---------- */

static void test_reader_basic(void) {
    struct mcp_reader r;
    mcp_reader_init(&r, 0);

    const char *input = "one\ntwo\nthree\n";
    mcp_reader_feed(&r, input, strlen(input));

    const char *line;
    size_t len;

    int rc = mcp_reader_pop(&r, &line, &len);
    CHECK(rc == MCP_READER_OK, "pop 1 OK");
    CHECK(len == 3 && memcmp(line, "one", 3) == 0, "line 1 is 'one'");

    rc = mcp_reader_pop(&r, &line, &len);
    CHECK(rc == MCP_READER_OK, "pop 2 OK");
    CHECK(len == 3 && memcmp(line, "two", 3) == 0, "line 2 is 'two'");

    rc = mcp_reader_pop(&r, &line, &len);
    CHECK(rc == MCP_READER_OK, "pop 3 OK");
    CHECK(len == 5 && memcmp(line, "three", 5) == 0, "line 3 is 'three'");

    rc = mcp_reader_pop(&r, &line, &len);
    CHECK(rc == MCP_READER_EMPTY, "pop 4 EMPTY");

    mcp_reader_free(&r);
}

static void test_reader_partial(void) {
    struct mcp_reader r;
    mcp_reader_init(&r, 0);

    mcp_reader_feed(&r, "he", 2);

    const char *line;
    size_t len;
    CHECK(mcp_reader_pop(&r, &line, &len) == MCP_READER_EMPTY,
          "no line after partial feed");

    mcp_reader_feed(&r, "llo", 3);
    CHECK(mcp_reader_pop(&r, &line, &len) == MCP_READER_EMPTY,
          "still no line without newline");

    mcp_reader_feed(&r, "\n", 1);
    CHECK(mcp_reader_pop(&r, &line, &len) == MCP_READER_OK, "line pops");
    CHECK(len == 5 && memcmp(line, "hello", 5) == 0, "line is 'hello'");

    mcp_reader_free(&r);
}

static void test_reader_empty_lines(void) {
    struct mcp_reader r;
    mcp_reader_init(&r, 0);

    mcp_reader_feed(&r, "\n\na\n", 4);

    const char *line;
    size_t len;

    CHECK(mcp_reader_pop(&r, &line, &len) == MCP_READER_OK, "pop1");
    CHECK(len == 0, "empty line 1");

    CHECK(mcp_reader_pop(&r, &line, &len) == MCP_READER_OK, "pop2");
    CHECK(len == 0, "empty line 2");

    CHECK(mcp_reader_pop(&r, &line, &len) == MCP_READER_OK, "pop3");
    CHECK(len == 1 && line[0] == 'a', "line 3 is 'a'");

    mcp_reader_free(&r);
}

static void test_reader_overflow(void) {
    struct mcp_reader r;
    mcp_reader_init(&r, 16); /* tiny cap */

    /* One oversized line followed by a normal one. */
    const char *big = "this line is much longer than sixteen bytes\n";
    mcp_reader_feed(&r, big, strlen(big));

    const char *line;
    size_t len;
    int rc = mcp_reader_pop(&r, &line, &len);
    CHECK(rc == MCP_READER_TOO_LONG, "oversized line reported");

    /* Next call should go back to normal. */
    mcp_reader_feed(&r, "ok\n", 3);
    rc = mcp_reader_pop(&r, &line, &len);
    CHECK(rc == MCP_READER_OK, "subsequent normal line pops");
    CHECK(len == 2 && memcmp(line, "ok", 2) == 0, "it's 'ok'");

    mcp_reader_free(&r);
}

static void test_reader_mixed_short_and_oversized(void) {
    /* A single feed containing short lines and an oversized line
     * must preserve the short lines. The drain-lines-first
     * semantics surface TOO_LONG after both short lines have
     * been popped — this is documented in mcp.h. */
    struct mcp_reader r;
    mcp_reader_init(&r, 8);

    const char *input = "ok\nwaytoolongforeight\nend\n";
    mcp_reader_feed(&r, input, strlen(input));

    const char *line;
    size_t len;

    int rc = mcp_reader_pop(&r, &line, &len);
    CHECK(rc == MCP_READER_OK, "first short line pops");
    CHECK(len == 2 && memcmp(line, "ok", 2) == 0, "it's 'ok'");

    rc = mcp_reader_pop(&r, &line, &len);
    CHECK(rc == MCP_READER_OK, "second short line pops (drain-first)");
    CHECK(len == 3 && memcmp(line, "end", 3) == 0, "it's 'end'");

    rc = mcp_reader_pop(&r, &line, &len);
    CHECK(rc == MCP_READER_TOO_LONG, "overflow reported after drain");

    rc = mcp_reader_pop(&r, &line, &len);
    CHECK(rc == MCP_READER_EMPTY, "empty after final drain");

    mcp_reader_free(&r);
}

static void test_reader_overflow_preserves_prior_line(void) {
    /* When an oversized line arrives in a separate feed after a
     * successfully buffered line, the prior line must survive. */
    struct mcp_reader r;
    mcp_reader_init(&r, 8);

    mcp_reader_feed(&r, "ok\n", 3);
    mcp_reader_feed(&r, "waytoolongforeight\n", 19);

    const char *line;
    size_t len;

    int rc = mcp_reader_pop(&r, &line, &len);
    CHECK(rc == MCP_READER_OK, "prior line survives overflow");
    CHECK(len == 2 && memcmp(line, "ok", 2) == 0, "it's 'ok'");

    rc = mcp_reader_pop(&r, &line, &len);
    CHECK(rc == MCP_READER_TOO_LONG, "then overflow");

    mcp_reader_free(&r);
}

static void test_reader_max_length_exact(void) {
    /* A line exactly at the max length must succeed. */
    struct mcp_reader r;
    mcp_reader_init(&r, 5);

    mcp_reader_feed(&r, "exact\n", 6);

    const char *line;
    size_t len;
    int rc = mcp_reader_pop(&r, &line, &len);
    CHECK(rc == MCP_READER_OK, "5-char line at max=5 pops");
    CHECK(len == 5 && memcmp(line, "exact", 5) == 0, "content matches");

    mcp_reader_free(&r);
}

static void test_reader_growth(void) {
    struct mcp_reader r;
    mcp_reader_init(&r, 1024 * 1024); /* 1 MiB cap */

    /* Build a large line just under the cap. */
    size_t n = 600 * 1024;
    char *payload = malloc(n + 1);
    for (size_t i = 0; i < n; i++) {
        payload[i] = 'x';
    }
    payload[n] = '\n';

    mcp_reader_feed(&r, payload, n + 1);

    const char *line;
    size_t len;
    int rc = mcp_reader_pop(&r, &line, &len);
    CHECK(rc == MCP_READER_OK, "600KB line pops");
    CHECK(len == n, "600KB line length matches");

    free(payload);
    mcp_reader_free(&r);
}

/* ---------- mcp_build_list_changed ---------- */

static void check_list_changed_kind(const char *kind, const char *expected_method) {
    size_t len = 0;
    char *msg = mcp_build_list_changed(kind, &len);
    CHECK(msg != NULL, "built");
    CHECK(len > 0, "non-empty");
    CHECK(msg[len - 1] == '\n', "newline-terminated");

    /* Must be parseable as an MCP notification with the right method. */
    struct mcp_msg parsed = {0};
    int rc = mcp_msg_parse(msg, len - 1, &parsed); /* strip trailing \n */
    CHECK(rc == MCP_PARSE_OK, "emitted notification parses");
    CHECK(parsed.kind == MCP_KIND_NOTIFICATION, "is notification");
    CHECK(mcp_msg_is_notification(&parsed, expected_method),
          "method matches");
    mcp_msg_free(&parsed);
    free(msg);
}

static void test_build_list_changed(void) {
    check_list_changed_kind("tools",     "notifications/tools/list_changed");
    check_list_changed_kind("prompts",   "notifications/prompts/list_changed");
    check_list_changed_kind("resources", "notifications/resources/list_changed");

    /* Unknown kinds return NULL. */
    CHECK(mcp_build_list_changed("nope", NULL) == NULL,
          "unknown kind rejected");
    CHECK(mcp_build_list_changed(NULL, NULL) == NULL,
          "NULL kind rejected");
}

/* ---------- mcp_build_error_response ---------- */

static void test_build_error_response_no_data(void) {
    struct mcp_id id = {MCP_ID_INT, {.i = 5}};
    size_t len = 0;
    char *buf = mcp_build_error_response(&id, -32001, "oops", NULL, &len);
    CHECK(buf != NULL, "basic error response built");
    CHECK(len > 0, "non-empty");
    CHECK(buf[len - 1] == '\n', "newline-terminated");

    struct mcp_msg m = {0};
    int rc = mcp_msg_parse(buf, len - 1, &m);
    CHECK(rc == MCP_PARSE_OK, "parses");
    CHECK(m.kind == MCP_KIND_RESPONSE, "is response");
    CHECK(m.id.tag == MCP_ID_INT && m.id.v.i == 5, "id round-trips");
    CHECK(m.is_error, "is error envelope");
    mcp_msg_free(&m);

    /* No "data" field should appear. */
    CHECK(strstr(buf, "\"data\"") == NULL, "no data field when NULL passed");
    free(buf);
}

static void test_build_error_response_with_data(void) {
    struct mcp_id id = {MCP_ID_STRING, {.s = (char *)"req-99"}};

    /* Build the data object the same way dispatch.c does. */
    cJSON *backend = cJSON_CreateObject();
    cJSON_AddStringToObject(backend, "name", "spyder");
    cJSON_AddStringToObject(backend, "url", "http://localhost:3030/mcp");
    cJSON *data = cJSON_CreateObject();
    cJSON_AddItemToObject(data, "backend", backend);

    size_t len = 0;
    char *buf = mcp_build_error_response(
        &id, -32001,
        "mcpbridge: upstream unreachable past timeout",
        data, /* ownership transferred; do NOT free data after this */
        &len);

    CHECK(buf != NULL, "error response with data built");
    CHECK(len > 0, "non-empty");
    CHECK(buf[len - 1] == '\n', "newline-terminated");

    /* Parse and verify the outer envelope. */
    struct mcp_msg m = {0};
    int rc = mcp_msg_parse(buf, len - 1, &m);
    CHECK(rc == MCP_PARSE_OK, "parses");
    CHECK(m.kind == MCP_KIND_RESPONSE, "is response");
    CHECK(m.id.tag == MCP_ID_STRING && strcmp(m.id.v.s, "req-99") == 0,
          "string id round-trips");
    CHECK(m.is_error, "is error envelope");
    mcp_msg_free(&m);

    /* Deep-check data.backend.{name,url} via cJSON re-parse. */
    cJSON *root = cJSON_ParseWithLength(buf, len - 1);
    CHECK(root != NULL, "re-parses as JSON");
    if (root != NULL) {
        cJSON *err  = cJSON_GetObjectItemCaseSensitive(root, "error");
        cJSON *dobj = (err != NULL)
            ? cJSON_GetObjectItemCaseSensitive(err, "data") : NULL;
        cJSON *bobj = (dobj != NULL)
            ? cJSON_GetObjectItemCaseSensitive(dobj, "backend") : NULL;

        CHECK(dobj != NULL, "error.data present");
        CHECK(bobj != NULL, "error.data.backend present");
        if (bobj != NULL) {
            cJSON *name = cJSON_GetObjectItemCaseSensitive(bobj, "name");
            cJSON *url  = cJSON_GetObjectItemCaseSensitive(bobj, "url");
            CHECK(cJSON_IsString(name) &&
                  strcmp(name->valuestring, "spyder") == 0,
                  "error.data.backend.name == \"spyder\"");
            CHECK(cJSON_IsString(url) &&
                  strcmp(url->valuestring, "http://localhost:3030/mcp") == 0,
                  "error.data.backend.url matches");
        }
        cJSON_Delete(root);
    }
    free(buf);
}

static void test_build_error_response_none_id_frees_data(void) {
    /* Passing MCP_ID_NONE must return NULL and free the data object
     * (valgrind would catch a leak if it doesn't). */
    struct mcp_id id = {MCP_ID_NONE, {.i = 0}};
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "x", "y");
    char *buf = mcp_build_error_response(&id, -32001, "msg", data, NULL);
    CHECK(buf == NULL, "NONE id returns NULL");
    /* data is now owned by the function and has been freed — no free() here */
}

int main(void) {
    test_id_equality();
    test_parse_initialize_request();
    test_parse_initialize_response();
    test_parse_tools_list_request();
    test_parse_tools_call_request();
    test_parse_notification_initialized();
    test_parse_rejects_malformed_json();
    test_parse_rejects_non_object();
    test_parse_rejects_float_id();
    test_parse_rejects_non_string_method();
    test_parse_rejects_incoherent();
    test_reader_basic();
    test_reader_partial();
    test_reader_empty_lines();
    test_reader_overflow();
    test_reader_mixed_short_and_oversized();
    test_reader_overflow_preserves_prior_line();
    test_reader_max_length_exact();
    test_reader_growth();
    test_build_list_changed();
    test_build_error_response_no_data();
    test_build_error_response_with_data();
    test_build_error_response_none_id_frees_data();

    if (fail_count > 0) {
        fprintf(stderr, "%d mcp_test assertion(s) failed\n", fail_count);
        return 1;
    }
    puts("mcp_test: ok");
    return 0;
}
