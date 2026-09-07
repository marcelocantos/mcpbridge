/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

/* Benchmarks for the per-message path, plus the gate that locks their
 * numbers in both directions.
 *
 *     make bench             run them and print the results
 *     make bench-lock        make this run the new baseline
 *     make bench-gate        compare against docs/perf/baseline.txt
 *     make bench-gate-loose  compare only the counted metrics
 *
 * Every MCP message an agent sends or receives crosses this wrapper,
 * so mcp_msg_parse runs on the critical path of every tool call. What
 * it costs is a function of message size, and tool results are the big
 * ones: a directory listing or a file read is kilobytes, and a large
 * one is hundreds of kilobytes.
 *
 * Two kinds of number come out of a run:
 *
 *   - Timings are the FLOOR over many rounds, never an average. Load on
 *     the machine can only ever make a round slower, so the fastest
 *     round is the closest thing to an uncontended measurement. They
 *     are still only comparable against a baseline recorded on the same
 *     machine, so they carry a wide tolerance and are skipped entirely
 *     under --loose.
 *   - Counted metrics are exact and identical everywhere. The one that
 *     matters here is `allocs`: how many times parsing one message
 *     calls malloc, counted by handing cJSON its own allocator. It is
 *     the metric that says whether the parser is building a tree it
 *     immediately throws away.
 *
 * There is no benchmark framework here on purpose; the harness is this
 * file, which is the same reason the tests have no framework either. */

#include "../src/mcp.h"

#include "cJSON.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* How long each benchmark is sampled for before its per-iteration cost
 * is reported. */
static const double SAMPLE_SECONDS = 0.5;

/* Every benchmark runs at least this many rounds, however slow it is,
 * so its floor is taken over more than a couple of samples. */
static const int MIN_ROUNDS = 8;

/* The shortest a timed round may be, in seconds. Iterations are batched
 * up to this so the clock's granularity does not become the
 * measurement. */
static const double MIN_ROUND_SECONDS = 0.001;

/* Timing tolerance, as a fraction of the baseline. Deliberately wide: a
 * gate that fails on scheduler noise gets switched off, which is worse
 * than a coarse one. */
static const double TIMING_TOLERANCE = 0.25;

/* Where the locked baseline lives, relative to the repository root.
 * The bench binary is run from wrapper/, so the path is relative to
 * that. */
static const char *BASELINE = "../docs/perf/baseline.txt";

#define MAX_METRICS 4
#define MAX_BENCHES 8

/* ---------- allocation counting ---------- */

/* cJSON takes its allocator through cJSON_InitHooks, which is how the
 * benchmark counts allocations without the product knowing it is being
 * measured. Counting malloc calls rather than bytes is deliberate: the
 * cost of building a parse tree is one allocation per node, and the
 * node count is what tracks message size. */
static uint64_t alloc_count = 0;

static void *counting_malloc(size_t n) {
    alloc_count++;
    return malloc(n);
}

static void counting_free(void *p) { free(p); }

/* ---------- corpus ---------- */

/* The messages the wrapper actually carries, smallest to largest. The
 * shapes are real JSON-RPC 2.0: a notification with no id, a request,
 * and responses whose result is a large string — which is what a tool
 * result is. */
struct message {
    const char *name;
    char       *bytes;
    size_t      len;
};

/* Build a response whose result.content[0].text is `payload_len` bytes
 * of plausible text. The wrapper only ever looks at the top level, so
 * the nesting below it is what a real tool result has: an array of
 * content blocks. */
