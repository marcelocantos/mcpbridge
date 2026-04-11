/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

/* Unit tests for wrapper/src/dispatch.c. Uses a fake sink that
 * records every call in arrays so each test can assert the exact
 * output sequence. No forks, no pipes. */

#include "../src/dispatch.h"
#include "../src/fsm.h"
#include "../src/mcp.h"

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

/* ---------- Recording sink ---------- */

#define FAKE_BUF_CAP (16 * 1024)

struct fake_sink_state {
    char   up_buf[FAKE_BUF_CAP];
    size_t up_len;

    char   child_buf[FAKE_BUF_CAP];
    size_t child_len;

    enum fsm_event events[32];
    int            event_count;
};

static void fake_send_upstream(void *ctx, const void *bytes, size_t n) {
    struct fake_sink_state *s = ctx;
    if (s->up_len + n > FAKE_BUF_CAP) {
        return;
    }
    memcpy(s->up_buf + s->up_len, bytes, n);
    s->up_len += n;
}

static void fake_send_child(void *ctx, const void *bytes, size_t n) {
    struct fake_sink_state *s = ctx;
    if (s->child_len + n > FAKE_BUF_CAP) {
        return;
    }
    memcpy(s->child_buf + s->child_len, bytes, n);
    s->child_len += n;
}

static void fake_emit_event(void *ctx, enum fsm_event ev) {
    struct fake_sink_state *s = ctx;
    if (s->event_count < (int)(sizeof(s->events) / sizeof(s->events[0]))) {
        s->events[s->event_count++] = ev;
    }
}

/* ---------- Parse helper ---------- */

static struct mcp_msg parse_or_die(const char *bytes) {
    struct mcp_msg m = {0};
    int rc = mcp_msg_parse(bytes, strlen(bytes), &m);
    if (rc != MCP_PARSE_OK) {
        fprintf(stderr, "parse_or_die: rc=%d for %s\n", rc, bytes);
        exit(99);
    }
    return m;
}

/* ---------- Tests ---------- */

/* Helper: drive the FSM into a target state via a scripted sequence
 * of events. Keeps tests readable. */
static void fsm_to_running(struct fsm *f) {
    fsm_init(f);
    fsm_step(f, FSM_EV_INITIALIZE_OK);
}

static void test_round_trip_running(void) {
    struct fsm f;
    fsm_to_running(&f);

    struct fake_sink_state sink_state = {0};
    struct dispatch_sink sink = {
        .send_upstream = fake_send_upstream,
        .send_child    = fake_send_child,
        .emit_event    = fake_emit_event,
        .ctx           = &sink_state,
    };

    struct dispatch *d = dispatch_new(&f, &sink);
    CHECK(d != NULL, "dispatch_new");

    /* Upstream sends a request. */
    struct mcp_msg req = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"foo\"}}");
    dispatch_on_upstream(d, &req);
    CHECK(dispatch_in_flight(d) == 1, "in_flight incremented to 1");
    CHECK(sink_state.child_len > 0, "request forwarded to child");
    CHECK(sink_state.child_buf[sink_state.child_len - 1] == '\n',
          "newline appended");

    /* Child responds. */
    struct mcp_msg resp = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":7,"
        "\"result\":{\"content\":[],\"isError\":false}}");
    dispatch_on_child(d, &resp);
    CHECK(dispatch_in_flight(d) == 0, "in_flight back to 0");
    CHECK(sink_state.up_len > 0, "response forwarded upstream");
    CHECK(sink_state.up_buf[sink_state.up_len - 1] == '\n',
          "newline appended to upstream");

    mcp_msg_free(&req);
    mcp_msg_free(&resp);
    dispatch_free(d);
}

static void test_queue_while_starting_drain_on_running(void) {
    struct fsm f;
    fsm_init(&f); /* state = STARTING */

    struct fake_sink_state sink_state = {0};
    struct dispatch_sink sink = {
        .send_upstream = fake_send_upstream,
        .send_child    = fake_send_child,
        .emit_event    = fake_emit_event,
        .ctx           = &sink_state,
    };
    struct dispatch *d = dispatch_new(&f, &sink);

    /* Upstream sends a request while we are STARTING. It must be
     * queued — nothing goes to the child yet. */
    struct mcp_msg req1 = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}");
    dispatch_on_upstream(d, &req1);
    CHECK(sink_state.child_len == 0, "nothing sent to child during STARTING");

    /* Another one. */
    struct mcp_msg req2 = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
    dispatch_on_upstream(d, &req2);
    CHECK(sink_state.child_len == 0, "still nothing forwarded");

    /* FSM now transitions to RUNNING (initialize came back). The
     * event loop calls dispatch_on_state_change, which drains the
     * queue in FIFO order. */
    fsm_step(&f, FSM_EV_INITIALIZE_OK);
    dispatch_on_state_change(d, f.state);

    CHECK(sink_state.child_len > 0, "queue drained to child on RUNNING");
    /* Both messages should appear in order. */
    const char *ping = strstr(sink_state.child_buf, "ping");
    const char *list = strstr(sink_state.child_buf, "tools/list");
    CHECK(ping != NULL && list != NULL, "both queued messages forwarded");
    CHECK(ping < list, "FIFO order preserved");

    mcp_msg_free(&req1);
    mcp_msg_free(&req2);
    dispatch_free(d);
}

