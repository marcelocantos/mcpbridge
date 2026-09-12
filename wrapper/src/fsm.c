/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#include "fsm.h"

#include <stddef.h>

void fsm_init(struct fsm *f) {
    if (f == NULL) {
        return;
    }
    f->state = FSM_STARTING;
}

/* All state transitions live here. The switch-on-state / switch-on-
 * event shape is deliberately boring: every transition is visible at
 * a glance, and unreachable combinations just fall through to the
 * "absorb and stay" default. */
enum fsm_state fsm_step(struct fsm *f, enum fsm_event ev) {
    if (f == NULL) {
        return FSM_FAILED;
    }

    switch (f->state) {

    case FSM_STARTING:
        switch (ev) {
        case FSM_EV_INITIALIZE_OK:
            f->state = FSM_RUNNING;
            break;
        case FSM_EV_INITIALIZE_FAILED:
        case FSM_EV_CHILD_EXIT:
        case FSM_EV_TRANSPORT_FAILED:
            f->state = FSM_FAILED;
            break;
        default:
            break;
        }
        break;

    case FSM_RUNNING:
        switch (ev) {
        case FSM_EV_RELOAD_REQUESTED:
            f->state = FSM_DRAINING;
            break;
        case FSM_EV_CHILD_EXIT:
        case FSM_EV_TRANSPORT_FAILED:
            f->state = FSM_FAILED;
            break;
        default:
            break;
        }
        break;

    case FSM_DRAINING:
        switch (ev) {
        case FSM_EV_IN_FLIGHT_ZERO:
        case FSM_EV_CHILD_EXIT:
            /* A child that exits during DRAINING is treated as
             * drain-complete: there is nothing left to wait for. */
            f->state = FSM_SWAPPING;
            break;
        default:
            break;
        }
        break;

    case FSM_SWAPPING:
        switch (ev) {
        case FSM_EV_TRANSPORT_STARTED:
            /* New child spawned; we need to re-init it. */
            f->state = FSM_STARTING;
            break;
        case FSM_EV_CHILD_EXIT:
        case FSM_EV_TRANSPORT_FAILED:
            f->state = FSM_FAILED;
            break;
        default:
            break;
        }
        break;

    case FSM_FAILED:
        /* Terminal: absorb anything. */
        break;
    }

    return f->state;
}

const char *fsm_state_name(enum fsm_state s) {
    switch (s) {
    case FSM_STARTING: return "STARTING";
    case FSM_RUNNING:  return "RUNNING";
    case FSM_DRAINING: return "DRAINING";
    case FSM_SWAPPING: return "SWAPPING";
    case FSM_FAILED:   return "FAILED";
    }
    return "?";
}

const char *fsm_event_name(enum fsm_event ev) {
    switch (ev) {
    case FSM_EV_TRANSPORT_STARTED:  return "TRANSPORT_STARTED";
    case FSM_EV_INITIALIZE_OK:      return "INITIALIZE_OK";
    case FSM_EV_INITIALIZE_FAILED:  return "INITIALIZE_FAILED";
    case FSM_EV_CHILD_EXIT:         return "CHILD_EXIT";
    case FSM_EV_TRANSPORT_FAILED:   return "TRANSPORT_FAILED";
    case FSM_EV_RELOAD_REQUESTED:   return "RELOAD_REQUESTED";
    case FSM_EV_IN_FLIGHT_ZERO:     return "IN_FLIGHT_ZERO";
    }
    return "?";
}
