# Titans

An event-driven C++20 trading engine with a **hard boundary between a
nanosecond-scale fast path and an LLM-scale slow lane**, and a measurement
harness that refuses to report numbers it cannot resolve.

The second half of that sentence is the point. A trading system that publishes
latency figures without stating how they were measured is not making a claim
that can be checked. Everything below is reproducible on the commands given, and
every figure carries the machine state that produced it.

Three of the results in this README are refusals — the tool declining to answer
because the answer would not have been supportable. They are kept deliberately.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
scripts/reproduce.sh          # re-derives every number below, exits with the failure count
```

| Document | What it covers |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | The fast/slow boundary and why each mechanism is shaped the way it is |
| [docs/RELATED_WORK.md](docs/RELATED_WORK.md) | What here is genuinely unusual, and what is a re-implementation |
| [docs/ROADMAP.md](docs/ROADMAP.md) | What is next, in priority order, and what industry practice each item comes from |
| [docs/FRESHNESS.md](docs/FRESHNESS.md) | Advisory age as a contract: why expiry was the wrong knob |
| [docs/LATENCY.md](docs/LATENCY.md) | Tick to trade, per stage, and where the coordinated-omission correction fails |
| [docs/REGRESSION.md](docs/REGRESSION.md) | Catching a slowdown across commits, and the bimodal hardware underneath |
| [docs/SELECTION.md](docs/SELECTION.md) | Fold-boundary adjacency and the search behind a reported number |
| [docs/CONTEXT_COST.md](docs/CONTEXT_COST.md) | What a richer prompt costs in staleness, on real model latencies |
| [docs/SUBSET.md](docs/SUBSET.md) | Subset selection beats recency, and the QUBO is not the reason |
| [docs/RESEARCH_FRAMEWORK.md](docs/RESEARCH_FRAMEWORK.md) | The context-contamination study and its rules |
| [docs/API_REFERENCE.md](docs/API_REFERENCE.md) | Types and headers |
| [docs/DEBUGGING_GUIDE.md](docs/DEBUGGING_GUIDE.md) | Logging, assertions, profiling, memory tracking |
| [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | Build, runtime, GPU, and backend failures |

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

These were taken on an otherwise idle host. Re-running under load moves them —
the same suite during a concurrent GPU inference job returns 1.72 ns for
`try_push` rather than 1.21 — which is the point of reporting the median/minimum
spread and the `caveats` block rather than a single figure. What should
reproduce anywhere is the ordering and the order of magnitude, not the third
significant digit.

**The second digit does not reproduce either, and that is now measured.** A
40-run series on this host shows six of these nine metrics are *bimodal across
processes*: `try_push` takes one of two values, 1.02 ns or 1.48 ns, decided at
process start and stable for that whole run. Every row above is one draw from
such a distribution. See [docs/REGRESSION.md](docs/REGRESSION.md).

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

That table is one run, and it is a file:
[`results/benchmark_GW-X570-Taichi_20260830_readme.json`](results/benchmark_GW-X570-Taichi_20260830_readme.json).
Every row above, throughput column included, can be read out of it, along with
the machine state and the `caveats` block that produced it. Reproduce your own
with `./build/titans_benchmark --json results/mine.json`.

That file exists under that name because the obvious name was not safe. It was
originally committed as `results/benchmark_GW-X570-Taichi_20260830.json` and
was overwritten later the same day by `scripts/reproduce.sh`, which wrote its
own run to `benchmark_$(hostname)_$(date +%Y%m%d).json` — the same path. From
that day until this one the table cited a file that no longer contained it, and
nothing noticed, because nothing in this repository checks its prose against its
own artifacts. The script now writes to `/tmp`, and the artifact is kept under a
name no run of it can generate.

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

### Tick to trade

Those are per-operation costs, and a system is not a sum of them. `titans_ticktotrade`
runs the real path over the real tape — ingest, book, signal, risk, order — with
the strategy logic inside the measured region, and stamps each boundary once
rather than bracketing each stage twice. 400,000 trades, 658.9 minutes of tape.

| stage | mean | p99 | p99.9 | share |
|---|---|---|---|---|
| ingest | under floor | 110.3 ns | 210.3 ns | 6.6% |
| `L2OrderBook` update + top of book | 170.0 ns | 780.9 ns | 1609.2 ns | 40.4% |
| advisory read + policy | 104.6 ns | 210.3 ns | 580.9 ns | 24.9% |
| `RiskManager::check_order` | 90.8 ns | 130.3 ns | 290.3 ns | 21.6% |
| order out to the wire | under floor | under floor | under floor | 6.5% |
| **end to end** | **420.4 ns** | **1100.9 ns** | **2305.6 ns** | |

Two of the five stages cost less than three times the clock read used to measure
them, so the table refuses them rather than printing a number. The stage means
sum to the end-to-end mean exactly, which is the property a boundary chain has
and five nested scopes do not; the share column uses means because means add and
percentiles do not. The instrument costs a measured 23.3 ns per boundary, 139.7 ns
per tick, and that figure is printed in the header of every run rather than
assumed away.

**The 1101 ns is what a closed-loop benchmark would report, and quoted alone it
is misleading.** It is the handler's own cost and nothing else. Measured from
the moment each tick was *due*, the same run gives a p99 of **78.9 µs** — 72×
larger — because 43% of these trades share a millisecond with the one before
them and the queue builds from arrivals bunching. A host-jitter control, the
same schedule with no pipeline attached, has a p99 of 3.6 µs, so the tail clears
the machine by 22× and is real.

Gil Tene's coordinated-omission correction is the standard repair for this, and
on this workload **it recovers under 2% of the gap while synthesising 8,741
samples**. It infers omission from a service time longer than the expected
interval; here no single call is slow, so it has nothing to find. Accelerating
the tape 512× moves the two numbers in opposite directions: service p99 *falls*
monotonically from 1844 ns to 621 ns while the real tail climbs to 2.17 ms. Full
account, including the saturation sweep, the run-to-run spread, and what none of
it settles, in [docs/LATENCY.md](docs/LATENCY.md).

```
./build/titans_ticktotrade --data data/raw/BTCUSDT-aggTrades-2024-01-15.csv \
    --rows 400000 --speed 2000 --core 2 --slow-core 4 --sweep
