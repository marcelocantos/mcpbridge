/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

/* Unit tests for wrapper/src/fsm.c. The FSM is pure, so these tests
 * exercise transitions by feeding event sequences and asserting the
 * resulting state. No forks, no pipes, no temp files. */

#include "../src/fsm.h"

#include <stdio.h>

static int fail_count = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);  \
            fail_count++;                                                  \
        }                                                                  \
    } while (0)

/* Helper: feed a sequence of events into a freshly-initialised FSM
 * and return the final state. */
static enum fsm_state run_seq(const enum fsm_event *events, int n) {
    struct fsm f;
    fsm_init(&f);
    for (int i = 0; i < n; i++) {
        fsm_step(&f, events[i]);
    }
    return f.state;
}

/* ---------- init ---------- */

static void test_init_defaults(void) {
    struct fsm f;
    fsm_init(&f);
    CHECK(f.state == FSM_STARTING, "initial state is STARTING");
}

/* ---------- STARTING transitions ---------- */

static void test_starting_to_running(void) {
    struct fsm f;
    fsm_init(&f);
    enum fsm_state s = fsm_step(&f, FSM_EV_INITIALIZE_OK);
    CHECK(s == FSM_RUNNING, "STARTING + INITIALIZE_OK -> RUNNING");
}

static void test_starting_init_failed(void) {
    struct fsm f;
    fsm_init(&f);
    enum fsm_state s = fsm_step(&f, FSM_EV_INITIALIZE_FAILED);
    CHECK(s == FSM_FAILED, "STARTING + INITIALIZE_FAILED -> FAILED");
}

static void test_starting_child_exit(void) {
    struct fsm f;
    fsm_init(&f);
    enum fsm_state s = fsm_step(&f, FSM_EV_CHILD_EXIT);
    CHECK(s == FSM_FAILED, "STARTING + CHILD_EXIT -> FAILED");
}

static void test_starting_transport_failed(void) {
    struct fsm f;
    fsm_init(&f);
    enum fsm_state s = fsm_step(&f, FSM_EV_TRANSPORT_FAILED);
    CHECK(s == FSM_FAILED, "STARTING + TRANSPORT_FAILED -> FAILED");
}

static void test_starting_ignores_unrelated(void) {
    struct fsm f;
    fsm_init(&f);
    enum fsm_state s = fsm_step(&f, FSM_EV_RELOAD_REQUESTED);
    CHECK(s == FSM_STARTING, "STARTING ignores RELOAD_REQUESTED");
    s = fsm_step(&f, FSM_EV_IN_FLIGHT_ZERO);
    CHECK(s == FSM_STARTING, "STARTING ignores IN_FLIGHT_ZERO");
}

/* ---------- RUNNING transitions ---------- */

static void test_running_reload(void) {
    const enum fsm_event ev[] = {FSM_EV_INITIALIZE_OK, FSM_EV_RELOAD_REQUESTED};
    CHECK(run_seq(ev, 2) == FSM_DRAINING,
          "RUNNING + RELOAD_REQUESTED -> DRAINING");
}

static void test_running_child_exit_goes_to_failed(void) {
    const enum fsm_event ev[] = {FSM_EV_INITIALIZE_OK, FSM_EV_CHILD_EXIT};
    CHECK(run_seq(ev, 2) == FSM_FAILED,
          "RUNNING + CHILD_EXIT -> FAILED");
}

static void test_running_transport_failed_goes_to_failed(void) {
    const enum fsm_event ev[] = {FSM_EV_INITIALIZE_OK, FSM_EV_TRANSPORT_FAILED};
    CHECK(run_seq(ev, 2) == FSM_FAILED,
          "RUNNING + TRANSPORT_FAILED -> FAILED");
}

static void test_running_ignores_unrelated(void) {
    struct fsm f;
    fsm_init(&f);
    fsm_step(&f, FSM_EV_INITIALIZE_OK);
    CHECK(fsm_step(&f, FSM_EV_IN_FLIGHT_ZERO) == FSM_RUNNING,
          "RUNNING ignores IN_FLIGHT_ZERO");
    CHECK(fsm_step(&f, FSM_EV_TRANSPORT_STARTED) == FSM_RUNNING,
          "RUNNING ignores TRANSPORT_STARTED");
}

/* ---------- DRAINING transitions ---------- */

static void test_draining_in_flight_zero(void) {
    const enum fsm_event ev[] = {
        FSM_EV_INITIALIZE_OK,
        FSM_EV_RELOAD_REQUESTED,
        FSM_EV_IN_FLIGHT_ZERO,
    };
    CHECK(run_seq(ev, 3) == FSM_SWAPPING,
          "DRAINING + IN_FLIGHT_ZERO -> SWAPPING");
}

