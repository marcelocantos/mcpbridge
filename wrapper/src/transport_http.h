/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#ifndef MCPBRIDGE_TRANSPORT_HTTP_H
#define MCPBRIDGE_TRANSPORT_HTTP_H

/* HTTP transport: speak MCP Streamable HTTP against a localhost
 * endpoint (POST + per-POST SSE responses). Intended for wrapping
 * MCP servers that listen on http://localhost:PORT/mcp rather than
 * on stdio.
 *
 * Scope for v1 (🎯T3.2):
 *   - Plain `http://` to localhost/127.0.0.1/::1 only. https and
 *     remote hosts are rejected at construction.
 *   - POST-only. Every outbound MCP message is sent as a POST; the
 *     response body may be plain application/json (single reply) or
 *     text/event-stream (one or more events streamed before the
 *     server closes the connection). Both shapes are parsed and
 *     their inbound messages routed to pump()'s callback.
 *   - NO standing GET SSE stream for server-initiated notifications.
 *     Unsolicited server pushes outside of a POST response will be
 *     missed — acceptable for v1 because session continuity across
 *     upgrades does not depend on them.
 *   - MCP-Session-Id assigned by the server on the initialize
 *     response is captured from the headers and echoed on every
 *     subsequent request.
 *   - MCP-Protocol-Version header is sent on every request.
 *
 * The transport is single-threaded. An internal self-pipe is the
 * stable poll_fd exposed to the event loop; send() writes one byte
 * to it whenever it queues inbound messages so the next poll()
 * wakes the loop and pump() drains the queue. */

#include "transport.h"

/* Create an HTTP transport for the given URL. Must be of the form
 * `http://host[:port]/path` where host is localhost / 127.0.0.1 /
 * [::1]. Scheme, host, and port are rejected otherwise; errno is
 * set to EINVAL and NULL is returned.
 *
 * The URL is copied — the caller can free its input after the call.
 * Must be started with transport_start() before any I/O.
 * Free with transport_destroy(). */
struct transport *transport_http_new(const char *url);

/* Set the per-tool-call timeout (milliseconds). Bounds how long any
 * single send() may wait for the upstream — including any retry
 * loop triggered by transient connect failures. 0 means "no timeout"
 * (retry indefinitely). Default 300000 (5 minutes). See 🎯T7.1.
 *
 * Safe to call before or after transport_start. The new timeout
 * takes effect on the next send. */
void transport_http_set_call_timeout(struct transport *t, int timeout_ms);

#endif /* MCPBRIDGE_TRANSPORT_HTTP_H */