static void test_initialize_handshake_intercept(void) {
    /* The real protocol flow: FSM starts in STARTING, upstream sends
     * initialize, dispatch forwards it immediately (bypassing the
     * queue), child responds, dispatch emits INITIALIZE_OK, event
     * loop steps FSM to RUNNING. */
    struct fsm f;
    fsm_init(&f); /* STARTING */

    struct fake_sink_state sink_state = {0};
    struct dispatch_sink sink = {
        .send_upstream = fake_send_upstream,
        .send_child    = fake_send_child,
        .emit_event    = fake_emit_event,
        .ctx           = &sink_state,
    };
    struct dispatch *d = dispatch_new(&f, &sink);

    /* Initialize request from upstream while in STARTING. */
    struct mcp_msg req = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"protocolVersion\":\"2024-11-05\","
        "\"capabilities\":{},\"clientInfo\":{\"name\":\"x\",\"version\":\"y\"}}}");
    dispatch_on_upstream(d, &req);

    CHECK(sink_state.child_len > 0, "initialize forwarded during STARTING");
    CHECK(dispatch_in_flight(d) == 1, "in_flight=1 after request");
    CHECK(!dispatch_initialized(d), "not yet initialized");

    /* Child response arrives. */
    struct mcp_msg resp = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,"
        "\"result\":{\"protocolVersion\":\"2024-11-05\","
        "\"serverInfo\":{\"name\":\"x\",\"version\":\"1\"},"
        "\"capabilities\":{}}}");
    dispatch_on_child(d, &resp);

    CHECK(dispatch_initialized(d), "initialized flag set");
    CHECK(dispatch_in_flight(d) == 0, "in_flight decremented");
    CHECK(sink_state.event_count == 1, "one event emitted");
    CHECK(sink_state.events[0] == FSM_EV_INITIALIZE_OK,
          "INITIALIZE_OK emitted");
    CHECK(sink_state.up_len > 0, "response forwarded upstream");

    /* Event loop would now step the FSM to RUNNING and call
     * dispatch_on_state_change, draining any queued messages. */
    fsm_step(&f, FSM_EV_INITIALIZE_OK);
    dispatch_on_state_change(d, f.state);

    /* A subsequent non-handshake request goes through the normal
     * RUNNING path. */
    struct mcp_msg req2 = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}");
    struct mcp_msg resp2 = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}");
    dispatch_on_upstream(d, &req2);
    dispatch_on_child(d, &resp2);
    CHECK(sink_state.event_count == 1, "no second INITIALIZE_OK");

    mcp_msg_free(&req);
    mcp_msg_free(&resp);
    mcp_msg_free(&req2);
    mcp_msg_free(&resp2);
    dispatch_free(d);
}

static void test_non_handshake_queued_in_starting(void) {
    /* Non-handshake messages must still queue while STARTING. */
    struct fsm f;
    fsm_init(&f);

    struct fake_sink_state sink_state = {0};
    struct dispatch_sink sink = {
        .send_upstream = fake_send_upstream,
        .send_child    = fake_send_child,
        .emit_event    = fake_emit_event,
        .ctx           = &sink_state,
    };
    struct dispatch *d = dispatch_new(&f, &sink);

    /* A random ping during STARTING should be queued, not forwarded. */
    struct mcp_msg ping = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"ping\"}");
    dispatch_on_upstream(d, &ping);
    CHECK(sink_state.child_len == 0, "ping queued during STARTING");

    mcp_msg_free(&ping);
    dispatch_free(d);
}

static void test_initialized_notification_passes_through(void) {
    /* notifications/initialized is part of the handshake and must
     * bypass the queue just like the initialize request. */
    struct fsm f;
    fsm_init(&f);

    struct fake_sink_state sink_state = {0};
    struct dispatch_sink sink = {
        .send_upstream = fake_send_upstream,
        .send_child    = fake_send_child,
        .emit_event    = fake_emit_event,
        .ctx           = &sink_state,
    };
    struct dispatch *d = dispatch_new(&f, &sink);

    struct mcp_msg n = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    dispatch_on_upstream(d, &n);
    CHECK(sink_state.child_len > 0,
          "initialized notification forwarded during STARTING");
    /* Notifications have no id; in_flight must NOT be incremented. */
    CHECK(dispatch_in_flight(d) == 0, "notifications do not count as in-flight");

    mcp_msg_free(&n);
    dispatch_free(d);
}

