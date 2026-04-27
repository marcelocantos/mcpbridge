/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#ifndef MCPBRIDGE_MCP_H
#define MCPBRIDGE_MCP_H

/* MCP framing, parsing, and message construction.
 *
 * MCP over stdio is newline-delimited JSON-RPC 2.0: one complete JSON
 * object per line, terminated by a single '\n'. This module owns the
 * byte-level framing, the top-level envelope parsing (kind, method,
 * id), and the small amount of message construction the wrapper needs
 * to do on its own (currently just notifications/tools/list_changed).
 *
 * Design principle: forwarding is the common case. For every parsed
 * message we retain the original bytes so the wrapper can forward
 * verbatim without re-serialising. The parsed view is a routing hint,
 * not the canonical representation.
 *
 * This module is pure: no I/O, no globals, no state-machine
 * dependencies. The event loop owns the fds and feeds bytes in. */

#include <stddef.h>
#include <stdint.h>

/* Default line cap for MCP stdio messages. 4 MiB per the design
 * (large enough for any reasonable tool payload, small enough that
 * a malicious or broken child cannot exhaust memory). */
#define MCP_LINE_MAX_DEFAULT (4u * 1024u * 1024u)

/* ---------- Envelope classification ---------- */

enum mcp_kind {
    MCP_KIND_UNKNOWN = 0,
    MCP_KIND_REQUEST,      /* has method AND id */
    MCP_KIND_RESPONSE,     /* has id AND (result or error), no method */
    MCP_KIND_NOTIFICATION, /* has method, no id */
};

enum mcp_id_tag {
    MCP_ID_NONE = 0,
    MCP_ID_INT,
    MCP_ID_STRING,
};

/* A JSON-RPC id as seen on the wire. Can be an integer, a string, or
 * absent entirely (for notifications). Floats are rejected by the
 * parser — the MCP spec allows them, but neither Claude Code nor any
 * known MCP server uses them, and permitting them forces awkward
 * equality semantics on id tracking. */
struct mcp_id {
    enum mcp_id_tag tag;
    union {
        int64_t i;
        char   *s; /* owned; NUL-terminated */
    } v;
};

/* Free any owned storage inside an mcp_id. Safe on zeroed structs. */
void mcp_id_free(struct mcp_id *id);

/* Lexical equality. Two ids are equal iff same tag and same value.
 * MCP_ID_NONE compares equal only to MCP_ID_NONE. */
int mcp_id_eq(const struct mcp_id *a, const struct mcp_id *b);

/* ---------- Parsed message ---------- */

/* A parsed MCP message. Owns its raw bytes (a copy of the original
 * line without the trailing newline) and, if it is a notification or
 * request, its method string. The raw bytes are what the wrapper
 * forwards; the parsed fields are what the wrapper uses to decide
 * how to route the forward. */
struct mcp_msg {
    enum mcp_kind kind;
    struct mcp_id id;
    char  *method;  /* owned; NUL-terminated; NULL for responses */
    char  *raw;     /* owned; not NUL-terminated */
    size_t raw_len;
    int    is_error; /* response only: 1 if the envelope carried an
                       * "error" field rather than a "result" field */
};

/* Parse one complete MCP message from a byte slice.
 *
 * The slice must NOT contain a trailing newline. On success, *out is
 * populated with owned copies of the interesting fields and must be
 * freed with mcp_msg_free(). On failure, *out is left zeroed and the
 * function returns one of the MCP_PARSE_* error codes below. */
#define MCP_PARSE_OK              0
#define MCP_PARSE_ERR_JSON       -1 /* malformed JSON */
#define MCP_PARSE_ERR_NOT_OBJECT -2 /* top-level not a JSON object */
#define MCP_PARSE_ERR_BAD_ID     -3 /* id present but not int or string */
#define MCP_PARSE_ERR_BAD_METHOD -4 /* method present but not a string */
#define MCP_PARSE_ERR_INCOHERENT -5 /* kind cannot be determined */

int mcp_msg_parse(const char *bytes, size_t len, struct mcp_msg *out);

/* Free any owned storage inside an mcp_msg. Safe on zeroed structs.
 * Safe to call multiple times on the same struct (it is re-zeroed). */
