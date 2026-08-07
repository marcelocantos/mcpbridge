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

    /* When non-zero, the next N calls to fake_send_child return
     * DISPATCH_SEND_STALE instead of OK. Used to simulate the upstream
     * having invalidated our session — without this hook, dispatch's
     * stale-recovery pathway is unreachable from a unit test. */
    int            stale_returns_remaining;

    /* When non-zero, the next N calls to fake_send_child return
     * DISPATCH_SEND_UPSTREAM_ERROR — a per-request upstream failure on
     * a still-usable transport (HTTP 5xx / truncated body). Used to
     * exercise the survive-and-surface-error path (🎯T17). */
    int            upstream_error_returns_remaining;
};

static void fake_send_upstream(void *ctx, const void *bytes, size_t n) {
    struct fake_sink_state *s = ctx;
    if (s->up_len + n > FAKE_BUF_CAP) {
        return;
    }
    memcpy(s->up_buf + s->up_len, bytes, n);
    s->up_len += n;
}

static int fake_send_child(void *ctx, const void *bytes, size_t n) {
    struct fake_sink_state *s = ctx;
    if (s->stale_returns_remaining > 0) {
        s->stale_returns_remaining--;
        return DISPATCH_SEND_STALE;
    }
    if (s->upstream_error_returns_remaining > 0) {
        s->upstream_error_returns_remaining--;
        return DISPATCH_SEND_UPSTREAM_ERROR;
    }
    if (s->child_len + n > FAKE_BUF_CAP) {
        return DISPATCH_SEND_OK;
    }
    memcpy(s->child_buf + s->child_len, bytes, n);
    s->child_len += n;
    return DISPATCH_SEND_OK;
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

static void test_initialize_cached_on_first_sighting(void) {
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

    CHECK(!dispatch_has_cached_init(d), "nothing cached initially");

    struct mcp_msg req = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"protocolVersion\":\"2024-11-05\","
        "\"capabilities\":{},\"clientInfo\":{\"name\":\"x\",\"version\":\"y\"}}}");
    dispatch_on_upstream(d, &req);

    CHECK(dispatch_has_cached_init(d),
          "initialize cached after first upstream sighting");

    /* notifications/initialized also gets cached. */
    struct mcp_msg inited = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    dispatch_on_upstream(d, &inited);

    /* Still cached, and also able to replay now. */
    CHECK(dispatch_has_cached_init(d), "still cached after initialized");

    mcp_msg_free(&req);
    mcp_msg_free(&inited);
    dispatch_free(d);
}

static void test_replay_sends_cached_bytes_to_child(void) {
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

    /* Drive the first handshake so the init request is cached. */
    struct mcp_msg req = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"protocolVersion\":\"2024-11-05\","
        "\"capabilities\":{},\"clientInfo\":{\"name\":\"x\",\"version\":\"y\"}}}");
    struct mcp_msg resp = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}");
    struct mcp_msg inited = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    dispatch_on_upstream(d, &req);
    dispatch_on_child(d, &resp);
    dispatch_on_upstream(d, &inited);

    /* Clear the sink state so we only observe the replay. */
    memset(&sink_state, 0, sizeof(sink_state));

    int rc = dispatch_replay_initialize(d);
    CHECK(rc == 1, "replay_initialize reported bytes sent");
    CHECK(sink_state.child_len > 0, "replay wrote to child");
    /* Should contain both the initialize request and the initialized
     * notification. */
    CHECK(strstr(sink_state.child_buf, "\"initialize\"") != NULL,
          "replay payload includes initialize");
    CHECK(strstr(sink_state.child_buf, "notifications/initialized") != NULL,
          "replay payload includes initialized notification");
    CHECK(dispatch_in_flight(d) == 1,
          "in_flight incremented by replay (initialize request)");

    mcp_msg_free(&req);
    mcp_msg_free(&resp);
    mcp_msg_free(&inited);
    dispatch_free(d);
}

