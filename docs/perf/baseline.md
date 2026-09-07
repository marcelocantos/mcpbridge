# mcpbridge performance baseline (🎯T23)

Every MCP message an agent sends or receives crosses the wrapper, so
`mcp_msg_parse` runs on the critical path of every tool call — twice per
round trip, once in each direction. `wrapper/bench/bench.c` measures it.

## Running them

```bash
make -C wrapper bench             # run them and print the results
make -C wrapper bench-lock        # make this run the new baseline
make -C wrapper bench-gate        # compare against docs/perf/baseline.txt
make -C wrapper bench-gate-loose  # compare only the counted metrics
```

The gate fails in **both** directions. A regression needs a fix; an
improvement needs `bench-lock` in the same commit as the change that
earned it, because a baseline nobody re-locks stops describing the code
and the next regression only has to stay under the stale number.

## Two kinds of number

**Timings are floors, not averages.** Each benchmark runs many timed
rounds and reports the fastest. Load on the machine can only ever make
a round slower, so the floor is the closest thing to an uncontended
measurement. Floors are still only comparable on the machine that
recorded them, so `bench-gate-loose` skips them.

**Counted metrics are exact and identical everywhere.** `allocs` is the
one that matters: how many times parsing one message calls `malloc`,
counted by handing cJSON its own allocator through `cJSON_InitHooks`.
It is the metric that says whether the parser is building a tree it
immediately throws away.

## Reference machine

| | |
|---|---|
| Machine | Apple M4 Max, macOS 26.6.2, arm64 |
| Compiler | `cc -std=c11 -O2` |
| Recorded | 2026-09-07, under a large agent fan-out (see the floor note above) |

## Locked numbers

| Benchmark | ns/op floor | allocs | message bytes |
|---|---|---|---|
| `Notification` | 178 | 7 | 54 |
| `ToolCallRequest` | 407 | 19 | 116 |
| `ToolResult4KB` | 2,612 | 19 | 4,188 |
| `ToolResult256KB` | 140,143 | 19 | 262,236 |
| `Large_JsonParseOnly` | 136,833 | 19 | 262,236 |
| `Large_RawCopyOnly` | 3,195 | 0 | 262,236 |

## What the attribution says

The last two rows are not benchmarks of the product; they are a
decomposition of the row above them, the same way a spawn control
decomposes a process benchmark. They split what `mcp_msg_parse` does to
a large message into the cJSON parse and the retained copy of the raw
bytes:

| | |
|---|---|
| Classifying a 256 KB tool result | 140 µs |
| …of which the cJSON parse is | 137 µs (98%) |
| …and the raw copy the wrapper actually needs is | 3 µs |

The wrapper reads three things out of a message — the `id`, the
`method`, and whether `result` or `error` is present — and then
forwards the bytes verbatim. All three live in the top-level object.
Everything below it is parsed into a tree, allocated, unescaped, and
deleted without being looked at.

So the floor for this work is on the order of the copy, and the current
cost is forty times that on a large message. 🎯T24 carries the fix and
the three traps found while scoping it. It was deliberately not
attempted in the burst that measured it: a hand-rolled JSON validator
on the path that carries all agent traffic is not a change to rush, and
the measurement is worth having on its own.

The allocation count tells the same story from the other side. It is
flat at 19 across a 116-byte request and a 256 KB result — cJSON
allocates one buffer for the giant string rather than one per byte — so
allocations are *not* what makes a large message expensive. The scan
and the unescape are. A metric that stayed flat while the timing grew
800-fold is a useful reminder that the counted metric and the timing
answer different questions.
