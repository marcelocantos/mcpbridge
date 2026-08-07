/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#include "dispatch.h"

#include "fsm.h"
#include "log.h"
#include "mcp.h"
#include "util.h"

#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

/* A queued upstream message, retained as raw bytes (plus a trailing
 * newline when we emit it). Kept simple: singly-linked FIFO. */
struct queued_msg {
    struct queued_msg *next;
    char              *bytes;  /* owned; raw message body, no newline */
    size_t             len;
};

/* One request forwarded to the child and not yet answered. We keep
 * the id, not just a count, because a backend that goes away owes the
 * agent a response for each of these and we cannot write one without
 * knowing which id to address it to (🎯T21). */
struct inflight_req {
    struct inflight_req *next;
    struct mcp_id        id;
};

struct dispatch {
    struct fsm          *fsm;
    struct dispatch_sink sink;

    /* Head of the outstanding-request list; in_flight is its length,
     * kept in step so the hot path stays a plain int compare. */
    struct inflight_req *inflight_head;
    struct inflight_req *inflight_tail;
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

    /* Backend identity for -32001 error envelopes (🎯T10). Borrowed
     * pointers; lifetime tied to the cfg object (outlives dispatch).
     * NULL for stdio backends — no data field is emitted in that case. */
    const char *backend_name;
    const char *backend_url;
};

/* Returns DISPATCH_SEND_OK / _STALE / _FATAL when to_child is set;
 * always DISPATCH_SEND_OK for the upstream direction (failures there
 * abort the wrapper synchronously inside the sink and never surface
 * as a return code). */