static void test_replay_response_consumed_not_forwarded(void) {
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

    /* Prime the cache via the first real handshake. */
    struct mcp_msg req = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}");
    struct mcp_msg first_resp = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}");
    dispatch_on_upstream(d, &req);
    dispatch_on_child(d, &first_resp);

    /* Reset recording so we only see replay-era activity. */
    memset(&sink_state, 0, sizeof(sink_state));

    /* Trigger the replay. */
    dispatch_replay_initialize(d);
    CHECK(dispatch_in_flight(d) == 1, "in_flight=1 after replay");

    /* The new child responds to the replayed initialize. */
    struct mcp_msg replay_resp = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,"
        "\"result\":{\"serverInfo\":{\"name\":\"x\",\"version\":\"2\"}}}");
    dispatch_on_child(d, &replay_resp);

    /* The init response bytes themselves (the distinguishing
     * "serverInfo" marker) must NOT appear upstream — the agent
     * already has an initialize response from the first child. */
    sink_state.up_buf[sink_state.up_len < FAKE_BUF_CAP
                         ? sink_state.up_len : FAKE_BUF_CAP - 1] = '\0';
    CHECK(strstr(sink_state.up_buf, "serverInfo") == NULL,
          "replay response bytes NOT forwarded upstream");
    /* But three list_changed notifications MUST have been emitted
     * so the agent refetches tools / prompts / resources from the
     * new child. */
    CHECK(strstr(sink_state.up_buf, "notifications/tools/list_changed") != NULL,
          "tools/list_changed emitted on successful replay");
    CHECK(strstr(sink_state.up_buf, "notifications/prompts/list_changed") != NULL,
          "prompts/list_changed emitted on successful replay");
    CHECK(strstr(sink_state.up_buf, "notifications/resources/list_changed") != NULL,
          "resources/list_changed emitted on successful replay");

    CHECK(dispatch_in_flight(d) == 0, "in_flight decremented to 0");
    CHECK(sink_state.event_count == 1, "one event emitted by replay consume");
    CHECK(sink_state.events[0] == FSM_EV_INITIALIZE_OK,
          "INITIALIZE_OK emitted");

    /* After the replay completes, a subsequent real request/response
     * still flows normally upstream. */
    memset(&sink_state, 0, sizeof(sink_state));
    struct mcp_msg ping = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}");
    struct mcp_msg pong = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}");
    dispatch_on_upstream(d, &ping);
    dispatch_on_child(d, &pong);
    CHECK(sink_state.up_len > 0, "normal response still flows after replay");

    mcp_msg_free(&req);
    mcp_msg_free(&first_resp);
    mcp_msg_free(&replay_resp);
    mcp_msg_free(&ping);
    mcp_msg_free(&pong);
    dispatch_free(d);
}

static void test_replay_error_no_list_changed(void) {
    /* If the replayed initialize comes back as an error, the
     * wrapper must emit INITIALIZE_FAILED and must NOT emit any
     * list_changed notifications — there's no new surface to
     * refetch, the session is about to die. */
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

    /* Prime the cache. */
    struct mcp_msg req = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}");
    struct mcp_msg first_resp = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}");
    dispatch_on_upstream(d, &req);
    dispatch_on_child(d, &first_resp);

    memset(&sink_state, 0, sizeof(sink_state));
    dispatch_replay_initialize(d);

    /* Error response on the replay. */
    struct mcp_msg err_resp = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,"
        "\"error\":{\"code\":-32000,\"message\":\"child refused\"}}");
    dispatch_on_child(d, &err_resp);

    sink_state.up_buf[sink_state.up_len < FAKE_BUF_CAP
                         ? sink_state.up_len : FAKE_BUF_CAP - 1] = '\0';
    CHECK(strstr(sink_state.up_buf, "list_changed") == NULL,
          "no list_changed notifications on replay error");
    CHECK(sink_state.event_count == 1, "one event emitted");
    CHECK(sink_state.events[0] == FSM_EV_INITIALIZE_FAILED,
          "INITIALIZE_FAILED emitted");

    mcp_msg_free(&req);
    mcp_msg_free(&first_resp);
    mcp_msg_free(&err_resp);
    dispatch_free(d);
}

