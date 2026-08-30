# Titans

An event-driven C++20 trading engine with a **hard boundary between a
nanosecond-scale fast path and an LLM-scale slow lane**, and a measurement
harness that refuses to report numbers it cannot resolve.

The second half of that sentence is the point. A trading system that publishes
latency figures without stating how they were measured is not making a claim
that can be checked. Everything below is reproducible on the commands given, and
every figure carries the machine state that produced it.

---

## Why a boundary, and where it sits

A local language model answers in 10–500 ms. This engine's fast path costs
single-digit nanoseconds per operation. Six orders of magnitude separate them, so
the model is never in the request path. It publishes **advisories** — small,
immutable, explicitly expiring facts — into a slot the fast lane samples with a
bounded, non-blocking read.

```
market data ─▶ book ─▶ signal ─▶ risk ─▶ order      FAST LANE   (ns, pinned, no alloc)
                 │                  ▲
        offer()  │                  │  current()
     drops when  ▼                  │  bounded, never waits
          full  ┌──────────┐  ┌──────────────┐
                │LaneBridge│  │ AdvisorySlot │
                └────┬─────┘  └──────▲───────┘
                     │               │
                     ▼               │ publish()
        context ─▶ LLM ─▶ advisory                   SLOW LANE   (ms, may block)
```

The contract is that **nothing the slow lane does can degrade the fast path**.
That is asserted, not asserted-and-hoped:

```
$ ./build/tests/titans_tests
=== Testing Lane Isolation ===
  [ RUN ] hung slow lane does not slow fast lane
    fast-lane p99: slow lane draining 610.0 ns, slow lane hung 40.0 ns (0.07x)
    bridge drop rate while hung: 99.9% (199743 of 200000 dropped)
  [ OK   ] hung slow lane does not slow fast lane
  [ RUN ] advisory slot never tears
    adversarial writer: 2564325 reads, 0 torn, 650 lapped give-ups
    realistic writer:   7008703 reads, 0 torn, 616 give-ups (99.9912% success)
  [ OK   ] advisory slot never tears
```

With the slow lane sleeping 200 ms per item — the latency of a real model — the
fast lane's p99 is unchanged and the bridge sheds 99.9% of observations. Load
shedding is the designed behaviour on that edge, and the drop rate is a reported
metric rather than a swallowed error.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the mechanism, including
why the advisory slot is a versioned ring rather than a seqlock (the seqlock
version tore 414 times in 2.8M reads, and the test caught it).

---

## Measured performance

AMD Ryzen 9 5950X, 16 cores, `performance` governor, GCC 15.2, `-O3
-march=native -flto`. Amortized batch timing, 15 repetitions, median across
repetitions; minimum in parentheses is the interference-free estimate.

| Operation | Cost | Throughput |
|---|---|---|
| `SPSCQueue::try_push` | 1.21 ns (0.99) | 826 M ops/s |
| `SPSCQueue::try_pop` | 0.78 ns (0.77) | 1279 M ops/s |
| `ObjectPool::allocate` | 1.41 ns (1.40) | 708 M ops/s |
| `ObjectPool::deallocate` | 1.08 ns (1.06) | 926 M ops/s |
| `operator new`/`delete` *(baseline)* | 11.71 ns (11.26) | 85 M ops/s |
| `L2OrderBook::update_level` | 24.03 ns (23.70) | 42 M ops/s |
| `L2OrderBook::best_bid` | 5.45 ns (5.40) | 184 M ops/s |
| `EventBus::publish` (pre-stamped) | 38.26 ns (37.20) | 26 M ops/s |
| `EventBus::publish` (auto-timestamp) | 59.83 ns (57.18) | 17 M ops/s |

Reproduce with `./build/titans_benchmark --json results/mine.json`.

**No p99 appears in that table, deliberately.** These operations cost less than
the 20 ns timing floor measured at startup, so a per-operation distribution is
not observable and the harness will not print one. What it prints instead is the
spread between the median and the minimum across repetitions, which tells you
how busy the host was.

The last two rows are a finding rather than a datum: `EventBus::publish` spends
21.6 ns — 36% of its cost — on an unconditional `clock_gettime`. Callers that
already hold an ingress timestamp should use `publish_prestamped()`, which is
also more correct, since dispatch time is a worse estimate of arrival than the
stamp taken when the packet landed.