```

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

### Is it slower than last week?

Nothing in this repository used to answer that. CI checked the harness
calibrates and never compared two runs, so a 20% regression in `update_level`
would have passed silently. `titans_regression` runs E-Divisive change-point
detection (Matteson & James 2014, as deployed at MongoDB) over a series of runs
and fails only when the most recent level shift clears two bars: significant
under a permutation test, **and** larger than 3× the dispersion the series
itself shows.

The second bar is the one that matters. Injecting a step of known size into the
real 40-run baseline:

| injected | p | effect | gate |
|---|---|---|---|
| 2% | 0.0050 | 1.3 sd | clean |
| 5% | 0.0010 | 2.8 sd | clean |
| **6%** | **0.0010** | **3.2 sd** | **REGRESSION** |
| 20% | 0.0010 | 9.8 sd | REGRESSION |

A 2% injection is already *significant*. A gate built on significance alone
fires there, three times more sensitive than the noise justifies, and gets
switched off by its owners inside a week.

Two things came out of building it that the plan did not anticipate. The
harness's own `rep_spread_pct` is the wrong noise model — two committed runs of
the same binary seven days apart differ by 45% on `operator new/delete` while
each claims internal consistency of 1.4% — and the hardware is bimodal, which
breaks the effect-size bar on six of nine metrics. Both are reported by the tool
rather than assumed away.

```
./build/titans_regression results/benchmark_history/local \
    --reference results/benchmark_GW-X570-Taichi_20260906.json