static void test_draining_child_exit_shortcircuits(void) {
    const enum fsm_event ev[] = {
        FSM_EV_INITIALIZE_OK,
        FSM_EV_RELOAD_REQUESTED,
        FSM_EV_CHILD_EXIT,
    };
    CHECK(run_seq(ev, 3) == FSM_SWAPPING,
          "DRAINING + CHILD_EXIT -> SWAPPING (drain short-circuit)");
}

static void test_draining_ignores_unrelated(void) {
    struct fsm f;
    fsm_init(&f);
    fsm_step(&f, FSM_EV_INITIALIZE_OK);
    fsm_step(&f, FSM_EV_RELOAD_REQUESTED);
    CHECK(fsm_step(&f, FSM_EV_RELOAD_REQUESTED) == FSM_DRAINING,
          "DRAINING ignores a second RELOAD_REQUESTED");
    CHECK(fsm_step(&f, FSM_EV_TRANSPORT_STARTED) == FSM_DRAINING,
          "DRAINING ignores TRANSPORT_STARTED");
}

/* ---------- SWAPPING transitions ---------- */

static void test_swapping_transport_started(void) {
    const enum fsm_event ev[] = {
        FSM_EV_INITIALIZE_OK,
        FSM_EV_RELOAD_REQUESTED,
        FSM_EV_IN_FLIGHT_ZERO,
        FSM_EV_TRANSPORT_STARTED,
    };
    CHECK(run_seq(ev, 4) == FSM_STARTING,
          "SWAPPING + TRANSPORT_STARTED -> STARTING");
}

static void test_swapping_child_exit_goes_to_failed(void) {
    const enum fsm_event ev[] = {
        FSM_EV_INITIALIZE_OK,
        FSM_EV_RELOAD_REQUESTED,
        FSM_EV_IN_FLIGHT_ZERO,
        FSM_EV_CHILD_EXIT,
    };
    CHECK(run_seq(ev, 4) == FSM_FAILED,
          "SWAPPING + CHILD_EXIT -> FAILED");
}

static void test_swapping_transport_failed_goes_to_failed(void) {
    const enum fsm_event ev[] = {
        FSM_EV_INITIALIZE_OK,
        FSM_EV_RELOAD_REQUESTED,
        FSM_EV_IN_FLIGHT_ZERO,
        FSM_EV_TRANSPORT_FAILED,
    };
    CHECK(run_seq(ev, 4) == FSM_FAILED,
          "SWAPPING + TRANSPORT_FAILED -> FAILED");
}

/* ---------- FAILED is terminal ---------- */

static void test_failed_is_terminal(void) {
    struct fsm f;
    fsm_init(&f);
    fsm_step(&f, FSM_EV_INITIALIZE_FAILED);
    /* Feed every event and assert we stay FAILED. */
    const enum fsm_event all[] = {
        FSM_EV_TRANSPORT_STARTED,
        FSM_EV_INITIALIZE_OK,
        FSM_EV_INITIALIZE_FAILED,
        FSM_EV_CHILD_EXIT,
        FSM_EV_TRANSPORT_FAILED,
        FSM_EV_RELOAD_REQUESTED,
        FSM_EV_IN_FLIGHT_ZERO,
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        CHECK(fsm_step(&f, all[i]) == FSM_FAILED, "FAILED absorbs event");
    }
}

/* ---------- names ---------- */

static void test_names(void) {
    CHECK(fsm_state_name(FSM_STARTING) != NULL, "STARTING name");
    CHECK(fsm_state_name(FSM_FAILED) != NULL, "FAILED name");
    CHECK(fsm_event_name(FSM_EV_INITIALIZE_OK) != NULL, "event name");
    /* Out-of-range enum values must not crash. */
    CHECK(fsm_state_name((enum fsm_state)99) != NULL, "bogus state -> ?");
    CHECK(fsm_event_name((enum fsm_event)99) != NULL, "bogus event -> ?");
}

int main(void) {
    test_init_defaults();

    test_starting_to_running();
    test_starting_init_failed();
    test_starting_child_exit();
    test_starting_transport_failed();
    test_starting_ignores_unrelated();

    test_running_reload();
    test_running_child_exit_goes_to_failed();
    test_running_transport_failed_goes_to_failed();
    test_running_ignores_unrelated();

    test_draining_in_flight_zero();
    test_draining_child_exit_shortcircuits();
    test_draining_ignores_unrelated();

    test_swapping_transport_started();
    test_swapping_child_exit_goes_to_failed();
    test_swapping_transport_failed_goes_to_failed();

    test_failed_is_terminal();

    test_names();

    if (fail_count > 0) {
        fprintf(stderr, "%d fsm_test assertion(s) failed\n", fail_count);
        return 1;
    }
    puts("fsm_test: ok");
    return 0;
}