static struct message make_tool_result(const char *name, size_t payload_len) {
    const char *prefix =
        "{\"jsonrpc\":\"2.0\",\"id\":4242,\"result\":{\"content\":"
        "[{\"type\":\"text\",\"text\":\"";
    const char *suffix = "\"}],\"isError\":false}}";
    size_t plen = strlen(prefix), slen = strlen(suffix);
    char  *buf  = malloc(plen + payload_len + slen + 1);
    if (buf == NULL) {
        fprintf(stderr, "bench: out of memory\n");
        exit(2);
    }
    memcpy(buf, prefix, plen);
    /* Printable, escape-free filler: whatever a file read returns is
     * mostly this, and an escape would only make cJSON slower. */
    for (size_t i = 0; i < payload_len; i++) {
        buf[plen + i] = (char)('a' + (int)(i % 26));
    }
    memcpy(buf + plen + payload_len, suffix, slen);
    buf[plen + payload_len + slen] = '\0';
    struct message m = {name, buf, plen + payload_len + slen};
    return m;
}

static struct message make_literal(const char *name, const char *json) {
    struct message m = {name, strdup(json), strlen(json)};
    if (m.bytes == NULL) {
        fprintf(stderr, "bench: out of memory\n");
        exit(2);
    }
    return m;
}

/* ---------- harness ---------- */

struct metric {
    const char *name;
    double      value;
};

struct sample {
    const char   *name;
    double        ns;
    struct metric counted[MAX_METRICS];
    int           n_counted;
};

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void add_metric(struct sample *s, const char *name, double value) {
    if (s->n_counted >= MAX_METRICS) {
        return;
    }
    s->counted[s->n_counted].name  = name;
    s->counted[s->n_counted].value = value;
    s->n_counted++;
}

/* Parse one message and free everything it produced. This is the unit
 * under test: exactly what the dispatch path does per message. */
static void parse_once(const struct message *m) {
    struct mcp_msg msg;
    int rc = mcp_msg_parse(m->bytes, m->len, &msg);
    if (rc != MCP_PARSE_OK) {
        fprintf(stderr, "bench: %s failed to parse (rc=%d)\n", m->name, rc);
        exit(2);
    }
    mcp_msg_free(&msg);
}

/* Run parse_once over `m` repeatedly and report the floor of the
 * per-iteration cost, plus the allocations one parse makes. */
static struct sample bench_parse(const struct message *m) {
    struct sample s;
    memset(&s, 0, sizeof(s));
    s.name = m->name;

    /* One untimed pass so a first-call cost is not charged to the
     * measurement, and one counted pass for the allocation figure. */
    parse_once(m);
    uint64_t before = alloc_count;
    parse_once(m);
    double allocs = (double)(alloc_count - before);

    double probe_start = now_seconds();
    parse_once(m);
    double single = now_seconds() - probe_start;
    if (single <= 0) {
        single = 1e-9;
    }
    int batch = (int)(MIN_ROUND_SECONDS / single) + 1;

    double start = now_seconds();
    double floor = -1;
    int    rounds = 0;
    for (;;) {
        double round_start = now_seconds();
        for (int i = 0; i < batch; i++) {
            parse_once(m);
        }
        double per_iteration = (now_seconds() - round_start) / batch;
        if (floor < 0 || per_iteration < floor) {
            floor = per_iteration;
        }
        rounds++;
        if (rounds >= MIN_ROUNDS && now_seconds() - start >= SAMPLE_SECONDS) {
            break;
        }
    }

    s.ns = floor * 1e9;
    add_metric(&s, "allocs", allocs);
    add_metric(&s, "message-bytes", (double)m->len);
    return s;
}

/* Attribution benchmarks: the two halves of what mcp_msg_parse does to
 * a large message, measured separately so the total can be read as a
 * sum rather than a mystery. Same role the spawn control plays in a
 * process benchmark. */
