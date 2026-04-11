/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#ifndef MCPBRIDGE_LOG_H
#define MCPBRIDGE_LOG_H

/* Simple leveled logging to stderr.
 *
 * Thread-unsafe (matches the rest of the program — single-threaded poll
 * loop). Messages are written with a trailing newline. Lines longer than
 * LOG_LINE_MAX are truncated with a trailing "...". */

#define LOG_LINE_MAX 4096

enum log_level {
    LOG_ERROR = 0,
    LOG_WARN  = 1,
    LOG_INFO  = 2,
    LOG_DEBUG = 3,
};

/* Set the minimum level that will be printed. Default is LOG_INFO. */
void log_set_level(enum log_level lvl);

/* Returns nonzero if a message at the given level would be printed. */
int log_enabled(enum log_level lvl);

/* Emit a log message. Arguments follow printf semantics. */
void log_msg(enum log_level lvl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define log_error(...) log_msg(LOG_ERROR, __VA_ARGS__)
#define log_warn(...)  log_msg(LOG_WARN,  __VA_ARGS__)
#define log_info(...)  log_msg(LOG_INFO,  __VA_ARGS__)
#define log_debug(...) log_msg(LOG_DEBUG, __VA_ARGS__)

#endif /* MCPBRIDGE_LOG_H */