```

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

### What the live-model runs actually produced

Two models were run end to end against this pipeline. Both results are in
`results/llm/`, raw responses included. Neither supports a conclusion about
contamination, and the program says so rather than letting the table speak:

| Model | Events/arm | Result |
|---|---|---|
| Qwen2.5-1.5B-Instruct (CPU) | 50 | answered `anomaly` to **every** event in all 6 arms |
| Qwen2.5-3B-Instruct (GPU) | 400 | 5 of 6 arms ≥98% one class |

The 1.5B table is the instructive one, because it looks like a clean null:

```
NoHistory          0.5000   0.5000   +0.0000
FixedWindow        0.5000   0.5000   +0.0000
VersionedContext   0.5000   0.5000   +0.0000
```

Exactly 0.5000 six times is a constant classifier, not a finding. A model that
answers the same thing regardless of input scores exactly 0.5 balanced accuracy
no matter what it is shown, so every arm ties and every delta is zero by
construction. Reading that as "contamination had no effect" would be completely
wrong. `titans_llm_experiment` now detects this, prints a block naming each
affected arm before any delta is interpretable, and exits 3.

**The diagnosis is more interesting than the failure.** Reading the raw
responses, the 3B model was applying the rule — `"Value diff > 15 from recent
trade"` — but comparing against records belonging to *other entities*. Entity
levels are spread across [50, 500], so a foreign reference makes almost any
value look extreme. That is the entity-binding failure mode this project
studies, occurring spontaneously on **clean** context.

Three prompt revisions followed: state the numeric tolerance, require filtering
context to matching `entity`, and add a worked example. They changed the
model's reasoning text on 45 of 60 trials and **not one classification**. At
that point the honest move was to stop: a fourth revision tuned until the number
came out right is the failure mode this repository exists to prevent.

So the standing conclusion is that this task needs a more capable model than the
hardware here could host, the infrastructure to run it is verified correct
(context sizes track the strategy, contamination reaches the prompt, latency
scales with context, every raw response is recorded), and the guard prevents a
degenerate run from being mistaken for a null result.

### What a richer prompt costs in staleness

The research framework studies context strategies and the lane reports advisory
age. Those meet in an awkward place: prefill cost scales with context length, so
a richer prompt directly buys delay. `titans_context_cost` prices the trade.

First, what a delay costs on its own. The flow heuristic needs no context, so
scoring it at a range of delivery delays isolates the price of arriving late:

| delay | 0 ms | 100 ms | **250 ms** | 1000 ms | 4000 ms |
|---|---|---|---|---|---|
| informedness | +0.0759 | +0.0409 | **+0.0195** | +0.0086 | +0.0009 |

**Half the value is gone by 250 ms**, against a 1000 ms horizon.

Then what context costs. llama3.1:8b on an RTX 3090, 400 decision points, the
same points in every arm, compared by resampling them jointly:

| context | total latency | J at context, vs 8 trades | J delivered, vs 8 trades |
|---|---|---|---|
| 8 trades | 413 ms | — | — |
| 128 | 708 ms | +0.0550 [−0.0732, +0.1791] | **−0.1864 [−0.3419, −0.0302]** |
| 512 | 1608 ms | −0.0900 [−0.2198, +0.0350] | **−0.2970 [−0.5044, −0.0964]** |
| 1024 | 2887 ms | +0.0450 [−0.0918, +0.1721] | −0.1802 [−0.4316, +0.0684] |

**More context did not make the model better and did make the answer later.**
Across 128× more context no at-context difference is resolved in any of four
runs; delivered, the 512-trade arm is resolved negative in all four.

There is no interior optimum here to find. At the smallest context the model
spends 30 ms on prefill and 383 ms on everything else, and that floor alone
already exceeds the 250 ms half-life before context has bought anything. Full
account, including all four runs and the two guards that were both wrong on the
first attempt, in [docs/CONTEXT_COST.md](docs/CONTEXT_COST.md).