static struct sample bench_named(const char *name, const struct message *m,
                                 void (*fn)(const struct message *)) {
    struct sample s;
    memset(&s, 0, sizeof(s));
    s.name = name;

    fn(m);
    uint64_t before = alloc_count;
    fn(m);
    double allocs = (double)(alloc_count - before);

    double probe_start = now_seconds();
    fn(m);
    double single = now_seconds() - probe_start;
    if (single <= 0) {
        single = 1e-9;
    }
    int batch = (int)(MIN_ROUND_SECONDS / single) + 1;

    double start  = now_seconds();
    double floor  = -1;
    int    rounds = 0;
    for (;;) {
        double round_start = now_seconds();
        for (int i = 0; i < batch; i++) {
            fn(m);
        }
        double per_iteration = (now_seconds() - round_start) / batch;
        if (floor < 0 || per_iteration < floor) {
            floor = per_iteration;
        }
        rounds++;
        if (rounds >= MIN_ROUNDS && now_seconds() - start >= SAMPLE_SECONDS) {
            break;
        }
    }
    s.ns = floor * 1e9;
    add_metric(&s, "allocs", allocs);
    add_metric(&s, "message-bytes", (double)m->len);
    return s;
}

/* Just the cJSON parse and free — the tree mcp_msg_parse builds and
 * then throws away. */
static void json_only(const struct message *m) {
    cJSON *root = cJSON_ParseWithLength(m->bytes, m->len);
    if (root == NULL) {
        fprintf(stderr, "bench: json_only failed on %s\n", m->name);
        exit(2);
    }
    cJSON_Delete(root);
}

/* Just the retained copy of the raw bytes that the wrapper forwards
 * verbatim. This is the irreducible part: the message has to be kept.
 *
 * The sink is not decoration. Without it the optimiser deletes the
 * whole function — malloc, memcpy and free together — and the benchmark
 * reports one nanosecond, which is how this measurement first came out. */
static volatile char copy_sink;

static void copy_only(const struct message *m) {
    char *p = malloc(m->len);
    if (p == NULL) {
        fprintf(stderr, "bench: out of memory\n");
        exit(2);
    }
    memcpy(p, m->bytes, m->len);
    copy_sink = p[m->len - 1];
    free(p);
}

/* ---------- reporting and the gate ---------- */

static void render(const struct sample *results, int n, FILE *out) {
    fprintf(out, "# mcpbridge benchmarks — see docs/perf/baseline.md\n");
    for (int i = 0; i < n; i++) {
        fprintf(out, "%s\t%.0f ns/op", results[i].name, results[i].ns);
        for (int j = 0; j < results[i].n_counted; j++) {
            fprintf(out, "\t%.0f %s", results[i].counted[j].value,
                    results[i].counted[j].name);
        }
        fprintf(out, "\n");
    }
}

/* Parse a rendered baseline back into samples. Tab-separated, one line
 * per benchmark, each field "<value> <unit>". */
static int parse_baseline(const char *path, struct sample *out, int max) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "bench: cannot read %s\n", path);
        exit(2);
    }
    char line[4096];
    int  n = 0;
    while (n < max && fgets(line, sizeof(line), f) != NULL) {
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        struct sample s;
        memset(&s, 0, sizeof(s));
        char *save = NULL;
        char *tok  = strtok_r(line, "\t", &save);
        if (tok == NULL) {
            continue;
        }
        s.name = strdup(tok);
        while ((tok = strtok_r(NULL, "\t", &save)) != NULL) {
            double      value = 0;
            char        unit[64];
            if (sscanf(tok, "%lf %63s", &value, unit) != 2) {
                continue;
            }
            if (strcmp(unit, "ns/op") == 0) {
                s.ns = value;
            } else {
                add_metric(&s, strdup(unit), value);
            }
        }
        out[n++] = s;
    }
    fclose(f);
    return n;
}

/* Compare a fresh run against the baseline. Returns 0 when everything
 * is within tolerance in both directions. */