### The benchmark checks itself first

```
$ ./build/titans_benchmark
 SECTION 0 - MEASUREMENT METHODOLOGY SELF-CHECK

TSC 3.3999 GHz | single-shot floor: 20.00 ns (p99 30.00 ns) | resolvable above 60.00 ns

Naive clock-bracketed timing, as used before this rewrite:
  cost of now_ns() itself, measured with now_ns():      40.00 ns (p50)
  cost of an integer increment, same method:            20.00 ns (p50)
Same integer increment, amortized batch timing:          0.239 ns/op

VERDICT
  [confirmed] The naive method reports an integer increment at 20 ns, within a
  factor of two of the 40 ns clock read used to measure it. An increment does
  not cost 20 ns. The naive method measures the clock, not the operation.
  [confirmed] Amortized timing resolves the same increment at 0.239 ns/op,
  consistent with a single retired ALU op on a 3.40 GHz core.
```

The run exits non-zero if that check fails.

### What was not controlled

Every results file carries a `caveats` list. On this workstation:

- **No `isolcpus`/`nohz_full`.** The kernel schedules other work on the measured
  core, so tail percentiles include unrelated interference and are upper bounds.
- **SMT enabled.** Benchmarks pin to one logical CPU but do not idle its sibling.
- **Turbo enabled.** Burst and sustained load run at different clocks.

A p99 from this host is not a p99 from a tuned trading server.

---

## The research framework

The slow lane is where an LLM's *context management* stops being prompt
engineering and becomes a systems problem: an advisory derived from context that
has since been invalidated is a contaminated inference, and `valid_until` +
`context_generation` are how its blast radius is bounded.

`include/titans/context/` studies what happens without those controls — how
different context strategies retain or shed corrupted material, and what that
does to a model's conclusions.

Two rules govern this code, both enforced by tests rather than convention.

**1. The task must actually require context.** A task solvable from a single
event cannot distinguish one context strategy from another; every method scores
the same and any difference is noise.

```
$ ./build/tests/titans_tests
=== Testing Task Design ===
    best context-free global rule:      0.5575 balanced accuracy (chance = 0.5)
    per-entity rolling median/MAD rule: 0.9592 balanced accuracy
```

The same audit runs on real market data (below), and `titans_dataset` exits
non-zero on a dataset that fails it.

**2. Assumed numbers are never presented as findings.**
`AssumedDegradationModel` computes accuracy from hardcoded multipliers — 0.5 for
an entity-binding error, 0.7 for stale state. It is genuinely useful for
exercising the pipeline and catching regressions, and it is genuinely incapable
of saying anything about a real model, because its ranking of contamination
types is just the ranking of its constants. `titans_experiment` says so in its
own output. Claims about models come from `titans_llm_experiment`, which queries
a live backend and writes every raw response to the results file.

The ablation runner also flags configurations that changed nothing:

```
$ ./build/titans_experiment
Configuration        Accuracy   d Clean   Stale Ref       FPR   status
full                   87.52%    19.81%      93.38%    11.87%
no_temporal            87.52%    19.81%      93.38%    11.87%   INERT
no_provenance          86.05%    23.07%      99.01%    13.24%
no_forgetting          51.78%    11.32%      99.46%    45.73%
...
1 of 7 ablations were INERT -- identical to the full system, meaning the
disabled component never executed:
  - no_temporal   selective forgetting already caps context age at the
                  per-entity revisit interval (~50 ms for 50 entities at
                  1.0 ms spacing), inside the 2.0 s temporal window, so the
                  window is never the binding constraint.
```

An identical row is not a result saying the component does not matter; it is the
experiment saying it cannot tell. It is labelled that way, with the reason.

---

## Real market data