static void test_stale_send_requeues_and_triggers_reload(void) {
    /* When send_child returns STALE while RUNNING for an idempotent
     * read (here: tools/list), dispatch must:
     *   - undo the in-flight increment (the request never landed)
     *   - put the message bytes back in the queue
     *   - emit RELOAD_REQUESTED so the wrapper transitions through
     *     DRAINING -> SWAPPING and re-handshakes the upstream
     * After RUNNING is restored, the queued message drains under the
     * new session id without the agent ever seeing an error. */
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

    /* Drive the first handshake so the cache is populated. */
    struct mcp_msg init_req = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}");
    struct mcp_msg init_resp = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}");
    dispatch_on_upstream(d, &init_req);
    dispatch_on_child(d, &init_resp);
    CHECK(dispatch_initialized(d), "initialized");

    memset(&sink_state, 0, sizeof(sink_state));

    /* Arm the fake sink to return STALE on the next send. */
    sink_state.stale_returns_remaining = 1;

    /* Upstream issues an idempotent tools/list — safe to replay. */
    struct mcp_msg req = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/list\"}");
    dispatch_on_upstream(d, &req);

    /* The send was rejected → dispatch must not have left in_flight
     * incremented, must have queued the bytes, and must have emitted
     * RELOAD_REQUESTED. */
    CHECK(dispatch_in_flight(d) == 0,
          "in_flight rolled back after STALE");
    CHECK(sink_state.child_len == 0,
          "no bytes recorded as forwarded (the STALE attempt failed)");
    CHECK(sink_state.up_len == 0,
          "no error response surfaced for replay-safe method");
    CHECK(sink_state.event_count == 1, "one event emitted");
    CHECK(sink_state.events[0] == FSM_EV_RELOAD_REQUESTED,
          "RELOAD_REQUESTED emitted on STALE");

    /* Simulate the wrapper running the reload pathway: FSM moves
     * RUNNING -> DRAINING -> SWAPPING -> STARTING -> RUNNING.
     * dispatch_on_state_change(RUNNING) must drain the requeued
     * message. */
    fsm_step(&f, FSM_EV_RELOAD_REQUESTED); /* RUNNING -> DRAINING */
    fsm_step(&f, FSM_EV_IN_FLIGHT_ZERO);   /* DRAINING -> SWAPPING */
    fsm_step(&f, FSM_EV_TRANSPORT_STARTED); /* SWAPPING -> STARTING */
    fsm_step(&f, FSM_EV_INITIALIZE_OK);    /* STARTING -> RUNNING */
    dispatch_on_state_change(d, f.state);

    CHECK(strstr(sink_state.child_buf, "tools/list") != NULL,
          "queued tools/list drained to child after recovery");

    mcp_msg_free(&init_req);
    mcp_msg_free(&init_resp);
    mcp_msg_free(&req);
    dispatch_free(d);
}

