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

/* ---------- T24 / T28 parse-skip contract ----------
 *
 * The hot path must classify without building a cJSON tree, but it
 * must not change the contract. Three traps the scanner has to match:
 * first-wins on duplicate keys, escaped id/method fallback, and
 * skip-must-validate (a skipped value is still a JSON value). */

static uint64_t cjson_allocs;

static void *counting_malloc(size_t n) {
    cjson_allocs++;
    return malloc(n);
}

static void counting_free(void *p) {
    free(p);
}

/* Reference classifier: today's cJSON path, copied into the test so
 * the scanner cannot silently become the only implementation. */
static int ref_parse(const char *bytes, size_t len, struct mcp_msg *out) {
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

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (id != NULL) {
        if (cJSON_IsString(id) && id->valuestring != NULL) {
            out->id.tag = MCP_ID_STRING;
            out->id.v.s = strdup(id->valuestring);
        } else if (cJSON_IsNumber(id)) {
            double d = id->valuedouble;
            if (d != (double)(int64_t)d) {
                cJSON_Delete(root);
                return MCP_PARSE_ERR_BAD_ID;
            }
            out->id.tag = MCP_ID_INT;
            out->id.v.i = (int64_t)d;
        } else {
            cJSON_Delete(root);
            return MCP_PARSE_ERR_BAD_ID;
        }
    }

    const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    if (method != NULL) {
        if (!cJSON_IsString(method) || method->valuestring == NULL) {
            mcp_id_free(&out->id);
            cJSON_Delete(root);
            return MCP_PARSE_ERR_BAD_METHOD;
        }
        out->method = strdup(method->valuestring);
    }

    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    const cJSON *error  = cJSON_GetObjectItemCaseSensitive(root, "error");

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
    cJSON_Delete(root);
    return MCP_PARSE_OK;
}

static void test_parse_skip_must_validate(void) {
    /* A skipper that only brace-counts would accept these. cJSON
     * rejects them. The scanner must too — including the value of a
     * duplicate key it is otherwise ignoring (first-wins). */
    static const char *const bad[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"broken\":}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":[1,]}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":[}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"a\":true,}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\"unterminated}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\"bad\\xescape\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":tru}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":true,\"result\":{\"broken\":}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"x\",\"params\":{\"a\":[1,2,{\"b\":}]}}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        struct mcp_msg m = {0};
        int rc = mcp_msg_parse(bad[i], strlen(bad[i]), &m);
        CHECK(rc == MCP_PARSE_ERR_JSON, "skip-must-validate: malformed rejected");
        CHECK(m.raw == NULL, "skip-must-validate: no raw on reject");
        mcp_msg_free(&m);
    }
}

static void test_parse_first_wins_duplicate_keys(void) {
    struct mcp_msg m = {0};
    const char *dup_id =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"id\":99,\"method\":\"initialize\"}";
    CHECK(mcp_msg_parse(dup_id, strlen(dup_id), &m) == MCP_PARSE_OK,
          "dup id parses");
    CHECK(m.kind == MCP_KIND_REQUEST, "dup id is still a request");
    CHECK(m.id.tag == MCP_ID_INT && m.id.v.i == 1, "first id wins");
    mcp_msg_free(&m);

    const char *dup_method =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\","
        "\"method\":\"tools/call\"}";
    CHECK(mcp_msg_parse(dup_method, strlen(dup_method), &m) == MCP_PARSE_OK,
          "dup method parses");
    CHECK(m.method != NULL && strcmp(m.method, "tools/list") == 0,
          "first method wins");
    mcp_msg_free(&m);

    const char *dup_result =
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":true,\"result\":false}";
    CHECK(mcp_msg_parse(dup_result, strlen(dup_result), &m) == MCP_PARSE_OK,
          "dup result parses");
    CHECK(m.kind == MCP_KIND_RESPONSE && !m.is_error, "first result, not error");
    mcp_msg_free(&m);
}

static void test_parse_escaped_id_method_fallback(void) {
    /* Escapes in id/method must not be compared raw. Fallback to
     * cJSON (which unescapes) is the contracted path. */
    struct mcp_msg m = {0};
    const char *esc_id =
        "{\"jsonrpc\":\"2.0\",\"id\":\"a\\tb\",\"method\":\"tools/call\"}";
    CHECK(mcp_msg_parse(esc_id, strlen(esc_id), &m) == MCP_PARSE_OK,
          "escaped id parses");
    CHECK(m.id.tag == MCP_ID_STRING && m.id.v.s != NULL, "escaped id is string");
    CHECK(strcmp(m.id.v.s, "a\tb") == 0, "escaped id is unescaped");
    mcp_msg_free(&m);

    const char *esc_method =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools\\/call\"}";
    CHECK(mcp_msg_parse(esc_method, strlen(esc_method), &m) == MCP_PARSE_OK,
          "escaped method parses");
    CHECK(m.method != NULL && strcmp(m.method, "tools/call") == 0,
          "escaped method is unescaped");
    mcp_msg_free(&m);
}