static int send_raw_with_newline(const struct dispatch_sink *sink,
                                 int to_child,
                                 const void *bytes, size_t n) {
    /* The parser strips the trailing newline from the raw bytes.
     * When we forward, we need to re-append it so the downstream
     * framing is correct. We deliver the whole message (bytes +
     * newline) in a SINGLE sink call — the HTTP backend treats
     * each send as one complete POST and would otherwise fire a
     * spurious one-byte POST for the trailing newline. A scratch
     * copy is cheap; localhost throughput is not the bottleneck. */
    char *scratch = xmalloc(n + 1);
    memcpy(scratch, bytes, n);
    scratch[n] = '\n';
    int rc = DISPATCH_SEND_OK;
    if (to_child) {
        rc = sink->send_child(sink->ctx, scratch, n + 1);
    } else {
        sink->send_upstream(sink->ctx, scratch, n + 1);
    }
    free(scratch);
    return rc;
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

/* ---------- In-flight request tracking ---------- */

/* Deep-copy an id into freshly owned storage. */
static void id_copy(struct mcp_id *dst, const struct mcp_id *src) {
    dst->tag = src->tag;
    if (src->tag == MCP_ID_STRING && src->v.s != NULL) {
        dst->v.s = xstrdup(src->v.s);
    } else {
        dst->v = src->v;
    }
}

static void inflight_push(struct dispatch *d, const struct mcp_id *id) {
    struct inflight_req *r = xcalloc(1, sizeof(*r));
    id_copy(&r->id, id);
    if (d->inflight_tail == NULL) {
        d->inflight_head = r;
    } else {
        d->inflight_tail->next = r;
    }
    d->inflight_tail = r;
    d->in_flight++;
}

static void inflight_req_free(struct inflight_req *r) {
    if (r == NULL) {
        return;
    }
    mcp_id_free(&r->id);
    free(r);
}

/* Drop the entry matching id, if present. Returns 1 if one was
 * removed. An unmatched response is not an error — the child may
 * answer something we never counted (notably the replayed
 * initialize) — so the caller keeps its old floor-at-zero behaviour
 * for the count. */
static int inflight_remove(struct dispatch *d, const struct mcp_id *id) {
    struct inflight_req *prev = NULL;
    for (struct inflight_req *r = d->inflight_head; r != NULL; r = r->next) {
        if (mcp_id_eq(&r->id, id)) {
            if (prev == NULL) {
                d->inflight_head = r->next;
            } else {
                prev->next = r->next;
            }
            if (d->inflight_tail == r) {
                d->inflight_tail = prev;
            }
            inflight_req_free(r);
            if (d->in_flight > 0) {
                d->in_flight--;
            }
            return 1;
        }
        prev = r;
    }
    return 0;
}

static void inflight_clear(struct dispatch *d) {
    struct inflight_req *r = d->inflight_head;
    while (r != NULL) {
        struct inflight_req *next = r->next;
        inflight_req_free(r);
        r = next;
    }
    d->inflight_head = NULL;
    d->inflight_tail = NULL;
    d->in_flight = 0;
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
    inflight_clear(d);
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

void dispatch_set_backend_id(struct dispatch *d,
                             const char *name,
                             const char *url) {
    if (d == NULL) {
        return;
    }
    d->backend_name = name;
    d->backend_url  = url;
}

/* Build the error.data object for a -32001 envelope (🎯T10).
 * Returns NULL if the dispatch has no backend identity (stdio), or if
 * both name and url are NULL/empty (defensive). Caller passes the
 * returned pointer to mcp_build_error_response, which takes ownership. */
static cJSON *build_backend_data(const struct dispatch *d) {
    if (d->backend_name == NULL && d->backend_url == NULL) {
        return NULL;
    }
    cJSON *backend = cJSON_CreateObject();
    if (backend == NULL) {
        return NULL;
    }
    if (d->backend_name != NULL && d->backend_name[0] != '\0') {
        cJSON_AddStringToObject(backend, "name", d->backend_name);
    }
    if (d->backend_url != NULL && d->backend_url[0] != '\0') {
        cJSON_AddStringToObject(backend, "url", d->backend_url);
    }
    /* If both fields ended up empty/NULL, free the partial object. */
    if (cJSON_GetArraySize(backend) == 0) {
        cJSON_Delete(backend);
        return NULL;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        cJSON_Delete(backend);
        return NULL;
    }
    cJSON_AddItemToObject(data, "backend", backend);
    return data;
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

/* MCP methods that are safe to replay against a freshly re-handshaken
 * upstream. ESTALE only fires when the upstream rejected the request
 * with a 4xx before processing it — but mcpbridge cannot prove that
 * property in general for arbitrary servers, so it limits automatic
 * replay to methods whose contract is explicitly read-only. Side-
 * effecting requests (notably tools/call and sampling/createMessage)
 * are surfaced to the agent as a structured recoverable error so the
 * agent can decide whether retrying is appropriate. */
static int is_replay_safe_method(const char *method) {
    if (method == NULL) {
        return 0;
    }
    static const char *const safe[] = {
        "initialize",
        "notifications/initialized",
        "ping",
        "tools/list",
        "prompts/list",
        "prompts/get",
        "resources/list",
        "resources/read",
        "resources/templates/list",
        "resources/subscribe",
        "resources/unsubscribe",
        "roots/list",
        "logging/setLevel",
        "completion/complete",
    };
    size_t n = sizeof(safe) / sizeof(safe[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(method, safe[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

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
            inflight_push(d, &m->id);
        }
        int rc = send_raw_with_newline(&d->sink, 1 /* to_child */,
                                       m->raw, m->raw_len);
        if (rc == DISPATCH_SEND_TIMEOUT) {
            /* The upstream stayed unreachable past the per-tool-call
             * timeout. Synthesise a structured JSON-RPC error to
             * the agent so it sees a normal failure for THIS call,
             * and keep the wrapper alive — the next call resumes
             * normal flow as soon as the upstream comes back
             * (🎯T7.1). Notifications have no id; we just drop them
             * and log. */
            if (m->kind == MCP_KIND_REQUEST) {
                inflight_remove(d, &m->id);
                size_t elen = 0;
                char *err = mcp_build_error_response(
                    &m->id, -32001,
                    "mcpbridge: upstream unreachable past timeout",
                    build_backend_data(d),
                    &elen);
                if (err != NULL) {
                    d->sink.send_upstream(d->sink.ctx, err, elen);
                    free(err);
                }
            } else {
                log_warn("dispatch: dropped %s notification (upstream "
                         "unreachable past timeout)",
                         m->method != NULL ? m->method : "(unknown)");
            }
            return;
        }
        if (rc == DISPATCH_SEND_UPSTREAM_ERROR) {
            /* A single upstream request failed but the transport is
             * still usable (HTTP 5xx, truncated/reset body, malformed
             * framing, or a refused connection to a restarting
             * backend). Surface a JSON-RPC error for THIS request and
             * keep the wrapper alive so a later call reaches the
             * recovered backend — mirroring the timeout path. Drop
             * notifications (no id to answer). 🎯T17. */
            if (m->kind == MCP_KIND_REQUEST) {
                d->in_flight--;
                size_t elen = 0;
                char *err = mcp_build_error_response(
                    &m->id, -32003,
                    "mcpbridge: upstream request failed",
                    build_backend_data(d),
                    &elen);
                if (err != NULL) {
                    d->sink.send_upstream(d->sink.ctx, err, elen);
                    free(err);
                }
            } else {
                log_warn("dispatch: dropped %s notification (upstream "
                         "request failed)",
                         m->method != NULL ? m->method : "(unknown)");
            }
            return;
        }
        if (rc == DISPATCH_SEND_STALE) {
            /* Session was invalidated mid-flight (e.g. the upstream
             * MCP server restarted and rejected our session id). The
             * request never reached a state where the upstream could
             * answer it. Undo the in-flight count and trigger a
             * reload — the existing reload pathway re-handshakes the
             * upstream (capturing a fresh session id).
             *
             * Idempotent reads (tools/list, resources/read, etc.) get
             * queued for transparent replay after the new handshake.
             * Side-effecting requests (tools/call,
             * sampling/createMessage, anything mcpbridge can't prove
             * read-only) surface a structured recoverable error to
             * the agent rather than being silently re-issued — even
             * though the upstream's 4xx implies the request was not
             * processed, mcpbridge cannot guarantee that across
             * arbitrary backends, and silently retrying writes
             * violates the agent's expectation of at-most-once
             * delivery for tool calls. */
            if (m->kind == MCP_KIND_REQUEST) {
                inflight_remove(d, &m->id);
            }
            int replay = (m->kind == MCP_KIND_NOTIFICATION)
                      || is_replay_safe_method(m->method);
            if (replay) {
                log_info("upstream: cycling — session stale; queuing %s "
                         "for replay after re-init",
                         m->method != NULL ? m->method : "(notification)");
                enqueue(d, m->raw, m->raw_len);
            } else {
                log_info("upstream: cycling — session stale; surfacing "
                         "recoverable error for %s (not safe to replay)",
                         m->method);
                size_t elen = 0;
                char *err = mcp_build_error_response(
                    &m->id, -32002,
                    "mcpbridge: upstream session reset during call; "
                    "please retry",
                    NULL,
                    &elen);
                if (err != NULL) {
                    d->sink.send_upstream(d->sink.ctx, err, elen);
                    free(err);
                }
            }
            d->sink.emit_event(d->sink.ctx, FSM_EV_RELOAD_REQUESTED);
        }
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

/* Send a list_changed notification of the given kind to upstream.
 * Used after a successful replay to prompt the agent to refetch
 * tools / prompts / resources from the new child. Silently no-ops
 * on allocation failure or an unknown kind — the agent will still
 * see correct output on its next explicit fetch; the notification
 * is a best-effort prompt, not a correctness requirement. */
static void emit_list_changed_upstream(struct dispatch *d, const char *kind) {
    size_t len = 0;
    char *buf = mcp_build_list_changed(kind, &len);
    if (buf == NULL) {
        return;
    }
    d->sink.send_upstream(d->sink.ctx, buf, len);
    free(buf);
}

void dispatch_on_child(struct dispatch *d, const struct mcp_msg *m) {
    if (d == NULL || m == NULL || m->raw == NULL) {
        return;
    }

    /* Replay-response consumption. After a swap, the wrapper sends
     * the cached initialize to the new child; the first response we
     * see on this new connection is that replayed init's answer.
     * The agent already has an initialize response from the previous
     * child, so we DO NOT forward the new one — we just emit
     * INITIALIZE_OK (or _FAILED) to advance the FSM.
     *
     * On a successful replay, the new child may expose a different
     * tools / prompts / resources surface than the one the agent
     * saw originally. MCP has standard notifications for each of
     * those — emit all three unconditionally. The agent reacts by
     * re-fetching each list, which is cheap and avoids the wrapper
     * having to maintain its own cached view for diffing. Changes
     * to capabilities or protocolVersion have no MCP notification
     * to carry them and are documented as unsupported across a
     * reload — see STABILITY.md. */
    if (d->replay_pending && m->kind == MCP_KIND_RESPONSE) {
        d->replay_pending = 0;
        if (d->in_flight > 0) {
            d->in_flight--;
        }
        if (!m->is_error) {
            emit_list_changed_upstream(d, "tools");
            emit_list_changed_upstream(d, "prompts");
            emit_list_changed_upstream(d, "resources");
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
        if (!inflight_remove(d, &m->id) && d->in_flight > 0) {
            /* A response we never counted (e.g. a request forwarded
             * from the queue before id tracking existed for it).
             * Preserve the historical floor-at-zero behaviour. */
            d->in_flight--;
        }
        if (d->in_flight == 0 && d->fsm->state == FSM_DRAINING) {
            d->sink.emit_event(d->sink.ctx, FSM_EV_IN_FLIGHT_ZERO);
        }
    }
}

void dispatch_settle_in_flight(struct dispatch *d, const char *reason) {
    if (d == NULL) {
        return;
    }
    int n = 0;
    for (struct inflight_req *r = d->inflight_head; r != NULL; r = r->next) {
        n++;
        size_t elen = 0;
        char *err = mcp_build_error_response(
            &r->id, -32003,
            reason != NULL ? reason
                           : "mcpbridge: backend cycled during call; "
                             "please retry",
            build_backend_data(d),
            &elen);
        if (err != NULL) {
            d->sink.send_upstream(d->sink.ctx, err, elen);
            free(err);
        }
    }
    /* Clears the list AND zeroes the count. Zeroing matters even when
     * the list is empty: the replayed initialize is counted without a
     * list entry (it is the wrapper's own request, not the agent's),
     * so a swap landing while one is outstanding would otherwise leave
     * in_flight stuck at 1 with nothing left to decrement it — the
     * same wedge by another route. */
    inflight_clear(d);
    d->replay_pending = 0;
    if (n > 0) {
        log_info("dispatch: answered %d abandoned request(s) with an "
                 "error so the agent is not left waiting", n);
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
        return;
    }
    if (new_state == FSM_DRAINING && d->in_flight == 0) {
        /* Entering DRAINING with nothing already in flight: there is
         * literally nothing to drain, so post IN_FLIGHT_ZERO
         * immediately. Without this the wrapper sits in DRAINING
         * forever because IN_FLIGHT_ZERO is otherwise only emitted
         * by dispatch_on_child when the last response decrements
         * in_flight to zero. */
        d->sink.emit_event(d->sink.ctx, FSM_EV_IN_FLIGHT_ZERO);
        return;
    }
}

/* Drain every queued upstream message, synthesising a JSON-RPC
 * error for each REQUEST and dropping notifications. Used when a
 * recovery cycle gives up because the upstream is unreachable past
 * the per-call timeout — the agent sees a structured failure for
 * each call that was waiting on the recovery, and the wrapper stays
 * up so subsequent calls can try again.
 *
 * The error message is opaque to dispatch; the caller supplies it
 * because the *reason* differs by recovery path (timeout vs failed
 * handshake). */
static void drain_queue_with_error(struct dispatch *d,
                                   int code,
                                   const char *message) {
    struct queued_msg *q;
    while ((q = dequeue(d)) != NULL) {
        struct mcp_msg m = {0};
        if (mcp_msg_parse(q->bytes, q->len, &m) == MCP_PARSE_OK
         && m.kind == MCP_KIND_REQUEST) {
            /* For -32001 envelopes, include backend identity so the
             * agent knows which backend failed (🎯T10). */
            cJSON *data = (code == -32001) ? build_backend_data(d) : NULL;
            size_t elen = 0;
            char *err = mcp_build_error_response(&m.id, code, message,
                                                 data, &elen);
            if (err != NULL) {
                d->sink.send_upstream(d->sink.ctx, err, elen);
                free(err);
            }
        } else {
            log_warn("dispatch: dropping queued %s during error drain",
                     m.method != NULL ? m.method : "(unparseable)");
        }
        mcp_msg_free(&m);
        queued_msg_free(q);
    }
}

int dispatch_replay_initialize(struct dispatch *d) {
    if (d == NULL || d->cached_init_req == NULL) {
        return 0;
    }
    /* Send the cached initialize request. We count this as one
     * in-flight request so the bookkeeping stays consistent with
     * how we handle real requests. */
    int rc = send_raw_with_newline(&d->sink, 1 /* to_child */,
                                   d->cached_init_req,
                                   d->cached_init_req_len);
    if (rc == DISPATCH_SEND_TIMEOUT || rc == DISPATCH_SEND_UPSTREAM_ERROR) {
        /* The re-initialize handshake itself failed: either the
         * upstream stayed unreachable past the per-call timeout
         * (🎯T7.1) or the backend returned a per-request failure such
         * as a 5xx / truncated body (🎯T17). Either way the transport
         * survives, so abort the recovery cleanly: synthesise a
         * structured error for every queued upstream request (the
         * calls waiting on this recovery) and let the caller drive the
         * FSM back to RUNNING. The wrapper stays alive; the next agent
         * request triggers a fresh recovery cycle. */
        int code = (rc == DISPATCH_SEND_TIMEOUT) ? -32001 : -32003;
        const char *msg = (rc == DISPATCH_SEND_TIMEOUT)
            ? "mcpbridge: upstream unreachable past timeout during reinit"
            : "mcpbridge: upstream request failed during reinit";
        log_warn("dispatch: replay initialize failed (%s); "
                 "draining %s queue with errors",
                 rc == DISPATCH_SEND_TIMEOUT ? "timeout" : "upstream error",
                 d->queue_head != NULL ? "non-empty" : "empty");
        drain_queue_with_error(d, code, msg);
        /* No bytes landed, so no response is coming — don't leave
         * replay_pending set; don't bump in_flight. */
        return -1;
    }
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