void mcp_msg_free(struct mcp_msg *m);

/* Convenience classifiers. Safe on NULL. */
int mcp_msg_is_request(const struct mcp_msg *m, const char *method);
int mcp_msg_is_notification(const struct mcp_msg *m, const char *method);

/* ---------- Streaming line reader ---------- */

/* Accumulates bytes from a read()-like source into an internal buffer
 * and yields complete lines. When a line would exceed `max`, the
 * reader enters a "dropping" state and discards bytes until the next
 * newline, at which point it resumes normal operation and the next
 * mcp_reader_pop() call returns MCP_READER_TOO_LONG exactly once.
 *
 * The reader uses a head/tail offset pair rather than compacting on
 * every pop, which lets popped line pointers remain valid until the
 * next *feed* (the only operation that may trigger compaction). */
struct mcp_reader {
    char   *buf;
    size_t  cap;
    size_t  head;              /* offset of first live byte */
    size_t  len;               /* offset one past last live byte */
    size_t  max;
    int     dropping;          /* nonzero while discarding an oversized line */
    int     overflow_pending;  /* one-shot: next pop() returns TOO_LONG */
};

/* Initialise a reader with the given line cap. Pass 0 for the default
 * (MCP_LINE_MAX_DEFAULT). */
void mcp_reader_init(struct mcp_reader *r, size_t max);

/* Free the reader's internal buffer. Safe on zeroed readers. */
void mcp_reader_free(struct mcp_reader *r);

/* Feed bytes read from a source fd into the reader.
 * Returns 0 always; the reader's buffer may grow by up to `n` bytes.
 * This function never reports errors itself — it just accumulates. */
void mcp_reader_feed(struct mcp_reader *r, const char *bytes, size_t n);

/* Pop one complete line from the reader. The returned pointer points
 * into the reader's internal buffer and is only valid until the next
 * feed() call that triggers compaction or growth — in practice,
 * consume the pointer before the next feed.
 *
 * Returns one of:
 *   MCP_READER_OK         — line and len populated, advance and retry
 *   MCP_READER_EMPTY      — no complete line yet, feed more bytes
 *   MCP_READER_TOO_LONG   — an earlier line exceeded max and was dropped
 *
 * When both complete lines and pending overflows are available, lines
 * are drained first. This means an overflow may be reported after
 * lines that chronologically followed it on the wire. The wrapper
 * treats TOO_LONG as a log-and-continue event, so this ordering is
 * benign for MCP. */
#define MCP_READER_OK        1
#define MCP_READER_EMPTY     0
#define MCP_READER_TOO_LONG -1

int mcp_reader_pop(struct mcp_reader *r, const char **line, size_t *len);

/* ---------- Message construction ---------- */

/* Build a list_changed notification for the given MCP namespace.
 * `kind` must be one of "tools", "prompts", "resources"; any other
 * value returns NULL. The returned buffer is a complete envelope
 * terminated by a single '\n', ready to write to a socket. Caller
 * owns the buffer (free() when done). out_len, if non-NULL,
 * receives the buffer length in bytes (including the newline). */
char *mcp_build_list_changed(const char *kind, size_t *out_len);

/* Build a JSON-RPC error response for the given request id, with the
 * given error code and message. Used by the wrapper to synthesise a
 * structured failure when the upstream is unreachable beyond the
 * per-tool-call timeout (🎯T7.1) — the agent receives a normal
 * error reply for that one call instead of losing its session.
 *
 * `id` must be either MCP_ID_INT or MCP_ID_STRING (notifications
 * have no id, so passing MCP_ID_NONE returns NULL). `message` must
 * be a UTF-8 string with no control characters; characters outside
 * the JSON-safe range are not escaped beyond what cJSON does
 * automatically. Returns a buffer terminated by a single '\n'.
 * Caller owns the buffer (free() when done). out_len, if non-NULL,
 * receives the buffer length including the newline. Returns NULL on
 * allocation failure or invalid id. */
char *mcp_build_error_response(const struct mcp_id *id,
                               int code,
                               const char *message,
                               size_t *out_len);

#endif /* MCPBRIDGE_MCP_H */