### A QUBO whose sophisticated half does nothing

The last roadmap item was a combinatorial subset selector taken unchanged from a
workshop formulation: pick *K* of the last *n* trades to maximise relevance minus
λ·redundancy, with greedy, simulated annealing and exhaustive search as three
points on one frontier. It was scheduled last on the expectation that it would
produce a null.

It was run across all 28 days, a day as one observation, scored against forward
toxic-flow labels the objective never sees.

| method | mean diff vs recency | 95% CI over days | days better |
|---|---|---|---|
| random-16 | −0.0009 | [−0.0230, +0.0213] | 12 / 28 |
| **greedy λ=0** | **+0.0576** | **[+0.0323, +0.0827]** | **21 / 28** |
| greedy λ=2 | +0.0414 | [+0.0162, +0.0661] | 20 / 28 |
| qubo/SA λ=0 | +0.0553 | [+0.0302, +0.0812] | 21 / 28 |
| qubo/SA λ=2 | +0.0425 | [+0.0167, +0.0674] | 20 / 28 |

**Selection wins.** Every interval excludes zero, the sign test over days gives
p = 0.00045, and picking the best of eleven methods is deflated for (bar
+0.0209, observed +0.0576, z +2.85, survives). Random selection is flat, which
is the control: keeping a subset is not what helps, keeping the *large* trades
is.

**The QUBO is not why.** λ = 0 switches the redundancy term off entirely, leaving
a sort by absolute size — and that is the best method in the table. Raising λ
makes it monotonically worse. The redundancy term, which is the source
formulation's headline, costs 28% of the effect; greedy at λ = 0 takes 2.0 µs
where the annealer takes 119 µs to reach the same answer.

This document nearly said the opposite. The first version was written from one
day, which happened to be one of the two in eight where recency wins. A
`reproduce.sh` assertion running the same tool on another day is what caught it.
Full account in [docs/SUBSET.md](docs/SUBSET.md).

### Ablations that changed nothing are labelled

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

### Reproducing every number above

```bash
scripts/reproduce.sh              # everything needing no network or GPU
scripts/reproduce.sh --with-data  # + download real trades, run the audit
scripts/reproduce.sh --with-llm   # + the live-model experiment
```

It rebuilds, runs each claim's source, prints what it got, and exits with the
number of stages that failed. It also asserts the three refusals — no backend,
no results, unreadable config — because a regression that restores a silent
fallback would otherwise look like success.

| Target | What it does |
|---|---|
| `titans_benchmark` | Fast-path latency, with a methodology self-check |
| `titans_dataset` | Label real trades and audit for leakage |
| `titans_lanes` | Replay real trades through both lanes, end to end |
| `titans_walkforward` | Out-of-sample policy evaluation across many days |
| `titans_ticktotrade` | Per-stage and end-to-end latency over the real path |
| `titans_regression` | Change-point gate over a series of benchmark runs |
| `titans_context_cost` | Freshness decay, and what context length costs in age |
| `titans_subset` | QUBO subset selection, scored against external labels |
| `titans_experiment` | Context-strategy comparison, assumed-degradation stand-in |
| `titans_llm_experiment` | Same comparison against a live model |
| `titans_replay` | Replay a binary market-data log |
| `titans_engine` | Event pipeline demo (synthetic ticks; see limitations) |
| `titans_tests` | 20 modules including lane isolation, task design, the walk-forward protocol, the freshness contract, the latency instrument, the regression gate, the multiple-testing correction, delivery-time scoring, and the subset objective |

`titans_engine --config config/engine.json` reads risk limits, symbols, and
strategy parameters from the file; command-line flags override it, and an
unreadable or malformed file is fatal rather than a silent fall back to
defaults. The previous `config/engine.yaml` and `config/strategy.yaml` were
never read by any code — the project has no YAML parser — so they documented
behaviour that did not exist and have been replaced.

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

## End-to-end: both lanes over real trades