static int compare(const struct sample *base, int n_base,
                   const struct sample *got, int n_got, int loose) {
    int failed = 0;
    printf("\n");
    for (int i = 0; i < n_base; i++) {
        const struct sample *b = &base[i];
        const struct sample *g = NULL;
        for (int j = 0; j < n_got; j++) {
            if (strcmp(got[j].name, b->name) == 0) {
                g = &got[j];
                break;
            }
        }
        if (g == NULL) {
            printf("%-22s MISSING from this run\n", b->name);
            failed++;
            continue;
        }
        for (int k = 0; k < b->n_counted; k++) {
            const char *metric = b->counted[k].name;
            double      want   = b->counted[k].value;
            double      have   = -1;
            for (int m = 0; m < g->n_counted; m++) {
                if (strcmp(g->counted[m].name, metric) == 0) {
                    have = g->counted[m].value;
                }
            }
            if (have != want) {
                printf("%-22s %s: locked %.0f, measured %.0f — a rise is a "
                       "regression, a fall must be re-locked in the same "
                       "commit\n",
                       b->name, metric, want, have);
                failed++;
            }
        }
        if (loose) {
            continue;
        }
        double delta = (g->ns - b->ns) / b->ns;
        if (delta > TIMING_TOLERANCE || delta < -TIMING_TOLERANCE) {
            printf("%-22s ns/op: locked %.0f, measured %.0f (%+.1f%%) — %s\n",
                   b->name, b->ns, g->ns, delta * 100.0,
                   delta > 0 ? "a regression"
                             : "an improvement; re-lock in the same commit "
                               "(make bench-lock)");
            failed++;
        }
    }
    for (int j = 0; j < n_got; j++) {
        int known = 0;
        for (int i = 0; i < n_base; i++) {
            if (strcmp(base[i].name, got[j].name) == 0) {
                known = 1;
            }
        }
        if (!known) {
            printf("%-22s NEW — lock it before gating on it\n", got[j].name);
        }
    }
    printf("\n%s\n", failed == 0 ? "bench gate: within tolerance"
                                 : "bench gate: FAILED");
    return failed == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    int lock = 0, gate = 0, loose = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--lock") == 0) {
            lock = 1;
        } else if (strcmp(argv[i], "--gate") == 0) {
            gate = 1;
        } else if (strcmp(argv[i], "--loose") == 0) {
            loose = 1;
        }
    }

    cJSON_Hooks hooks = {counting_malloc, counting_free};
    cJSON_InitHooks(&hooks);

    struct message corpus[] = {
        make_literal("Notification",
                     "{\"jsonrpc\":\"2.0\",\"method\":"
                     "\"notifications/initialized\"}"),
        make_literal("ToolCallRequest",
                     "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
                     "\"params\":{\"name\":\"read_file\",\"arguments\":"
                     "{\"path\":\"/tmp/example.txt\"}}}"),
        make_tool_result("ToolResult4KB", 4 * 1024),
        make_tool_result("ToolResult256KB", 256 * 1024),
    };
    int n = (int)(sizeof(corpus) / sizeof(corpus[0]));

    struct sample results[MAX_BENCHES];
    int n_results = 0;
    for (int i = 0; i < n; i++) {
        results[n_results++] = bench_parse(&corpus[i]);
    }
    /* Attribution for the largest message, which is where the cost is. */
    results[n_results++] = bench_named("Large_JsonParseOnly", &corpus[n - 1],
                                       json_only);
    results[n_results++] = bench_named("Large_RawCopyOnly", &corpus[n - 1],
                                       copy_only);

    render(results, n_results, stdout);

    int rc = 0;
    if (lock) {
        FILE *f = fopen(BASELINE, "w");
        if (f == NULL) {
            fprintf(stderr, "bench: cannot write %s\n", BASELINE);
            return 2;
        }
        render(results, n_results, f);
        fclose(f);
        printf("\nlocked %s — update docs/perf/baseline.md alongside it\n",
               BASELINE);
    } else if (gate) {
        struct sample base[MAX_BENCHES];
        int n_base = parse_baseline(BASELINE, base, MAX_BENCHES);
        rc = compare(base, n_base, results, n_results, loose);
    }

    for (int i = 0; i < n; i++) {
        free(corpus[i].bytes);
    }
    return rc;
}
