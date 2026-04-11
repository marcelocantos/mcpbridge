/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static enum log_level current_level = LOG_INFO;

static const char *level_name(enum log_level lvl) {
    switch (lvl) {
    case LOG_ERROR: return "ERROR";
    case LOG_WARN:  return "WARN ";
    case LOG_INFO:  return "INFO ";
    case LOG_DEBUG: return "DEBUG";
    }
    return "?????";
}

void log_set_level(enum log_level lvl) {
    current_level = lvl;
}

int log_enabled(enum log_level lvl) {
    return lvl <= current_level;
}

void log_msg(enum log_level lvl, const char *fmt, ...) {
    if (lvl > current_level) {
        return;
    }

    /* Timestamp. Best-effort: if clock_gettime fails we just print 0.000. */
    struct timespec ts = {0, 0};
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm);

    char line[LOG_LINE_MAX];
    int prefix = snprintf(line, sizeof(line), "%s.%03ld %s ",
                          time_buf, ts.tv_nsec / 1000000L, level_name(lvl));
    if (prefix < 0 || (size_t)prefix >= sizeof(line)) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    int body = vsnprintf(line + prefix, sizeof(line) - prefix, fmt, ap);
    va_end(ap);

    if (body < 0) {
        return;
    }
    size_t total = (size_t)prefix + (size_t)body;
    if (total >= sizeof(line)) {
        /* Truncate with ellipsis. */
        memcpy(line + sizeof(line) - 5, "...\n", 5);
        fputs(line, stderr);
        return;
    }

    line[total] = '\n';
    fwrite(line, 1, total + 1, stderr);
    /* Flush after every message: when stderr is redirected to a
     * file it would otherwise be fully buffered, which makes
     * diagnosing a hang impossible because the tail of the log
     * never reaches disk before the process is killed. */
    fflush(stderr);
}