`titans_lanes` is the only place the whole shape runs at once. Real trades drive
a pinned fast lane under a declared budget; a slow lane on another core
publishes advisories; the outcome is scored against the forward toxic-flow
labels — adverse selection avoided, versus benign flow needlessly declined.

```bash
./build/titans_lanes data/raw/BTCUSDT-aggTrades-2024-01-15.csv \
    --max-rows 100000 --speed 500 --ttl-ms 1000 --repeat 5
```

Replay is time-faithful: the fast lane paces to the trades' own timestamps
divided by `--speed`, and the slow lane's simulated latency is divided by the
same factor, so the ratio that matters — model latency against inter-trade
interval — is preserved. Replaying as fast as the CPU allows is not a faster
version of this experiment but a different one, and the first version of this
program proved it: the fast lane finished 5.2 hours of tape in tens of
milliseconds and 97% of advisories were rejected as expired.

**The tool declines to score the policy:**

```
Informedness (TPR - FPR) across 5 runs
  median +0.0171, range [-0.0084, +0.0592], sd 0.0251
VERDICT: NOT RESOLVED
```

The median sits inside two standard deviations of the run-to-run spread, so this
configuration does not measure the policy.

For a long time the reading was "the toy order-flow policy has no value". That
was wrong, and the walk-forward below is what showed it: the same rule, on the
same trades, with the same threshold, scores **+0.16** when it is evaluated
without the lane machinery. The policy works. What did not work was the lane.

Four hypotheses were tested. The first three are dead ends, kept because a
rejected explanation is worth as much as the one that survives:

| Hypothesis | Test | Result |
|---|---|---|
| Advisories expire before use | `--ttl-ms` 250 → 60 000 | rejection 34.4% → 10.9%, informedness flat (−0.012, +0.017, −0.004, +0.013) |
| The bridge sheds observations | read `dropped` | 0 of 99 972 |
| The advice is too old, so gate it | `--max-age-frac` 1.0 → 0.05 | acted-on p99 age tracks the gate exactly (897 → 50 ms). Informedness stays unresolved at **every** setting |
| The advice is *misaligned*, not stale | compare the delivered decision against the ideal one, trade by trade | **the lane acts at nearly the right rate and on the wrong trades** |

The fourth is the answer, and it took an instrument to see:

```
DELIVERED vs IDEAL decision, trade by trade
  ideal sizes down 3320, lane sizes down 3211
  both size down          1332  (40.1% of ideal's actions survived delivery)
  ideal only, lane no     1988  the lane missed these
  lane only, ideal no     1879  the lane acted where it should not
```

Nearly the right number of actions, barely 40% on the right trades. A
threshold *crossing* is only correct at the instant it is taken; the median
advisory here is three trades old, and three trades is enough to move the
action onto its neighbours. No expiry rule and no freshness gate can realign
it, which is exactly why neither helped.

### Ship the parameter, not the decision

So the slow lane stopped shipping its answer and started shipping the
calibrated threshold behind it, letting the fast lane evaluate the same rule
against a window that is current by construction. The threshold is a
5000-sample quantile: it moves slowly, so it survives the trip. The decision
moves every trade, so it does not.

Same rule, same trades, same threshold to four significant figures, same lane,
same advisory ages. Only the payload differs:

| `--advisory` | agreement with ideal | informedness | run-to-run sd | verdict |
|---|---|---|---|---|
| `decision` | 40.1% | median −0.0102 | 0.0255 | NOT RESOLVED |
| `parameter` | **89.8%** | median **+0.2426** | **0.0028** | **resolved** |

The standard deviation is the part worth staring at. Shipping a decision makes
the outcome depend on when the slow lane happened to wake — a 9× run-to-run
spread. Shipping a parameter makes it very nearly deterministic.