static void test_parse_hot_path_no_cjson_tree(void) {
    /* A 64 KiB escape-free tool result. The wrapper only reads the
     * top-level id and the presence of result. Building a cJSON tree
     * for the payload is the cost T24/T28 removes. */
    const char *prefix =
        "{\"jsonrpc\":\"2.0\",\"id\":4242,\"result\":{\"content\":"
        "[{\"type\":\"text\",\"text\":\"";
    const char *suffix = "\"}],\"isError\":false}}";
    const size_t payload = 64u * 1024u;
    size_t plen = strlen(prefix), slen = strlen(suffix);
    size_t len = plen + payload + slen;
    char *buf = malloc(len);
    CHECK(buf != NULL, "payload alloc");
    if (buf == NULL) {
        return;
    }
    memcpy(buf, prefix, plen);
    memset(buf + plen, 'a', payload);
    memcpy(buf + plen + payload, suffix, slen);

    cJSON_Hooks hooks = {counting_malloc, counting_free};
    cJSON_InitHooks(&hooks);
    cjson_allocs = 0;
    struct mcp_msg m = {0};
    int rc = mcp_msg_parse(buf, len, &m);
    uint64_t n = cjson_allocs;
    cJSON_InitHooks(NULL);

    CHECK(rc == MCP_PARSE_OK, "large result parses");
    CHECK(m.kind == MCP_KIND_RESPONSE, "large result is a response");
    CHECK(m.id.tag == MCP_ID_INT && m.id.v.i == 4242, "large result id");
    CHECK(n == 0, "hot path does not materialise a cJSON tree");
    mcp_msg_free(&m);
    free(buf);
}

static int msgs_agree(const struct mcp_msg *a, const struct mcp_msg *b) {
    if (a->kind != b->kind) {
        return 0;
    }
    if (a->is_error != b->is_error) {
        return 0;
    }
    if (!mcp_id_eq(&a->id, &b->id)) {
        return 0;
    }
    if (a->method == NULL || b->method == NULL) {
        return a->method == b->method;
    }
    return strcmp(a->method, b->method) == 0;
}

static void test_parse_scan_agrees_with_cjson(void) {
    static const char *const corpus[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"ok\":true}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-1,\"message\":\"x\"}}",
        ("{\"jsonrpc\":\"2.0\",\"id\":\"s\",\"method\":\"tools/call\","
            "\"params\":{\"name\":\"f\",\"arguments\":{\"n\":1,\"a\":[1,2,{\"k\":null}]}}}"),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"id\":2,\"method\":\"x\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"a\",\"method\":\"b\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":\"a\\tb\",\"method\":\"x\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools\\/call\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"broken\":}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":[1,]}",
        "{\"jsonrpc\":\"2.0\",\"id\":1}",
        "{\"jsonrpc\":\"2.0\",\"id\":1.5,\"method\":\"x\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":42}",
        "[1,2,3]",
        "{not json",
        "",
        "true",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":true} trailing",
        "{\"jsonrpc\":\"2.0\",\"id\":-7,\"method\":\"x\"}",
        "  {\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"x\"}  ",
    };

    for (size_t i = 0; i < sizeof(corpus) / sizeof(corpus[0]); i++) {
        const char *bytes = corpus[i];
        size_t len = strlen(bytes);
        struct mcp_msg got = {0};
        struct mcp_msg want = {0};
        int rc_got = mcp_msg_parse(bytes, len, &got);
        int rc_want = ref_parse(bytes, len, &want);
        CHECK(rc_got == rc_want, "differential: return codes agree");
        if (rc_got == MCP_PARSE_OK && rc_want == MCP_PARSE_OK) {
            CHECK(msgs_agree(&got, &want),
                  "differential: kind/id/method/is_error agree");
        }
        mcp_msg_free(&got);
        mcp_msg_free(&want);
    }
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
    test_parse_skip_must_validate();
    test_parse_first_wins_duplicate_keys();
    test_parse_escaped_id_method_fallback();
    test_parse_hot_path_no_cjson_tree();
    test_parse_scan_agrees_with_cjson();
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
