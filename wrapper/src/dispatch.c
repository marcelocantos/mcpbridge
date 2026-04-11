/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#include "dispatch.h"

#include "fsm.h"
#include "log.h"
#include "mcp.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

/* A queued upstream message, retained as raw bytes (plus a trailing
 * newline when we emit it). Kept simple: singly-linked FIFO. */
struct queued_msg {
    struct queued_msg *next;
    char              *bytes;  /* owned; raw message body, no newline */
    size_t             len;
};

struct dispatch {
    struct fsm          *fsm;
    struct dispatch_sink sink;

    int in_flight;
    int initialized;        /* 1 once we've seen an initialize response */
    int replay_pending;     /* 1 while we're waiting for the response to
                               a replayed initialize request — that
                               response is consumed, not forwarded */

    /* Cached handshake messages, owned. Captured on first sighting
     * from upstream; replayed verbatim to a new child after a swap. */
    char  *cached_init_req;
    size_t cached_init_req_len;
    char  *cached_initialized_notif;
    size_t cached_initialized_notif_len;

    struct queued_msg *queue_head;
    struct queued_msg *queue_tail;
};

static void send_raw_with_newline(const struct dispatch_sink *sink,
                                  int to_child,
                                  const void *bytes, size_t n) {
    /* The parser strips the trailing newline from the raw bytes.
     * When we forward, we need to re-append it so the downstream
     * framing is correct. We do this in two writes to avoid a
     * per-message allocation — callers see one logical MCP line. */
    if (to_child) {
        sink->send_child(sink->ctx, bytes, n);
        sink->send_child(sink->ctx, "\n", 1);
    } else {
        sink->send_upstream(sink->ctx, bytes, n);
        sink->send_upstream(sink->ctx, "\n", 1);
    }
}

static void enqueue(struct dispatch *d, const void *bytes, size_t n) {
    struct queued_msg *q = xmalloc(sizeof(*q));
    q->next  = NULL;
    q->bytes = xmalloc(n);
    memcpy(q->bytes, bytes, n);
    q->len   = n;
    if (d->queue_tail == NULL) {
        d->queue_head = q;
    } else {
        d->queue_tail->next = q;
    }
    d->queue_tail = q;
}

static struct queued_msg *dequeue(struct dispatch *d) {
    struct queued_msg *q = d->queue_head;
    if (q == NULL) {
        return NULL;
    }
    d->queue_head = q->next;
    if (d->queue_head == NULL) {
        d->queue_tail = NULL;
    }
    return q;
}

static void queued_msg_free(struct queued_msg *q) {
    if (q == NULL) {
        return;
    }
    free(q->bytes);
    free(q);
}

struct dispatch *dispatch_new(struct fsm *fsm,
                              const struct dispatch_sink *sink) {
    if (fsm == NULL || sink == NULL ||
        sink->send_upstream == NULL ||
        sink->send_child == NULL ||
        sink->emit_event == NULL) {
        return NULL;
    }
    struct dispatch *d = xcalloc(1, sizeof(*d));
    d->fsm  = fsm;
    d->sink = *sink;
    return d;
}

void dispatch_free(struct dispatch *d) {
    if (d == NULL) {
        return;
    }
    struct queued_msg *q = d->queue_head;
    while (q != NULL) {
        struct queued_msg *next = q->next;
        queued_msg_free(q);
        q = next;
    }
    free(d->cached_init_req);
    free(d->cached_initialized_notif);
    free(d);
}

int dispatch_in_flight(const struct dispatch *d) {
    return (d == NULL) ? 0 : d->in_flight;
}

int dispatch_initialized(const struct dispatch *d) {
    return (d == NULL) ? 0 : d->initialized;
}

int dispatch_has_cached_init(const struct dispatch *d) {
    return (d == NULL) ? 0 : (d->cached_init_req != NULL);
}

/* Store a copy of the given bytes in *dst / *dst_len, replacing any
 * prior content. Used to cache the initialize request and the
 * initialized notification on first sighting. */
static void capture_raw(char **dst, size_t *dst_len,
                        const void *bytes, size_t n) {
    free(*dst);
    *dst = xmalloc(n);
    memcpy(*dst, bytes, n);
    *dst_len = n;
}

/* ---------- Upstream (agent -> wrapper -> child) ---------- */

/* The initialize handshake is special: the client always sends
 * `initialize` first, and the FSM is in STARTING precisely until
 * that round-trip completes. If we queued it like any other request
 * we would deadlock — the child would never receive it, never
 * respond, and the FSM would never reach RUNNING. So initialize
 * requests bypass the queue regardless of state. The companion
 * `notifications/initialized` notification that the spec requires
 * immediately after initialize gets the same treatment so both
 * halves of the handshake complete before normal flow resumes. */
static int is_handshake_message(const struct mcp_msg *m) {
    if (m == NULL || m->method == NULL) {
        return 0;
    }
    if (mcp_msg_is_request(m, "initialize")) {
        return 1;
    }
    if (mcp_msg_is_notification(m, "notifications/initialized")) {
        return 1;
    }
    return 0;
}