static void test_stale_send_surfaces_error_for_side_effecting(void) {
    /* When send_child returns STALE for a side-effecting request
     * (tools/call, sampling/createMessage — anything not on the
     * replay-safe whitelist), dispatch must:
     *   - undo the in-flight increment
     *   - synthesise a structured -32002 error response to the agent
     *   - NOT queue the bytes for replay (no silent re-issue)
     *   - still emit RELOAD_REQUESTED so the next call lands cleanly
     * This is the at-most-once-delivery contract for tool calls under
     * upstream session loss. */
    struct fsm f;
    fsm_init(&f);
    fsm_step(&f, FSM_EV_INITIALIZE_OK);

    struct fake_sink_state sink_state = {0};
    struct dispatch_sink sink = {
        .send_upstream = fake_send_upstream,
        .send_child    = fake_send_child,
        .emit_event    = fake_emit_event,
        .ctx           = &sink_state,
    };
    struct dispatch *d = dispatch_new(&f, &sink);

    struct mcp_msg init_req = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}");
    struct mcp_msg init_resp = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}");
    dispatch_on_upstream(d, &init_req);
    dispatch_on_child(d, &init_resp);

    memset(&sink_state, 0, sizeof(sink_state));
    sink_state.stale_returns_remaining = 1;

    struct mcp_msg req = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"launch_app\"}}");
    dispatch_on_upstream(d, &req);

    CHECK(dispatch_in_flight(d) == 0,
          "in_flight rolled back after STALE");
    sink_state.up_buf[sink_state.up_len < FAKE_BUF_CAP
                          ? sink_state.up_len : FAKE_BUF_CAP - 1] = '\0';
    CHECK(strstr(sink_state.up_buf, "\"id\":42") != NULL,
          "error response carries the original request id");
    CHECK(strstr(sink_state.up_buf, "\"error\"") != NULL,
          "structured error surfaced to agent");
    CHECK(strstr(sink_state.up_buf, "-32002") != NULL,
          "session-reset error code present");
    CHECK(sink_state.event_count == 1, "RELOAD_REQUESTED still emitted");
    CHECK(sink_state.events[0] == FSM_EV_RELOAD_REQUESTED,
          "cycle is still triggered for next-call recovery");

    /* Run the cycle and confirm the side-effecting call is NOT
     * replayed to the new child. */
    sink_state.child_len = 0;
    fsm_step(&f, FSM_EV_RELOAD_REQUESTED);
    fsm_step(&f, FSM_EV_IN_FLIGHT_ZERO);
    fsm_step(&f, FSM_EV_TRANSPORT_STARTED);
    fsm_step(&f, FSM_EV_INITIALIZE_OK);
    dispatch_on_state_change(d, f.state);
    CHECK(strstr(sink_state.child_buf, "tools/call") == NULL,
          "side-effecting call NOT silently replayed after cycle");

    mcp_msg_free(&init_req);
    mcp_msg_free(&init_resp);
    mcp_msg_free(&req);
    dispatch_free(d);
}

static void test_upstream_error_surfaces_error_and_survives(void) {
    /* When send_child returns DISPATCH_SEND_UPSTREAM_ERROR for a
     * request (e.g. the HTTP backend answered 503, or reset the body
     * mid-stream), dispatch must:
     *   - undo the in-flight increment
     *   - synthesise a structured -32003 error carrying the request id
     *   - emit NO FSM event (no reload, no exit) so the wrapper stays
     *     alive and keeps serving the same session. 🎯T17. */
    struct fsm f;
    fsm_init(&f);
    fsm_step(&f, FSM_EV_INITIALIZE_OK);

    struct fake_sink_state sink_state = {0};
    struct dispatch_sink sink = {
        .send_upstream = fake_send_upstream,
        .send_child    = fake_send_child,
        .emit_event    = fake_emit_event,
        .ctx           = &sink_state,
    };
    struct dispatch *d = dispatch_new(&f, &sink);

    struct mcp_msg init_req = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}");
    struct mcp_msg init_resp = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}");
    dispatch_on_upstream(d, &init_req);
    dispatch_on_child(d, &init_resp);

    memset(&sink_state, 0, sizeof(sink_state));
    sink_state.upstream_error_returns_remaining = 1;

    struct mcp_msg req = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":77,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"do_thing\"}}");
    dispatch_on_upstream(d, &req);

    CHECK(dispatch_in_flight(d) == 0,
          "in_flight rolled back after upstream error");
    sink_state.up_buf[sink_state.up_len < FAKE_BUF_CAP
                          ? sink_state.up_len : FAKE_BUF_CAP - 1] = '\0';
    CHECK(strstr(sink_state.up_buf, "\"id\":77") != NULL,
          "error response carries the original request id");
    CHECK(strstr(sink_state.up_buf, "\"error\"") != NULL,
          "structured error surfaced to agent");
    CHECK(strstr(sink_state.up_buf, "-32003") != NULL,
          "upstream-request-failed error code present");
    CHECK(sink_state.event_count == 0,
          "no FSM event emitted — wrapper survives (no reload, no exit)");

    mcp_msg_free(&init_req);
    mcp_msg_free(&init_resp);
    mcp_msg_free(&req);
    dispatch_free(d);
}