This also fixes what the freshness contract was measuring. An advisory declares
the horizon of *what it carries*, and the two payloads do not have the same
one: a decision inherits the signal's 1000 ms horizon, while a threshold's
horizon is the stretch of market it was estimated over — 789 s here, measured
rather than configured. Under identical delivery, at an identical p99 age of
~1530 ms:

```
--advisory decision    declared horizon 1000 ms      SLO BREACHED   exit 5
--advisory parameter   declared horizon 789282 ms    SLO MET        exit 0
```

Both assertions run in `scripts/reproduce.sh`. A build where shipping a
decision suddenly passes means the contract stopped being enforced.

The general form is the same split online feature stores make when they give
each feature its own staleness budget instead of one global freshness target:
**put the slow-moving quantity on the slow lane, and evaluate the fast-moving
one where the state is fresh.**

What is also measured: the fast-lane stage costs 222 ns mean against a 500 ns
budget even with the rule evaluated locally; the bridge drops nothing at this
rate; and pacing lag runs four orders of magnitude below the advisory lifetime.

---

---

## Out-of-sample: 28 days, and a threshold that never saw its test day

Everything above came from one day, and the policy's threshold was calibrated on
the first 5000 observations of the same day it was then scored on. Both are
defensible in isolation and together they cannot answer the only question that
matters for a decision rule: does it work on a day it has never seen.

```bash
python python/data/fetch_binance.py --symbol BTCUSDT --dates 2024-01-08:2024-02-04
./build/titans_walkforward data/raw/BTCUSDT-aggTrades-2024-0*.csv --json results/wf.json
```

For each day *t*, fit on days `[0, t)` and test on day *t*. Day *t* contributes
nothing to its own threshold, and the run is refused outright if any fold's
training data reaches past its test day's first trade — checked on timestamps,
because argument order is a claim and a timestamp is evidence.

```
INFORMEDNESS (TPR - FPR) ACROSS 27 FOLDS
  out-of-sample              +0.1676  95% CI [+0.1385, +0.1966]
  in-sample (counterfactual) +0.1725  95% CI [+0.1402, +0.2067]
  in-sample minus OOS        +0.0049  95% CI [-0.0032, +0.0144]
  sign-flip test vs zero     p < 5e-05

VERDICT
  RESOLVED. The out-of-sample 95% interval [+0.1385, +0.1966]
  excludes zero across 27 days the threshold never saw.
```

Two things in that block are worth more than the headline.

**The in-sample arm is reported next to it, on purpose.** Fitting the threshold
on the test day itself buys only +0.0049, and the interval on that gap contains
zero. So the single-day number was not inflated by in-sample calibration — a
result that could easily have gone the other way, and one that only exists
because both arms were run.

**This was an upper bound, and the lane has since reached it.** The number here
removes the lane machinery: no threads, no advisory expiry, decision taken from
the window ending at the previous trade. When it was first measured,
`titans_lanes` scored ~0 on the same policy and the gap was unexplained. It is
now attributed and mostly closed — the lane recovers 89.8% of the reference's
actions once the slow lane ships the threshold instead of the decision. See the
section above.

### Two null results from this run

Both are recorded because a knob that did not help is worth exactly as much as
one that did, and only one of them normally gets written down.

**Three of 28 days fail the label audit** — the aggressor side reaches AUC
deviation 0.157 on 2024-01-23, past the 0.10 limit. The hypothesis was intraday
trend: a session-mean drift correction removes the day's average and leaves the
local trend standing. A rolling estimate should fix it. It does not, at any
window tested:

| Drift estimate | Days failing the audit | Out-of-sample informedness |
|---|---|---|
| 1 min rolling | 6 | +0.1515 |
| 5 min rolling | 5 | +0.1612 |
| 30 min rolling | 3 | +0.1672 |
| 2 h rolling | 3 | +0.1679 |
| **session mean (default)** | **3** | **+0.1676** |