`python/data/fetch_binance.py` pulls aggregated trades from
[data.binance.vision](https://data.binance.vision/), verifies the published
SHA256, and writes a manifest recording source URL, checksum, and row count.

Real data has no anomaly column, so the label is constructed **from the future**,
where no field of the event can reach: a trade is *toxic* if the price moves at
least `threshold_bps` in the aggressor's favour within `horizon_ms`. That is
ordinary adverse selection, and it is not recoverable from the trade itself.

```bash
python python/data/fetch_binance.py --symbol BTCUSDT --date 2024-01-15
./build/titans_dataset data/raw/BTCUSDT-aggTrades-2024-01-15.csv
```

```
  trades:        1364603
  labelled:      1364527
  toxic:         21769 (1.60% of labelled)
  session drift: -0.154 bps per horizon (removed from the label)

LEAKAGE AUDIT (AUC; 0.5 = chance)
  event-only features -- these must stay near chance:
    trade quantity          0.5383   (|dev| 0.0383)
    trade price             0.5735   (|dev| 0.0735)
    aggressor side          0.5012   (|dev| 0.0012)
  context feature -- this must beat chance:
    signed flow, last 50    0.7072   (|dev| 0.2072)

  [pass] No single event field clears the leakage limit
  [pass] Trailing signed order flow predicts the label above chance
```

The audit earned its place immediately. Before drift adjustment the aggressor
side alone scored AUC 0.604 against the label, because BTCUSDT trended that day
and aggressive buys were followed by favourable moves for reasons that had
nothing to do with informed flow. Subtracting the session's mean forward return
brings it to 0.5012 — chance.

It also has to run on the **whole** session. The correction is a sample-mean
statistic, so on a truncated prefix it removes the prefix's mean and leaves the
local trend standing. Same file, same settings, different amounts of it:

| Trades audited | Aggressor-side AUC | Verdict |
|---|---|---|
| First 20 000 | 0.6979 | rejected — leakage |
| First 200 000 | 0.5897 | passes, flagged `[near]` |
| All 1 364 603 | **0.5012** | clean pass |

`--max-rows` therefore prints a warning saying exactly this.

---

## Build and run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

| Target | What it does |
|---|---|
| `titans_benchmark` | Fast-path latency, with a methodology self-check |
| `titans_dataset` | Label real trades and audit for leakage |
| `titans_experiment` | Context-strategy comparison, assumed-degradation stand-in |
| `titans_llm_experiment` | Same comparison against a live model |
| `titans_replay` | Replay a binary market-data log |
| `titans_engine` | Event pipeline demo (synthetic ticks; see limitations) |
| `titans_tests` | 9 modules including lane isolation and task design |

### Running against a live model

```bash
# GPU
vllm serve Qwen/Qwen2.5-7B-Instruct --port 8000
./build/titans_llm_experiment --backend vllm --port 8000 --events 1000

# No spare GPU (also what CI uses)
python python/serving/cpu_shim.py --model Qwen/Qwen2.5-1.5B-Instruct --port 8011
./build/titans_llm_experiment --backend vllm --port 8011 --events 1000
```

`titans_llm_experiment` **will not run without a reachable backend**. There is
no stand-in fallback: a number produced without querying a model is not evidence
about a model.

Both arms of each comparison consume identical events, identical seeds, and
identical contamination draws; the only difference is whether the contamination
reached the prompt. Contamination attempts that could not be applied — too
little history to draw a stale value from, for instance — are excluded rather
than counted as treated, so the treatment group is not silently diluted.

---

## Requirements

- GCC 13+ or Clang 16+ (C++20)
- CMake 3.20+
- x86-64 with invariant TSC (`constant_tsc`, `nonstop_tsc`) for the benchmarks
- CUDA 11+ *(optional)*
- Python 3.10+ for the data and serving tools

```bash
conda create -n titans -c conda-forge python=3.11 numpy pandas scipy \
    matplotlib seaborn plotly requests pyarrow pytest gtest cmake ninja
```

---

## Known limitations

- **No live market connectivity.** `websocket_client.hpp` speaks plain TCP with
  no TLS; Binance's stream endpoint is `wss://` only. Historical replay works.
- **`titans_engine` feeds itself a random walk.** It demonstrates the event
  pipeline, not a strategy.
- **Benchmarks run on a non-isolated workstation.** See the caveats above.
- **CUDA kernels are not yet covered by the measurement rewrite.**
- **LLM experiment scale depends on available hardware.** The program warns
  below 200 events per arm and declines to present small deltas as effects.

## License

MIT — see [LICENSE](LICENSE).

## Disclaimer

Research and educational software. Nothing here is trading advice, and no part
of it has been run against live capital.