static void test_replay_noop_without_cache(void) {
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

    int rc = dispatch_replay_initialize(d);
    CHECK(rc == 0, "replay_initialize returns 0 with empty cache");
    CHECK(sink_state.child_len == 0, "no bytes sent");
    CHECK(sink_state.event_count == 0, "no events emitted");

    dispatch_free(d);
}

/* 🎯T21. Requests outstanding against a backend that is going away
 * must be answered, addressed to their own ids, and the in-flight
 * bookkeeping must return to zero — otherwise the next drain waits
 * on a count that can never reach zero and the session is dead. */
static void test_settle_answers_abandoned_requests(void) {
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

    /* Two outstanding requests: one integer id, one string id. */
    struct mcp_msg r1 = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"slow\"}}");
    struct mcp_msg r2 = parse_or_die(
        "{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"method\":\"tools/list\"}");
    dispatch_on_upstream(d, &r1);
    dispatch_on_upstream(d, &r2);
    CHECK(dispatch_in_flight(d) == 2, "two requests in flight");

    sink_state.up_len = 0;
    dispatch_settle_in_flight(d, "mcpbridge: backend cycled during call");

    CHECK(dispatch_in_flight(d) == 0,
          "in_flight reset to zero after settling");
    CHECK(strstr(sink_state.up_buf, "\"id\":11") != NULL,
          "error response addressed to the integer id");
    CHECK(strstr(sink_state.up_buf, "\"id\":\"abc\"") != NULL,
          "error response addressed to the string id");
    CHECK(strstr(sink_state.up_buf, "\"error\"") != NULL,
          "responses are JSON-RPC errors");

    /* The whole point: a subsequent drain must be able to finish. */
    sink_state.event_count = 0;
    fsm_step(&f, FSM_EV_RELOAD_REQUESTED);
    dispatch_on_state_change(d, f.state);
    int saw_zero = 0;
    for (int i = 0; i < sink_state.event_count; i++) {
        if (sink_state.events[i] == FSM_EV_IN_FLIGHT_ZERO) {
            saw_zero = 1;
        }
    }
    CHECK(saw_zero, "a later drain reaches IN_FLIGHT_ZERO");

    mcp_msg_free(&r1);
    mcp_msg_free(&r2);
    dispatch_free(d);
}

/* Settling with nothing outstanding must be silent and harmless —
 * it runs on every swap, including the orderly ones. */
static void test_settle_noop_when_idle(void) {
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

    dispatch_settle_in_flight(d, NULL);
    CHECK(sink_state.up_len == 0, "no bytes sent upstream when idle");
    CHECK(dispatch_in_flight(d) == 0, "in_flight still zero");

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
    test_initialize_cached_on_first_sighting();
    test_replay_sends_cached_bytes_to_child();
    test_replay_response_consumed_not_forwarded();
    test_replay_error_no_list_changed();
    test_stale_send_requeues_and_triggers_reload();
    test_stale_send_surfaces_error_for_side_effecting();
    test_upstream_error_surfaces_error_and_survives();
    test_replay_noop_without_cache();
    test_settle_answers_abandoned_requests();
    test_settle_noop_when_idle();

    if (fail_count > 0) {
        fprintf(stderr, "%d dispatch_test assertion(s) failed\n", fail_count);
        return 1;
    }
    puts("dispatch_test: ok");
    return 0;
}