static void test_initialize_failed_emitted(void) {
    struct fsm f;
    fsm_init(&f);
    fsm_step(&f, FSM_EV_INITIALIZE_OK); /* RUNNING */

    struct fake_sink_state sink_state = {0};
    struct dispatch_sink sink = {
        .send_upstream = fake_send_upstream,
        .send_child    = fake_send_child,
        .emit_event    = fake_emit_event,
        .ctx           = &sink_state,
    };
    struct dispatch *d = dispatch_new(&f, &sink);

    struct mcp_msg req = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}");
    dispatch_on_upstream(d, &req);

    struct mcp_msg err = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,"
        "\"error\":{\"code\":-32000,\"message\":\"nope\"}}");
    CHECK(err.is_error, "parser tagged error response");
    dispatch_on_child(d, &err);

    CHECK(sink_state.event_count == 1, "one event emitted");
    CHECK(sink_state.events[0] == FSM_EV_INITIALIZE_FAILED,
          "INITIALIZE_FAILED emitted on error response");

    mcp_msg_free(&req);
    mcp_msg_free(&err);
    dispatch_free(d);
}

static void test_drain_emits_in_flight_zero(void) {
    struct fsm f;
    fsm_init(&f);
    fsm_step(&f, FSM_EV_INITIALIZE_OK); /* RUNNING */

    struct fake_sink_state sink_state = {0};
    struct dispatch_sink sink = {
        .send_upstream = fake_send_upstream,
        .send_child    = fake_send_child,
        .emit_event    = fake_emit_event,
        .ctx           = &sink_state,
    };
    struct dispatch *d = dispatch_new(&f, &sink);

    /* Simulate the initial initialize so the "initialized" flag is
     * set and subsequent responses don't trigger INITIALIZE_OK. */
    struct mcp_msg init_req = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}");
    struct mcp_msg init_resp = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}");
    dispatch_on_upstream(d, &init_req);
    dispatch_on_child(d, &init_resp);
    CHECK(dispatch_initialized(d), "initialized");

    /* Two in-flight requests. */
    struct mcp_msg req_a = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\"}");
    struct mcp_msg req_b = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\"}");
    dispatch_on_upstream(d, &req_a);
    dispatch_on_upstream(d, &req_b);
    CHECK(dispatch_in_flight(d) == 2, "in_flight=2");

    /* FSM: reload requested -> DRAINING. */
    fsm_step(&f, FSM_EV_RELOAD_REQUESTED);
    dispatch_on_state_change(d, f.state);

    /* Reset event recording so we can assert the next event cleanly. */
    int events_before = sink_state.event_count;

    /* First response comes back; in_flight now 1; no event yet. */
    struct mcp_msg resp_a = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"result\":{}}");
    dispatch_on_child(d, &resp_a);
    CHECK(dispatch_in_flight(d) == 1, "in_flight=1 after first response");
    CHECK(sink_state.event_count == events_before,
          "no event yet");

    /* Second response — drops to zero, emit IN_FLIGHT_ZERO. */
    struct mcp_msg resp_b = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"result\":{}}");
    dispatch_on_child(d, &resp_b);
    CHECK(dispatch_in_flight(d) == 0, "in_flight=0 after second response");
    CHECK(sink_state.event_count == events_before + 1,
          "one event after drain");
    CHECK(sink_state.events[events_before] == FSM_EV_IN_FLIGHT_ZERO,
          "IN_FLIGHT_ZERO emitted");

    mcp_msg_free(&init_req);
    mcp_msg_free(&init_resp);
    mcp_msg_free(&req_a);
    mcp_msg_free(&req_b);
    mcp_msg_free(&resp_a);
    mcp_msg_free(&resp_b);
    dispatch_free(d);
}

int main(void) {
    test_round_trip_running();
    test_queue_while_starting_drain_on_running();
    test_initialize_handshake_intercept();
    test_non_handshake_queued_in_starting();
    test_initialized_notification_passes_through();
    test_initialize_failed_emitted();
    test_drain_emits_in_flight_zero();

    if (fail_count > 0) {
        fprintf(stderr, "%d dispatch_test assertion(s) failed\n", fail_count);
        return 1;
    }
    puts("dispatch_test: ok");
    return 0;
}
