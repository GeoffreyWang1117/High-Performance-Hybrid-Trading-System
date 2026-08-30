# Architecture

## The constraint that determines everything

A local 8B language model answers in 10-500 ms. The fast path of a trading
engine is budgeted in nanoseconds. Measured on this machine (AMD Ryzen 9 5950X,
amortized batch timing, 15 repetitions -- see [Measurement](#measurement)):

| Operation | Cost |
|---|---|
| `SPSCQueue::try_push` | 1.21 ns |
| `SPSCQueue::try_pop` | 0.78 ns |
| `ObjectPool::allocate` | 1.41 ns |
| `L2OrderBook::update_level` | 24.03 ns |
| `EventBus::publish` (pre-stamped) | 38.26 ns |
| `operator new` / `delete` | 11.71 ns |
| **Local 1.5B model, one short JSON answer (CPU)** | **~7 600 000 ns** |

That last row is six orders of magnitude above the rest. Any design that puts a
model between a market event and an order is not slow; it is broken. The model
is therefore never in the request path, and the architecture is the boundary
that keeps it out.

## Two lanes

```
   market data ingress (timestamped once, at arrival)
        │
        ▼
┌───────────────────────────────────────────────────────────┐
│  FAST LANE            pinned thread, no alloc, no syscall  │
│                                                            │
│  book update ─▶ signal ─▶ risk ─▶ order                    │
│       │                     ▲                              │
│       │  offer()            │  current()                   │
│       ▼  non-blocking       │  bounded, non-blocking       │
│  ┌─────────────┐      ┌──────────────┐                     │
│  │ LaneBridge  │      │ AdvisorySlot │                     │
│  │ drops when  │      │ versioned    │                     │
│  │ full        │      │ ring         │                     │
│  └─────────────┘      └──────────────┘                     │
└────────│──────────────────────▲───────────────────────────┘
         │ poll()               │ publish()
         ▼                      │
┌───────────────────────────────────────────────────────────┐
│  SLOW LANE            separate threads; may block and       │
│                       allocate and do network I/O           │
│                                                            │
│  context assembly ─▶ LLM inference ─▶ advisory              │
└───────────────────────────────────────────────────────────┘
```

Headers: `include/titans/lanes/`. Contract tests:
`tests/test_lane_isolation.cpp`.

### Fast → slow: `LaneBridge`, and why it drops

`LaneBridge::offer()` is wait-free and **drops when full**, recording the drop.
This inverts the default of most queueing libraries, deliberately. Blocking or
growing is correct for a batch pipeline; here it would let a 200 ms model call
stall the market-data thread, which is the exact failure the separation exists
to prevent.

Drop rate is therefore a first-class metric, not a hidden error path. A slow
lane that is losing 90% of observations is not malfunctioning -- it is sampling,
which is all it can do at its speed -- but an operator has to be able to see
that its advisories rest on 10% of the tape. Measured under a slow lane sleeping
200 ms per item, the bridge sheds 199 743 of 200 000 observations, and the fast
lane does not notice.

### Slow → fast: `AdvisorySlot`, and why staleness is structural

The model publishes an `Advisory`: a small, trivially-copyable value carrying a
stance, a size multiplier, and **two independent expiry conditions**.

```cpp
bool Advisory::is_valid_at(Timestamp now, uint64_t current_generation) const {
    return stance != AdvisoryStance::None &&
           valid_until > now &&                       // time expiry
           context_generation == current_generation;  // context expiry
}
```

Two conditions rather than one, because they catch different failures:

- **`valid_until`** handles the model simply being out of date.
- **`context_generation`** handles the model being *wrong about a world that
  changed*. The fast lane bumps the generation whenever it observes something
  that invalidates prior state -- a book reset, a halt, a reconnect. An advisory
  reasoned from the old generation is rejected **even if it is still inside its
  time window**, because the facts it was derived from no longer hold.

This is the mechanism, not a heuristic to tune: it is two comparisons the fast
path performs on every read. `tests/test_lane_isolation.cpp` asserts both,
including the case a time-only design would miss -- an advisory still within its
validity window whose context generation has moved on.

### Why the slot is a versioned ring, not a seqlock

The first implementation was a textbook seqlock. `test_advisory_slot_never_tears`
caught it handing back torn values: **414 in 2.8M reads** under a tight-loop
writer. The C++ fence formulation is easy to get subtly wrong -- a release fence
stops earlier accesses from sinking below it, but does *not* stop the payload
writes that follow from being hoisted above the "write in progress" marker.

The replacement removes the hazard structurally rather than ordering around it:

- `generation_` counts publishes and only increases.
- A reader reads slot `g0 % 16`.
- The writer, at generation `g`, writes slot `(g + 1) % 16` -- never the slot a
  reader sampled at any generation in `(g + 1 - 16, g]`.
- A read is therefore valid unless the writer **lapped** it, which needs the
  generation to advance by 15 during a 72-byte copy. The reader re-reads the
  generation and detects exactly that case.

Reads are bounded (at most 4 attempts) so the fast path has a hard worst case;
on exhaustion the caller keeps its cached advisory rather than waiting. Result:
0 torn reads in 2.56M under the same adversarial writer, 99.99% read success at
realistic producer rates.

### Degradation is defined, not incidental

Every slow-lane failure mode converges on the same behaviour: the fast lane
falls back to its configured stance and keeps trading.

| Slow lane state | Fast lane behaviour |
|---|---|
| Healthy | Honours the current advisory |
| Slow (200 ms/item) | Bridge drops; advisory ages out; falls back |
| Hung | Same; measured p99 unchanged at 40 ns |
| Dead / never started | Same; measured p99 40 ns, 99.9% bridge drop |
| Publishing mid-read | Bounded retry, then cached value |
| Publishing stale conclusions | Rejected by generation check |

## Measurement

Every latency figure in this repository comes from `include/titans/bench/` and
carries the machine state that produced it.

**The noise floor is enforced.** `TscClock` measures the cost of an empty
`rdtsc` pair at startup -- 20 ns median, 30 ns p99 on this host -- and the
harness refuses to report per-operation percentiles for anything that does not
clear 3x that floor. An amortized run cannot produce a p99, and the harness will
not print one.

This matters because the operations here are 1-2 ns. The previous benchmark
bracketed them with `clock_gettime`, which costs ~40 ns, and reported the result
as operation latency. Its numbers were internally contradictory (an SPSC push at
23 ns, measured with a 44 ns clock) and understated the components by 15-48x.
`titans_benchmark` opens with a self-check that reproduces that method and fails
the run if the harness is miscalibrated.

**Uncontrolled error sources are printed, not hidden.**
`MachineFingerprint::caveats()` reports what was *not* controlled. On this
workstation:

- no `isolcpus`/`nohz_full`, so tail percentiles include unrelated interference
  and are upper bounds;
- SMT enabled, so the sibling hyperthread shares execution resources;
- turbo enabled, so burst and sustained load run at different clocks.

These appear in every results JSON. A p99 from this host is not a p99 from a
tuned trading server, and the file says so.

## Where the research framework fits

The slow lane is where an LLM's context management becomes a *systems* problem
rather than a prompt-engineering one, and that is the connection this repository
explores.

An advisory derived from context that has since been invalidated is precisely a
contaminated inference. `valid_until` and `context_generation` are the blast
radius control. The research framework under `include/titans/context/` studies
what happens without them: how different context-management strategies retain or
shed corrupted material, and what that does to a model's conclusions.

Two rules govern that code, both enforced by tests:

1. **The task must need context.** A task solvable from the single event cannot
   discriminate between context strategies. `tests/test_task_design.cpp` asserts
   the best context-free rule stays near chance (0.5575 balanced accuracy) while
   a per-entity rolling median/MAD rule does well (0.9592). On real Binance
   trades, `titans_dataset` performs the same audit and refuses a dataset that
   fails it.

2. **Assumed numbers are never called findings.** `AssumedDegradationModel`
   computes accuracy from hardcoded multipliers. It is useful for exercising the
   pipeline and for regression detection; it cannot say anything about a real
   model, because its ranking of contamination types is just the ranking of its
   constants. `titans_experiment` states this in its own output. Claims about
   models come from `titans_llm_experiment`, which queries a live backend and
   writes every raw response to the results file.

## Component map

| Path | Role |
|---|---|
| `include/titans/core/` | Lock-free queues, object pools, event bus, event loop, JSON, HTTP |
| `include/titans/trading/` | L2/L3 order book, matching, risk, shadow engine |
| `include/titans/market_data/` | Feed handling, binary logging, replay |
| `include/titans/lanes/` | **Fast/slow boundary**: advisory slot, bridge, budgets |
| `include/titans/bench/` | **Measurement**: TSC timing, noise floor, fingerprint |
| `include/titans/context/` | Research framework: versioned context, contamination, LLM backends, Binance dataset |
| `include/titans/cuda/` | GPU kernels for rolling statistics and alpha factors |
| `python/data/` | Binance archive fetch with checksum verification |
| `python/serving/` | CPU inference shim (OpenAI-compatible) for CI and GPU-less hosts |
| `python/research/` | Figure generation, strictly from measured results |

## Known limitations

Stated here rather than discovered by a reader.

- **No live market connectivity.** `websocket_client.hpp` speaks plain TCP with
  no TLS, and Binance's stream endpoint is `wss://` only. Shadow trading against
  a live feed does not work today. Historical replay does.
- **The main engine feeds itself synthetic ticks.** `src/main.cpp` generates a
  random walk. It demonstrates the event pipeline, not a strategy.
- **Benchmarks run on a non-isolated workstation.** See the caveats above.
- **The GPU kernels are not covered by the measurement rewrite.** Their timings
  are not yet produced by `include/titans/bench/`.
- **LLM experiment scale is bounded by available hardware.** Runs on the CPU
  shim are smoke-scale; the program prints a warning below 200 events per arm
  and refuses to present small deltas as effects.