Short windows are worse (estimation noise), long ones converge back to the
session mean. The mechanism is implemented and unit-tested — on a fixture that
rises then falls, it takes the toxic set from 60% one-sided to balanced — so the
estimator works and the *hypothesis* is wrong. Something other than intraday
trend makes those three days leak, and it is not yet known what. The default
stays at the session mean and the three days stand as a known defect.

**The effect does not depend on those days.** Dropping them leaves 24 folds at
+0.1528, 95% CI [+0.1253, +0.1797]. That is a post-hoc subset and is reported
below the headline as a sensitivity bound, never in place of it.

### The fold boundary, and how many things were tried

Two ways this number could have been optimistic. Both are now measured rather
than argued.

**The fold boundary.** Training ends when day *t−1* ends and testing starts when
day *t* begins; the label looks 1000 ms ahead and the market does not reset at
midnight. `--embargo-ms` holds training samples within a given distance of a
day's end back from the threshold applied to the next day, cutting on time
rather than on a count of trades because trade density varies by two orders of
magnitude within a session.

| embargo | out-of-sample informedness |
|---|---|
| none *(headline)* | +0.1676 |
| 1 s | +0.1676 |
| 1 min | +0.1676 |
| 1 h | +0.1676 |
| 6 h | +0.1682 |

Six hours discards a quarter of every training day and moves the answer by
**+0.0006**, a fiftieth of the interval's half-width. The threshold is a 0.95
quantile over millions of samples and does not turn on its last few thousand.

**The search behind the number.** Sweeps had been run before and nothing was
selected on them, which is a discipline rather than a mechanism. The trial count
is now derived from what the tool actually did: passing five embargo values sets
trials to five without anyone remembering to. The result file records it, the
headline is printed as the *first* value with the best one named separately, and
a single-configuration run says it is uncorrected rather than saying nothing.

What that costs, simulated rather than cited — twenty configurations scored on
27 folds of pure noise, best kept:

```
best-of-20 on a pure null: 65.2% called significant uncorrected,
                            0.8% after deflation
```

Two runs in three. On the real result, five trials put the bar at +0.0177
against an observed +0.1676, a deflated z of +10.10, and the effect survives.
Full account in [docs/SELECTION.md](docs/SELECTION.md).

### What the numbers rest on

`titans_walkforward` refuses rather than reports when it cannot support a
number: fewer than 5 folds gets no bootstrap interval at all, a non-causal fold
aborts the run with exit 3, and a policy that took the same action on every
trade exits 4 rather than presenting a structural zero as a null result. The
permutation p-value uses the add-one estimator, so it cannot print 0 — the run
above prints `p < 5e-05`, the smallest value 20 000 sign assignments can
resolve.

The interval itself is checked rather than trusted: `tests/test_walk_forward.cpp`
runs 300 synthetic trials and asserts the nominal 95% interval covers the true
mean between 85% and 99% of the time (it measures 93.0% at n=20, which is the
known under-coverage of a percentile bootstrap at that size).

---

## Where this sits relative to published work

Asked directly: **is any of this new?** The full answer, with what was read and
what was only skimmed, is in [docs/RELATED_WORK.md](docs/RELATED_WORK.md). The
short one:

**Not new.** Running an LLM off the critical path of a low-latency system is
standard practice. The contamination taxonomy used here — stale state, entity
binding, prior-inference injection, retrieval pollution — is described in more
depth by the 2024–2026 agent-memory security literature
([AgentPoison](https://arxiv.org/abs/2407.12784),
[State Contamination in Memory-Augmented LLM Agents](https://arxiv.org/abs/2605.16746),
[Isolation as a First-Class Principle](https://arxiv.org/abs/2607.12406)).
LLM-for-trading has at least one system claiming priority.

**Different in kind.** Those papers assume an adversary and answer with a
detector. This assumes no adversary at all — only a market where a fact that was
true 800 ms ago is now wrong — and answers with expiry: `valid_until` plus
`context_generation`, checked on every read. Staleness is not an attack and does
not need a classifier; it needs a clock and a generation counter, and those cost
nothing at runtime.

**Unusual.** Three things did not turn up anywhere in the scan behind that
document:

1. **The isolation claim is executed, not asserted.** Systems papers state that
   the LLM does not interfere with execution.
   [`tests/test_lane_isolation.cpp`](tests/test_lane_isolation.cpp) hangs the
   slow lane at 200 ms per item and shows the fast lane's p99 unchanged, with
   the resulting 99.9% drop rate reported as a metric.
2. **The harness refuses.** It will not print a p99 for an operation below its
   own measured 20 ns floor, and exits non-zero if its calibration fails.
3. **The negative results shipped** — `NOT RESOLVED`, `INERT`, and the
   degenerate-classifier gate.

For calibration on the third point: a 2026 survey of LLM trading agents
([arXiv 2605.19337](https://arxiv.org/abs/2605.19337)) found that of 77 studies,
19 met minimum evaluation criteria, and of those 19, **two** reported an
extractable time-consistent split protocol and **one** an explicit
transaction-cost model. The bar the field is missing is not sophistication. It
is saying how the number was produced.

The nearest neighbour on the axis this repository actually cares about is
[Win Fast or Lose Slow](https://arxiv.org/abs/2505.19481) (HFTBench), which
makes latency a first-class evaluation variable for LLM agents. It measures the
*model* under a latency budget; this measures the *system* and asks whether the
slow lane can perturb it. Connecting the two is the most interesting unbuilt
thing here.

---

## Repository layout

```
include/titans/core/          lock-free queues, object pools, event bus, JSON, HTTP
include/titans/trading/       L2/L3 order book, matching, risk, shadow engine
include/titans/market_data/   feed handling, binary logging, replay
include/titans/lanes/         the fast/slow boundary: advisory slot, bridge, budgets
include/titans/bench/         measurement: TSC timing, noise floor, histograms, stage traces
include/titans/eval/          walk-forward, bootstrap CIs, permutation tests, change points, deflation, delivery
include/titans/context/       research framework: versioned context, contamination, LLM backends
include/titans/opt/           subset selection as a QUBO, with exact/greedy/annealing solvers
include/titans/cuda/          GPU kernels (not yet covered by the measurement rewrite)
src/, examples/, tests/       binaries, experiment drivers, 20 test modules
python/data/                  Binance archive fetch with checksum verification
python/serving/               CPU inference shim, OpenAI-compatible, for CI and GPU-less hosts
python/research/              figure generation, strictly from measured results
scripts/reproduce.sh          re-derives every number in this file
results/                      measured artifacts, including the benchmark series and live-model runs
```

`main` is the development branch and what CI builds. Every push runs the test
suite in Release and Debug, under ThreadSanitizer, and under AddressSanitizer +
UBSan; checks that every public header compiles standalone and that no
translation-unit pair produces an ODR violation; and asserts three refusals —
`titans_llm_experiment` exiting without a backend, figure generation exiting
rather than fabricating data, and the benchmark results file carrying its
`caveats` block. A regression that restored a silent fallback would otherwise
look like a passing build.

CI also runs the benchmark regression gate in all three of its directions —
quiet on a no-op series, firing on a planted 25% regression, refusing a series
too short to judge. It does **not** claim to compare a commit against its
parent: a shared runner's variance is several times the gate's sensitivity
floor, and pretending otherwise is how a performance gate becomes something
people rerun until it goes green.


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
- **Three of 28 days fail the label audit and it is not understood why.** The
  intraday-trend hypothesis was tested and rejected (see the drift sweep above).
  The headline is reported over all days with a sensitivity bound.
- **The out-of-sample number is an upper bound.** It is measured without
  advisory staleness. What the live lane achieves through the fast/slow
  boundary is a different and currently much smaller number.

## License

MIT — see [LICENSE](LICENSE).

## Disclaimer

Research and educational software. Nothing here is trading advice, and no part
of it has been run against live capital.
