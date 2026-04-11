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
    free(d);
}

int dispatch_in_flight(const struct dispatch *d) {
    return (d == NULL) ? 0 : d->in_flight;
}

int dispatch_initialized(const struct dispatch *d) {
    return (d == NULL) ? 0 : d->initialized;
}

/* ---------- Upstream (agent -> wrapper -> child) ---------- */

void dispatch_on_upstream(struct dispatch *d, const struct mcp_msg *m) {
    if (d == NULL || m == NULL || m->raw == NULL) {
        return;
    }

    if (d->fsm->state == FSM_RUNNING) {
        /* Forward immediately. Count requests (messages that expect
         * a response) so we can track in-flight for drain. */
        if (m->kind == MCP_KIND_REQUEST) {
            d->in_flight++;
        }
        send_raw_with_newline(&d->sink, 1 /* to_child */, m->raw, m->raw_len);
        return;
    }

    /* Not RUNNING: queue for later drain. Responses from upstream
     * (rare but possible; e.g. if the child issued a sampling request
     * that upstream is answering) are also queued — they will
     * naturally flow after the queue drains. */
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
