/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#ifndef MCPBRIDGE_DAEMON_CLIENT_H
#define MCPBRIDGE_DAEMON_CLIENT_H

/* Wrapper-side client for the mcpbridge daemon UDS.
 *
 * This module owns the socket fd, the line-framed read buffer, and
 * the per-connection sequence counter. It knows nothing about the
 * FSM, the dispatch layer, or the child transport. Higher layers
 * poll the fd, call daemon_client_try_read to get events, and call
 * daemon_client_send_* to push state back to the daemon.
 *
 * Protocol: newline-delimited JSON envelopes, schema version 1, as
 * specified in docs/wire-protocol.md. */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define DAEMON_CLIENT_PROTOCOL_VERSION 1

/* Events surfaced to the caller by try_read. */
enum daemon_event_kind {
    DAEMON_EV_NONE = 0,      /* no complete envelope buffered yet */
    DAEMON_EV_HELLO_OK,      /* peer acked our hello (handshake only) */
    DAEMON_EV_REGISTER_OK,   /* peer acked our register (handshake only) */
    DAEMON_EV_RELOAD,        /* daemon asks us to cycle our child */
    DAEMON_EV_SHUTDOWN,      /* daemon is going away */
    DAEMON_EV_ERROR,         /* protocol error from peer */
    DAEMON_EV_CLOSED,        /* socket closed by peer (EOF) */
};

struct daemon_event {
    enum daemon_event_kind kind;
    uint64_t seq;             /* envelope seq */
    uint64_t ack_seq;          /* for hello_ok / register_ok */
    char    *reason;           /* owned; NULL if absent */
    char    *name;             /* owned; NULL if absent */
    char    *code;             /* owned; for errors */
    char    *detail;           /* owned; for errors */
    int      config_found;     /* for register_ok */
    int      polling;          /* for register_ok */
};

void daemon_event_free(struct daemon_event *ev);

struct daemon_client;

/* Dial the daemon at the given socket path. Returns a heap-allocated
 * client on success, or NULL on failure (errno set: ENOENT = socket
 * file does not exist, ECONNREFUSED = daemon not running, etc.).
 *
 * The returned client is NOT yet handshaken — call
 * daemon_client_do_handshake next. */
struct daemon_client *daemon_client_new(const char *sock_path);

/* Free the client. Closes the socket if still open. */
void daemon_client_free(struct daemon_client *dc);

/* Return the raw socket fd for the event loop to poll. -1 if the
 * client has not connected or has been closed. */
int daemon_client_fd(const struct daemon_client *dc);

/* Synchronous hello + register handshake.
 *
 * Sends hello, blocks for hello_ok, sends register, blocks for
 * register_ok. Returns 0 on success. On failure returns -1 and
 * leaves the client in a state where destroy is safe (no partial
 * half-connected state the caller must worry about).
 *
 * `wrapper_version` is the mcpbridge version string.
 * `name` is the config name used by the daemon to find per-server
 * upgrade metadata.
 * `child_pid` / `child_binary` are metadata the daemon uses for
 * logging and for matching fswatch events. */
int daemon_client_do_handshake(struct daemon_client *dc,
                               const char *wrapper_version,
                               pid_t       wrapper_pid,
                               const char *name,
                               pid_t       child_pid,
                               const char *child_binary);

/* Non-blocking read of one envelope, parsed into *out. Call this
 * after poll() reports the fd readable, then repeatedly until it
 * returns DAEMON_EV_NONE.
 *
 * Returns 1 if an event was populated, 0 if there is no complete
 * envelope buffered (caller should poll for more bytes), -1 on a
 * protocol error. DAEMON_EV_CLOSED is returned as an event kind
 * (not a -1) so the caller can handle it uniformly.
 *
 * The caller owns any heap strings inside *out and must call
 * daemon_event_free when done. */
int daemon_client_try_read(struct daemon_client *dc, struct daemon_event *out);

/* Send a deregister envelope. Best-effort; ignores short writes and
 * errors because the caller is typically shutting down anyway. */
void daemon_client_send_deregister(struct daemon_client *dc, const char *reason);

/* Send a reload_ack envelope in reply to a reload whose seq was
 * ack_seq. status is one of "ok", "drain_timeout", "spawn_failed",
 * "init_failed". detail is optional. */
int daemon_client_send_reload_ack(struct daemon_client *dc,
                                  uint64_t ack_seq,
                                  const char *status,
                                  const char *detail);

#endif /* MCPBRIDGE_DAEMON_CLIENT_H */