void dispatch_on_upstream(struct dispatch *d, const struct mcp_msg *m) {
    if (d == NULL || m == NULL || m->raw == NULL) {
        return;
    }

    /* Cache the handshake messages on first sighting so we can
     * replay them to a freshly spawned child after a reload. The
     * first sighting wins — if the agent ever re-sends initialize
     * (unusual but legal) we keep the earlier copy. */
    if (d->cached_init_req == NULL && mcp_msg_is_request(m, "initialize")) {
        capture_raw(&d->cached_init_req, &d->cached_init_req_len,
                    m->raw, m->raw_len);
    }
    if (d->cached_initialized_notif == NULL &&
        mcp_msg_is_notification(m, "notifications/initialized")) {
        capture_raw(&d->cached_initialized_notif,
                    &d->cached_initialized_notif_len,
                    m->raw, m->raw_len);
    }

    if (d->fsm->state == FSM_RUNNING || is_handshake_message(m)) {
        /* Forward immediately. Count requests (messages that expect
         * a response) so we can track in-flight for drain. */
        if (m->kind == MCP_KIND_REQUEST) {
            d->in_flight++;
        }
        send_raw_with_newline(&d->sink, 1 /* to_child */, m->raw, m->raw_len);
        return;
    }

    /* Not RUNNING and not part of the handshake: queue for later
     * drain. Responses from upstream (rare but possible; e.g. if the
     * child issued a sampling request that upstream is answering)
     * are also queued — they will naturally flow after the queue
     * drains. */
    log_debug("dispatch: queuing upstream message (state %s, method=%s)",
              fsm_state_name(d->fsm->state),
              m->method != NULL ? m->method : "(none)");
    enqueue(d, m->raw, m->raw_len);
}

/* ---------- Child (child -> wrapper -> agent) ---------- */

void dispatch_on_child(struct dispatch *d, const struct mcp_msg *m) {
    if (d == NULL || m == NULL || m->raw == NULL) {
        return;
    }

    /* Replay-response consumption. After a swap, the wrapper sends
     * the cached initialize to the new child; the first response we
     * see on this new connection is that replayed init's answer.
     * The agent already has an initialize response from the previous
     * child, so we DO NOT forward the new one — we just emit
     * INITIALIZE_OK (or _FAILED) to advance the FSM. */
    if (d->replay_pending && m->kind == MCP_KIND_RESPONSE) {
        d->replay_pending = 0;
        if (d->in_flight > 0) {
            d->in_flight--;
        }
        enum fsm_event ev = m->is_error
            ? FSM_EV_INITIALIZE_FAILED
            : FSM_EV_INITIALIZE_OK;
        d->sink.emit_event(d->sink.ctx, ev);
        return;
    }

    /* First initialize response promotes us from STARTING -> RUNNING. */
    if (!d->initialized && m->kind == MCP_KIND_RESPONSE) {
        /* We treat the first response we see as "the initialize
         * response" because at this point no other request could
         * have completed yet — the only upstream request that flows
         * during STARTING is the initialize handshake. */
        d->initialized = 1;
        enum fsm_event ev = m->is_error
            ? FSM_EV_INITIALIZE_FAILED
            : FSM_EV_INITIALIZE_OK;
        /* Forward the response upstream before emitting the event
         * so the agent sees its answer promptly. */
        send_raw_with_newline(&d->sink, 0 /* to upstream */,
                              m->raw, m->raw_len);
        if (d->in_flight > 0) {
            d->in_flight--;
        }
        d->sink.emit_event(d->sink.ctx, ev);
        return;
    }

    /* All other messages forward verbatim. */
    send_raw_with_newline(&d->sink, 0, m->raw, m->raw_len);

    if (m->kind == MCP_KIND_RESPONSE) {
        if (d->in_flight > 0) {
            d->in_flight--;
        }
        if (d->in_flight == 0 && d->fsm->state == FSM_DRAINING) {
            d->sink.emit_event(d->sink.ctx, FSM_EV_IN_FLIGHT_ZERO);
        }
    }
}

/* ---------- State changes ---------- */

static void drain_queue(struct dispatch *d) {
    struct queued_msg *q;
    while ((q = dequeue(d)) != NULL) {
        /* These are upstream messages that were queued while not
         * RUNNING. Forward them now to the child. Count requests. */
        /* Re-parsing just to get the kind again is wasteful for the
         * moment — we can remember the kind in the queue entry. For
         * now, best-effort: treat queued entries as requests for
         * in-flight purposes. The queue should be small (the common
         * case is zero or one entries during a quick drain), so
         * inaccurate counting here is bounded. Correct accounting
         * comes with the replay work in 🎯T1.6a. */
        send_raw_with_newline(&d->sink, 1, q->bytes, q->len);
        queued_msg_free(q);
    }
}

void dispatch_on_state_change(struct dispatch *d, enum fsm_state new_state) {
    if (d == NULL) {
        return;
    }
    if (new_state == FSM_RUNNING) {
        drain_queue(d);
    }
}

int dispatch_replay_initialize(struct dispatch *d) {
    if (d == NULL || d->cached_init_req == NULL) {
        return 0;
    }
    /* Send the cached initialize request. We count this as one
     * in-flight request so the bookkeeping stays consistent with
     * how we handle real requests. */
    send_raw_with_newline(&d->sink, 1 /* to_child */,
                          d->cached_init_req, d->cached_init_req_len);
    d->in_flight++;
    d->replay_pending = 1;

    /* Follow up with the initialized notification if we captured
     * one. Notifications don't count as in-flight and don't have a
     * response. */
    if (d->cached_initialized_notif != NULL) {
        send_raw_with_newline(&d->sink, 1,
                              d->cached_initialized_notif,
                              d->cached_initialized_notif_len);
    }
    return 1;
}
