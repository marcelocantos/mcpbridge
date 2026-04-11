/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#ifndef MCPBRIDGE_FSM_H
#define MCPBRIDGE_FSM_H

/* Child-lifecycle state machine.
 *
 * This module is the coordination spine of the wrapper. Its single
 * job is to say "given the current state and this event, what is the
 * new state?" It does no I/O, owns no resources, and holds no
 * references to the outside world. The event loop feeds it events
 * and inspects the resulting state to decide what to do next.
 *
 * Making this pure buys two things:
 *
 *   1. Correctness by construction. Rules like "never forward a
 *      request while DRAINING" or "never swap while in-flight > 0"
 *      are encoded as transitions, not as boolean flags the caller
 *      must remember to check.
 *   2. Trivially testable. No mocks, no fake fds, no temp directories —
 *      feed events, assert transitions.
 *
 * Transitions (non-terminal source states only):
 *
 *   STARTING ── INITIALIZE_OK     ──► RUNNING
 *   STARTING ── INITIALIZE_FAILED ──► FAILED
 *   STARTING ── CHILD_EXIT        ──► FAILED
 *   STARTING ── TRANSPORT_FAILED  ──► FAILED
 *
 *   RUNNING  ── RELOAD_REQUESTED  ──► DRAINING
 *   RUNNING  ── CHILD_EXIT        ──► RESPAWN
 *   RUNNING  ── TRANSPORT_FAILED  ──► RESPAWN
 *
 *   DRAINING ── IN_FLIGHT_ZERO    ──► SWAPPING
 *   DRAINING ── CHILD_EXIT        ──► SWAPPING   (drain completed "naturally")
 *
 *   SWAPPING ── TRANSPORT_STARTED ──► STARTING   (new child spawned)
 *   SWAPPING ── TRANSPORT_FAILED  ──► RESPAWN
 *   SWAPPING ── CHILD_EXIT        ──► RESPAWN    (child died while we were starting it)
 *
 *   RESPAWN  ── BACKOFF_EXPIRED   ──► SWAPPING   (increments respawn_attempts)
 *
 *   FAILED is terminal.
 *
 * Any event that does not match an outgoing transition from the
 * current state is ignored: the state is unchanged and fsm_step
 * returns the current state. This is a deliberate choice — the event
 * loop may produce stale or duplicate events during transitions, and
 * the FSM should absorb them without crashing. */

enum fsm_state {
    FSM_STARTING = 0,
    FSM_RUNNING,
    FSM_DRAINING,
    FSM_SWAPPING,
    FSM_RESPAWN,
    FSM_FAILED,
};

enum fsm_event {
    FSM_EV_TRANSPORT_STARTED = 0,
    FSM_EV_INITIALIZE_OK,
    FSM_EV_INITIALIZE_FAILED,
    FSM_EV_CHILD_EXIT,
    FSM_EV_TRANSPORT_FAILED,
    FSM_EV_RELOAD_REQUESTED,
    FSM_EV_IN_FLIGHT_ZERO,
    FSM_EV_BACKOFF_EXPIRED,
};

/* Default maximum number of respawn attempts before giving up. The
 * event loop can override this via fsm_init_with_limit(). */
#define FSM_RESPAWN_LIMIT_DEFAULT 5

struct fsm {
    enum fsm_state state;
    int respawn_attempts;
    int respawn_limit;
};

/* Initialise an FSM in STARTING with the default respawn limit. */
void fsm_init(struct fsm *f);

/* Initialise an FSM with a custom respawn limit. A limit of 0 is
 * treated as 1 (the FSM will attempt at least one respawn before
 * giving up). */
void fsm_init_with_limit(struct fsm *f, int limit);

/* Feed an event and advance the state. Returns the resulting state.
 * Safe to call with stale or duplicate events — unknown transitions
 * are absorbed without changing state. */
enum fsm_state fsm_step(struct fsm *f, enum fsm_event ev);

/* Convenience accessors for logging. Return stable string literals,
 * no allocation. "?" is returned for out-of-range enum values. */
const char *fsm_state_name(enum fsm_state s);
const char *fsm_event_name(enum fsm_event ev);

#endif /* MCPBRIDGE_FSM_H */
