# Titans — Project Engineering Guide

A handoff document for an engineer who has never seen this repository. It was
written by reading the code, the build files, the scripts, the tests, the
committed result artifacts and the full Git history, and by running the test
suite and the tools on the machine the repository lives on.

**Audit date:** 2026-09-08. **Commit audited:** `7aea82f` on branch `main`.
**Working tree at audit time:** clean except two untracked files
(`results/benchmark_GW-X570-Taichi_20260907.json`, `..._20260908.json`).

Every non-obvious claim below carries an evidence tag:

| Tag | Meaning |
|---|---|
| `[CODE]` | Read directly from source; this is what the program does. |
| `[TEST]` | Asserted by a test in `tests/`. |
| `[DOC]` | Stated by a document in the repository; not independently verified. |
| `[GIT]` | Supported by commit history. |
| `[ARTIFACT]` | Supported by a committed result file under `results/`. |
| `[RUN]` | Observed by executing something during this audit. |
| `[INFERRED]` | Deduced from several signals; nothing states it outright. |
| `[UNKNOWN]` | The repository does not contain enough evidence. |

Where a document and the code disagree, **the code wins** and the disagreement
is recorded in [§29 Documentation discrepancies](#29-documentation-discrepancies).

---

## Table of contents

1. [Executive overview](#1-executive-overview)
2. [Project evolution](#2-project-evolution)
3. [System architecture](#3-system-architecture)
4. [Call chains](#4-call-chains)
5. [Repository map](#5-repository-map)
6. [Core abstractions](#6-core-abstractions)
7. [Configuration reference](#7-configuration-reference)
8. [Data flow and artifact lifecycle](#8-data-flow-and-artifact-lifecycle)
9. [Experiment inventory](#9-experiment-inventory)
10. [Quick start](#10-quick-start)
11. [Reproducing from a fresh machine](#11-reproducing-from-a-fresh-machine)
12. [Environment and dependencies](#12-environment-and-dependencies)
13. [Hardware requirements](#13-hardware-requirements)
14. [Concurrency and system assumptions](#14-concurrency-and-system-assumptions)
15. [Failure modes and debugging](#15-failure-modes-and-debugging)
16. [Testing and validation](#16-testing-and-validation)
17. [Logging, monitoring, diagnostics](#17-logging-monitoring-diagnostics)
18. [CLI reference](#18-cli-reference)
19. [Important design decisions](#19-important-design-decisions)
20. [System invariants](#20-system-invariants)
21. [Dangerous operations](#21-dangerous-operations)
22. [Cost model](#22-cost-model)
23. [Current state of the project](#23-current-state-of-the-project)
24. [Technical debt](#24-technical-debt)
25. [If you are taking over this project](#25-if-you-are-taking-over-this-project)
26. [Change impact map](#26-change-impact-map)
27. [Result provenance](#27-result-provenance)
28. [Sources of truth](#28-sources-of-truth)
29. [Documentation discrepancies](#29-documentation-discrepancies)
30. [Open questions](#30-open-questions)

---

## 1. Executive overview

### 1.1 Project in one sentence

Titans is a single-machine C++20 research codebase that builds a hard,
*measured* boundary between a nanosecond-scale trading fast path and a
millisecond-scale language-model slow lane, together with the statistical
instrumentation needed to say whether anything crossing that boundary is worth
anything.

### 1.2 The problem being solved

There are two problems, and they are joined at one seam.

**Systems problem.** A local 8B language model answers in 10–500 ms. The
engine's fast path costs single-digit nanoseconds per primitive operation. Five
to six orders of magnitude separate them, so the model can never sit in the
request path. The question is what channel the model *can* use without
degrading the fast path, and whether the isolation holds under stress. `[CODE]`
`include/titans/lanes/`

**Evaluation problem.** Every answer the slow lane produces arrives late. So the
project measures, on real market data, how much value a decision loses purely
by arriving late, and whether anything the slow lane could compute is worth its
own latency. `[CODE]` `include/titans/eval/`, `src/tools/context_cost_main.cpp`

| | |
|---|---|
| **Input** | Binance public `aggTrades` CSVs (28 days of BTCUSDT, 2024-01-08 → 2024-02-04), and optionally a live LLM backend over HTTP. |
| **Output** | Result artifacts under `results/` (JSON + captured stdout), plus exit codes that encode verdicts. |
| **Optimised for** | Not profit. The objective is *informedness* (Youden's J = TPR − FPR) of a sizing policy against a forward adverse-selection label, and honest reporting of when a number cannot be supported. |
| **Why it matters** | The stated calibration is that of 77 surveyed LLM-trading studies, 19 met minimum evaluation criteria and 2 reported an extractable time-consistent split protocol. `[DOC]` `README.md`, citing arXiv 2605.19337. The gap this repository targets is measurement discipline, not sophistication. |

### 1.3 Current project status

**Research prototype, feature-complete against its own roadmap, actively
maintained until 2026-09-08, and not production software.** `[GIT]` `[DOC]`

Concretely:

- `docs/ROADMAP.md` marks P0 through P5 all **DONE**; the only remaining
  roadmap content is an "Explicitly not doing" list. `[DOC]`
- 34 commits, first 2025-12-16, most recent 2026-09-08. The last nine commits
  (2026-09-06 → 2026-09-08) are the roadmap items P0–P5. `[GIT]`
- The test suite passes: 20 modules, 0 failures. `[RUN]`
- `scripts/reproduce.sh --with-data` reports "All 11 stages reproduced" per the
  prior session; not re-run in full during this audit. `[DOC]`
- **There is no live-market path.** `websocket_client.hpp` speaks plain TCP
  with no TLS, and Binance's stream endpoint is `wss://` only. `[DOC]`
  `README.md` "Known limitations". Everything runs from disk.
- Nothing has been run against live capital. `[DOC]` `README.md` disclaimer.

### 1.4 Current capabilities

Things that actually work, verified by running them or by a committed artifact:

1. **Lock-free fast-path primitives with an honest measurement harness.**
   `titans_benchmark` calibrates the TSC, measures its own noise floor, and
   *refuses* to print a percentile for an operation below `3 ×` that floor.
   `[CODE]` `include/titans/bench/cycle_timer.hpp:is_resolvable`
2. **An enforced fast/slow lane boundary.** A slow lane hung at 200 ms per item
   leaves the fast lane's p99 unchanged; the bridge sheds 99.9% of
   observations and reports the drop rate. `[TEST]`
   `tests/test_lane_isolation.cpp`
3. **Real labelled market data with a leakage audit.** Forward adverse-selection
   labels, session-mean drift correction, and an AUC audit that rejects a
   dataset whose event fields predict the label. `[CODE]`
   `include/titans/context/binance_dataset.hpp`, `src/tools/dataset_main.cpp`
4. **Out-of-sample walk-forward over 28 days**, with a timestamp-checked
   causality guard, bootstrap intervals, a sign-flip permutation test, an
   embargo sweep and a multiple-testing deflation. `[ARTIFACT]`
   `results/walkforward/`
5. **Tick-to-trade latency with per-stage attribution**, a probe-cost A/B, a
   host-jitter control and a saturation sweep. `[ARTIFACT]` `results/latency/`
6. **A benchmark regression gate** using E-Divisive change-point detection,
   with a significance bar *and* an effect-size bar. `[ARTIFACT]`
   `results/regression/`
7. **Context length priced in staleness** against a live Ollama model.
   `[ARTIFACT]` `results/context/`
8. **QUBO subset selection scored against external labels**, with exact, greedy
   and simulated-annealing solvers cross-checked against each other.
   `[ARTIFACT]` `results/subset/`
9. **A one-command reproduction script** with 11 stages, including three
   assertions that specific things must *fail*. `[CODE]` `scripts/reproduce.sh`

### 1.5 Current limitations

1. **No live connectivity.** No TLS in the WebSocket client. `[DOC]`
2. **`titans_engine` feeds itself a random walk** (`rand()` at
   `src/main.cpp:384`). It demonstrates the event pipeline, not a strategy.
   `[CODE]`
3. **Three of 28 days fail the label audit and nobody knows why.** The
   intraday-trend hypothesis was implemented, swept over five window widths and
   rejected. `[ARTIFACT]` `results/walkforward/run_2024-01-08_2024-02-04.txt`
4. **Six of nine benchmark metrics are bimodal across processes**, which breaks
   the regression gate's effect-size bar on those metrics — they are effectively
   ungated. `[DOC]` `docs/REGRESSION.md`
5. **The LLM half has no positive result.** Two models were run end to end and
   both were degenerate classifiers on the contamination task; the standing
   conclusion is that the task needs a model this hardware cannot host. `[DOC]`
   `[ARTIFACT]` `results/llm/`
6. **CUDA kernels are outside the measurement rewrite** and are not built by
   default in the checked-in build (`TITANS_ENABLE_CUDA:BOOL=OFF` in
   `build/CMakeCache.txt`). `[RUN]`
7. **Two headers are orphans** — `core/debug.hpp` and
   `context/data_adapters.hpp`, compiled by CI's header-hygiene job and
   included by nothing. See [§5.9](#59-orphan-headers).

### 1.6 The most important architectural idea

**Ship the parameter, not the decision.**

The slow lane originally published its *answer* ("size down on this trade"). A
threshold crossing is only correct at the instant it is taken; the median
advisory was three trades old, and three trades is enough to move the action
onto neighbouring trades. Measured: the lane fired at nearly the right rate
(3211 actions against an ideal 3320) and agreed with the ideal on **40.1%** of
them. No expiry rule and no freshness gate can realign that, which is why
neither helped. `[ARTIFACT]` `results/lanes/advisory_decision_2024-01-15.txt`

The fix was to change the payload. `Advisory::parameter` carries the *calibrated
threshold* — a 5000-sample quantile that moves slowly — and the fast lane
evaluates the rule itself against a window that is current by construction.
Same rule, same trades, same threshold, same lane, same advisory ages:

| `--advisory` | agreement with ideal | informedness | run-to-run sd |
|---|---|---|---|
| `decision` | 40.1% | median −0.0102 | 0.0255 |
| `parameter` | **89.8%** | median **+0.2426** | **0.0028** |

`[ARTIFACT]` `results/lanes/advisory_{decision,parameter}_2024-01-15.txt`

The generalisation the repository draws: put the slow-moving quantity on the
slow lane and evaluate the fast-moving one where the state is fresh. `[DOC]`
`docs/FRESHNESS.md`

### 1.7 Fastest mental model

Three layers, and one rule that binds them.

```
  LAYER 1   A trading fast path        nanoseconds, pinned, no allocation
  LAYER 2   A one-way advisory channel a versioned ring + a drop-on-full queue
  LAYER 3   A slow lane                milliseconds, may block, may be an LLM

  THE RULE  Nothing layer 3 does may degrade layer 1, and every number
            produced about any of it must state how it was measured
            or refuse to be printed.
```

Everything under `include/titans/bench/` and `include/titans/eval/` exists to
enforce the second half of that rule. Roughly half the repository's code is
instrumentation and statistics, not trading.

---

## 2. Project evolution

Reconstructed from `git log`, commit bodies, and the "the original text follows,
unedited" sections the roadmap preserves. `[GIT]`

### 2.1 Phase 1 — the trading engine (2025-12-16)

One commit, `7a5f4e5` "feat: Implement Titans high-performance hybrid trading
system". Establishes `include/titans/core/` (SPSC queue, object pool, event bus,
event loop), `include/titans/trading/` (L2/L3 order book, matching engine, risk
manager, shadow engine), `include/titans/market_data/` (WebSocket client, binary
logger, replay engine), `include/titans/strategy/`, `include/titans/cuda/`, and
`src/main.cpp`. `[GIT]`

This layer is still present and still compiles, but almost nothing built after
August 2026 depends on it. See [§23](#23-current-state-of-the-project).

### 2.2 Phase 2 — the contamination research framework (2026-07-18)

Six commits in one day (`66d4228` … `83f3cb9`) add
`include/titans/context/`: versioned entity state with provenance and temporal
validity, a contamination injector, evaluation metrics, an experiment harness
with eight context-management baselines, an LLM interface with Ollama and vLLM
backends, distributed experiments, GPU monitoring, and a debugging toolkit.
`[GIT]`

**Research question of this phase:** does tracking context provenance and
expiry make a language model more robust to contaminated context (stale state,
entity binding, prior-inference injection, retrieval pollution)?

### 2.3 Phase 3 — first repair pass (2026-08-16)

`b3919b8` "fix: Repair compile/runtime defects and wire real contamination
mechanics" and `fc3c653` "feat: Add CI, config-driven experiments, missing
baselines, real t-tests". `[GIT]`

### 2.4 Phase 4 — the honesty rebuild (2026-08-30)

**This is the pivot that defines the current repository.** Fifteen commits in
one day. Each one removes a way the project had been producing numbers that
could not be defended. The commit bodies are unusually explicit about what was
wrong; they are the best single source on why the code looks the way it does.

| Commit | What was broken | What replaced it |
|---|---|---|
| `24dc066` | Benchmarks bracketed sub-nanosecond operations with `clock_gettime`, which costs ~40 ns. The suite reported an SPSC push (23 ns) as *cheaper than the clock used to measure it* (44 ns). | `bench/cycle_timer.hpp` (rdtsc + measured noise floor), `bench/harness.hpp` (two modes, refuses percentiles below the floor), `bench/platform.hpp` (machine fingerprint with `caveats`). |
| `2921699` | The project claimed to combine "microsecond event processing" with "LLM analytics" and had **no boundary** between them. | `lanes/advisory.hpp`, `lanes/lane_bridge.hpp`, `lanes/latency_budget.hpp`. |
| `29ca29f` | `AblationRunner` built a config with five switches and read none of them; all seven ablations were the same run relabelled. Also: label leakage, and assumed constants reported as findings. | Flags gate distinct code paths; `INERT` labelling for ablations that changed nothing; `AssumedDegradationModel` states its own limits in its output. |
| `d2f1859` | The Binance adapter labelled a trade anomalous from fields the event itself carried — leakage by arithmetic. | Forward toxic-flow label + `python/data/fetch_binance.py` with SHA256 verification + `titans_dataset` leakage audit. |
| `9573612` | A live 1.5B model answered "anomaly" to all 50 events in all 6 arms, producing exactly `0.5000` six times — which *looks* like a clean null. | Degeneracy detection, a `DEGENERATE MODEL` block, exit code 3. |
| `4fc4cd6` | `config/engine.yaml` and `config/strategy.yaml` documented behaviour that did not exist — the project has no YAML parser and `--config` was stored but never opened. | `config/engine.json`, parsed by `core/json.hpp`; unreadable config is fatal. Both YAML files deleted. |
| `a03caf9` | `titans_lanes` replayed as fast as the CPU allowed, so 97% of advisories expired and the run measured nothing. | Time-faithful replay paced to the trades' own timestamps divided by `--speed`. |
| `1c4576b` | Truncating a session broke the sample-mean drift correction, so `--max-rows` silently produced leaky labels. | `truncated()` flag + a printed warning; the audit runs on the whole session. |
| `f6da5e4` | The Streamlit dashboard asserted system status nobody checked and displayed fabricated data. | It says the data is fake. |
| `15da180` | The Dockerfile could never have built. | Fixed. |
| `f5e59b4` | Nothing re-derived the README's numbers. | `scripts/reproduce.sh`. |

The generative rule that came out of this phase, and that every later item
follows: *a tool must refuse rather than report when it cannot support a
number, and the refusal must be asserted in CI so restoring the silent fallback
looks like a failure.*

### 2.5 Phase 5 — out-of-sample evaluation (2026-09-06)

`ab95c55` adds `include/titans/eval/` (walk-forward folds, bootstrap CIs,
sign-flip permutation tests) and `titans_walkforward`. This is where the project
learns that its single-day numbers were anecdotes. `[GIT]`

It also produces the finding that reframes the whole lane design: the same
policy scores **+0.1676** out-of-sample across 27 folds without the lane
machinery, and roughly zero through it. The gap is delivery. `[ARTIFACT]`

`3593d74` writes `docs/ROADMAP.md`, ordering the remaining work by "what
measurement in this repository demands it" rather than by appeal.

### 2.6 Phase 6 — the roadmap, P0 through P5 (2026-09-06 → 2026-09-08)

Six commits, one per roadmap item. **Each one produced a result the roadmap did
not predict**, and in four of six the item's own prescription turned out to be
wrong. This pattern is the single most useful thing to understand about the
repository's culture.

| Item | Commit | The item predicted | What was measured |
|---|---|---|---|
| **P0** Freshness SLO | `79aa674` | Gating advisory age against the signal horizon would recover the lost signal. | The gate was built, swept at five settings, and **changed nothing at any of them**. The real mechanism was *misalignment*, not staleness; the fix was to change the payload (see [§1.6](#16-the-most-important-architectural-idea)). |
| **P1** Tick-to-trade | `c91abd1` | Coordinated-omission correction would recover the tail. | It recovers **under 2%** of the gap while synthesising 8,741 samples, because Tene's correction infers omission from a slow *service* time and here no single call is slow. |
| **P2** Regression gate | `16c2878` | Gate on the harness's `rep_spread_pct`. | That is the wrong noise model — two runs of the same binary 7 days apart differ 45% while each claims 1.4% internal consistency. Also found six of nine metrics bimodal across processes. |
| **P3** Statistical gaps | `6c832e2` | An embargo at the fold boundary matters. | 0 → 6 h moves informedness by **+0.0006**. Trial counting is now derived from what the tool did, not declared. |
| **P4** Context cost | `2adf1d4` | There is an interior optimum in context length. | There is not. No at-context difference resolved across 128× more context in four runs; delivered, 512 trades costs −0.2970 [−0.5044, −0.0964]. The 383 ms non-prefill floor already exceeds the 250 ms half-life. |
| **P5** QUBO subset | `7aea82f` | A null; scheduled last for that reason. | **Selection beats recency** (+0.0576 over 28 days, 21/28 days, p = 0.00045, survives deflation). But **λ = 0 wins** — the redundancy term, the source formulation's headline, costs 28% of the effect. |

**P5 nearly shipped the opposite conclusion.** The doc was first written from
one day (2024-01-09), which is one of only two days in eight where recency wins.
An assertion in `scripts/reproduce.sh` running the same tool on a different day
is what caught it. `[DOC]` `docs/SUBSET.md` §"How this document was nearly
wrong". The assertion was then *weakened* to properties a single day can support
(λ moves diversity; selection raises relevance), with the 28-day claim left to
the artifact. `[CODE]` `scripts/reproduce.sh`

### 2.7 Historical baggage still present

- **The trading engine layer** (`src/main.cpp`, `strategy/`, `matching_engine`,
  `shadow_engine`, `websocket_client`, `binary_logger`) predates the pivot and
  is not on any measured path. See [§23.5](#235-deprecated--dormant).
- **`docs/API_REFERENCE.md`** (last touched 2026-08-30) documents only core
  types and the context framework. It has no entry for `lanes/`, `eval/`,
  `bench/` or `opt/` — the entire current active path. `[CODE]` `[DOC]`
- **Two orphan headers** carried forward from earlier phases, one of which
  (`context/data_adapters.hpp`) still holds the leaky labeller that commit
  `d2f1859` was written to remove. See [§5.9](#59-orphan-headers).
- **`origin/HEAD` still points at `claude/hybrid-trading-system-dVT8H`**, which
  is an ancestor of `main` and 9 commits behind it. A `git clone` therefore
  checks out a branch that is missing all of P0–P5. `[RUN]`

---

## 3. System architecture

### 3.1 The runtime shape (what `titans_lanes` actually runs)

```
  data/raw/BTCUSDT-aggTrades-YYYY-MM-DD.csv          28 days, 2.8 GB, gitignored
        |
        v
  BinanceToxicFlowDataset::load()                    two-pass; the label needs the future
  BinanceToxicFlowDataset::build_labels()            +1 toxic / 0 benign / -1 no horizon
        |
        +--------------------------------------------------+
        |                                                  |
        v                                                  v
  ========= FAST LANE (thread, pinned) =========      ===== SCORING (main thread) =====
                                                      PolicyOutcome counters
   paced to trade timestamps / --speed                informedness = TPR - FPR
        |                                             bootstrap CI over --repeat runs
        v
   ingest ---> L2OrderBook::update_level
        |             |
        |             v
        |      AdvisoryView::current(now, generation)  <---- reads AdvisorySlot
        |             |                                      (versioned ring, kRing=16,
        |             |                                       <= 4 retries, never waits)
        |             v
        |      FlowPolicy::decide()  (in --advisory parameter mode)
        |             |
        |             v
        |      RiskManager::check_order()
        |             |
        |             v
        |      SPSCQueue<Order> ---> (dropped on the floor; there is no exchange)
        |
        +--- LaneBridge::offer(LaneObservation)   wait-free, DROPS when full,
                     |                            drop count is a reported metric
                     v
  ========= SLOW LANE (thread, pinned to another core) =========
   LaneBridge::poll()  -- drains to the NEWEST observation, folds backlog into window
        |
        v
   FlowPolicy::observe() / FlowCalibrator::quantile(0.95)
        |
        v
   sleep(simulated model latency / --speed)      <-- or a real LLM call with --slow llm
        |
        v
   AdvisorySlot::publish(Advisory{ stance, parameter, valid_until,
                                   signal_horizon_ns, context_generation })
   CadenceBudget::observe_publish(market_time)
```

`[CODE]` `src/tools/lanes_demo_main.cpp`, `include/titans/lanes/*`

### 3.2 Layer responsibilities

#### Layer 1 — fast lane

| | |
|---|---|
| **Responsibility** | Turn a trade into an order decision under a declared per-event nanosecond budget, without allocating, blocking or waiting on the slow lane. |
| **Implementation** | `titans::trading::L2OrderBook`, `titans::trading::RiskManager`, `titans::core::SPSCQueue`, `titans::lanes::AdvisoryView`, `titans::lanes::FlowPolicy`. |
| **Inputs** | One `AggTrade` at a time, plus whatever `AdvisorySlot` currently holds. |
| **Outputs** | An `Order` pushed to an outbound SPSC queue; `LatencyBudget` violation counts; a `PolicyOutcome`. |
| **Dependencies** | Nothing that can block. Explicitly: no heap allocation, no syscalls, no locks on the measured path. |
| **Failure modes** | Budget breach (counted, surfaced, never averaged away); advisory read failure (falls back to the cached value, counted as `stale_reads`); bridge full (observation dropped, counted). All four are *designed* behaviours with metrics, not error paths. `[CODE]` `include/titans/lanes/lane_bridge.hpp` |

#### Layer 2 — the boundary

Two structures, deliberately asymmetric.

**Fast → slow: `LaneBridge<Capacity>`.** A bounded SPSC ring that **drops on
full** rather than blocking or growing. This inverts the usual queue default on
purpose: blocking here would let a 200 ms model call stall the market-data
thread. `drop_rate()` is a first-class metric. `[CODE]`
`include/titans/lanes/lane_bridge.hpp`

**Slow → fast: `AdvisorySlot`.** A single-writer/multi-reader latest-value slot,
implemented as a **versioned ring with lap detection**, not a seqlock. A
textbook seqlock was tried first and tore 414 times in 2.8M reads; the C++ fence
formulation is easy to get subtly wrong because a release fence stops earlier
accesses sinking below it but does not stop *following* payload writes being
hoisted above the marker. The ring removes the hazard structurally: the writer
at generation `g` writes slot `(g+1) % 16` and only then publishes `g+1`, so a
read is valid unless the writer lapped it, which requires the generation to
advance by 15 during a 72-byte copy. `[CODE]`
`include/titans/lanes/advisory.hpp` `[TEST]` `test_advisory_slot_never_tears`
measures 0 torn in 2,564,325 reads under an adversarial writer.

#### Layer 3 — slow lane

| | |
|---|---|
| **Responsibility** | Compute something slow-moving and publish it as an `Advisory`. |
| **Implementation** | `--slow heuristic` (a `FlowPolicy` + `FlowCalibrator` with a simulated model latency) or `--slow llm` (a real HTTP call). |
| **Inputs** | `LaneObservation`s drained from the bridge. |
| **Outputs** | `Advisory` values into `AdvisorySlot`; `CadenceBudget` violation counts. |
| **Failure modes** | Hanging, dying, publishing too rarely. The first two are covered by the isolation tests; the third is `CadenceBudget`, which exists because a slow lane can satisfy every advisory-level guarantee and still be useless by simply not publishing. `[CODE]` `include/titans/lanes/freshness.hpp` |

### 3.3 The measurement plane

This is orthogonal to the three layers and is roughly half the code.

```
  bench/cycle_timer.hpp   rdtsc/rdtscp + TSC calibration + measured noise floor
        |                 is_resolvable(ns) := ns > 3 * noise_floor_ns
        v
  bench/harness.hpp       AMORTIZED mode (no percentiles, ever) vs PER_OP mode
  bench/platform.hpp      CpuTopology, pin_to_cpu, MachineFingerprint + caveats
  bench/histogram.hpp     HdrHistogram layout, 7424 counters, 0.8% precision,
                          record_corrected() for coordinated omission
  bench/stage_trace.hpp   N+1 boundary stamps for N stages, so stage means
                          sum EXACTLY to the end-to-end mean
```

```
  eval/metrics.hpp        auc() via Mann-Whitney U; PolicyOutcome (TPR/FPR/J)
  eval/bootstrap.hpp      splitmix64 Rng, percentile bootstrap CI, sign-flip test
  eval/walk_forward.hpp   expanding-window folds + causality guard + embargo split
  eval/change_point.hpp   E-Divisive + permutation p + modality assessment
  eval/deflated.hpp       expected_max_z, Sidak, Deflated Sharpe-style correction
  eval/delivered.hpp      score a decision against the trade current when it LANDS
  opt/qubo.hpp            subset selection: exact / greedy / simulated annealing
```

### 3.4 The research framework plane

`include/titans/context/` is a separate experiment about LLM context
management. It shares `core/json.hpp` and `core/http_client.hpp` with the rest
and otherwise stands alone. Its only structural connection to the lanes is
conceptual: an advisory derived from invalidated context *is* a contaminated
inference, and `valid_until` + `context_generation` bound its blast radius.
`[DOC]` `docs/ARCHITECTURE.md`

---

## 4. Call chains

### 4.1 The lane replay path (the "whole shape")

```
scripts/reproduce.sh                                    stage "End-to-end lane replay"
  -> ./build/titans_lanes <csv> --max-rows 100000 --speed 500 --ttl-ms 1000
                                --repeat 5 --advisory {decision|parameter}
     src/tools/lanes_demo_main.cpp : main()
       -> parse_args()                                  lanes_demo_main.cpp:~130
       -> BinanceToxicFlowDataset::load(path, max_rows) binance_dataset.hpp
       -> BinanceToxicFlowDataset::build_labels()       two-pass, forward horizon
       -> eval::run_policy_over_day(...)                the no-lane REFERENCE arm
       -> for repeat in 1..N:
            std::thread slow([&]{                       lanes_demo_main.cpp:367
              LaneBridge::poll()  -> drain to newest
              FlowPolicy::observe() / FlowCalibrator
              sleep(model_latency_ns / speed)
              AdvisorySlot::publish(a)                  advisory.hpp:publish
              CadenceBudget::observe_publish(...)       freshness.hpp
            })
            fast lane (this thread, pinned):
              pace to trade timestamp / speed
              AdvisoryView::current(now, generation)    advisory.hpp:current
                -> AdvisorySlot::try_read()             bounded, <= 4 attempts
                -> FreshnessMonitor::record_offered()   BEFORE the expiry check
                -> is_valid_at() / is_fresh_at()
              (parameter mode) FlowPolicy::decide()     flow_policy.hpp
              LaneBridge::offer(obs)                    lanes_demo_main.cpp:553
              score against labels[i]
       -> bootstrap over repeats, print VERDICT
       -> exit 5 if the freshness SLO was breached      lanes_demo_main.cpp:878
```

**The exit code is load-bearing.** `--advisory decision` declares the signal's
1000 ms horizon and delivers at a p99 age of ~1530 ms, so it must exit 5.
`--advisory parameter` declares the 789,282 ms stretch of market the threshold
was estimated over, so it must exit 0. Both are asserted in `reproduce.sh`; a
build where shipping a decision suddenly passes means the contract stopped being
enforced. `[CODE]` `scripts/reproduce.sh`

### 4.2 The walk-forward evaluation path

```
scripts/reproduce.sh --with-walkforward
  -> python3 python/data/fetch_binance.py --dates 2024-01-08:2024-02-04
       -> _url() -> data.binance.vision -> _download() -> _sha256() verify
       -> unzip -> _write_manifest()                    .manifest beside the CSV
  -> ./build/titans_walkforward data/raw/BTCUSDT-aggTrades-*.csv --json <path>
     src/tools/walkforward_main.cpp : main()
       -> sort days by FIRST TRADE TIMESTAMP, not argv order
       -> for each day: load_day() -> build_labels()
                        -> eval::auc(aggressor_side, labels)   LEAKAGE audit
                        -> eval::auc(trailing_flow,  labels)   LEARNABILITY audit
       -> for each Arm (one per --embargo-ms value):
            for t in 1..N-1:
              Fold f; f.train_days = t
              eval::check_fold(f)      -> exit 3 on a non-causal fold
              FlowCalibrator::quantile(0.95) over days [0, t) MINUS the embargo tail
              eval::run_policy_over_day(day[t], labels, cfg, collect=false)
              (counterfactual) same, with the threshold fitted on day t itself
       -> eval::bootstrap_mean_ci(per-fold informedness)
       -> eval::sign_flip_test(per-fold informedness)
       -> eval::deflate(observed, se, p, trials)        trials = #embargo values
       -> write_json()                                  titans.walkforward.v1
```

`[CODE]` `src/tools/walkforward_main.cpp`, `include/titans/eval/walk_forward.hpp`

### 4.3 The tick-to-trade measurement path

```
./build/titans_ticktotrade --data <csv> --rows 400000 --speed 2000 --sweep
  src/tools/pipeline_main.cpp : main()
    -> TscClock ctor                calibrate 200 ms + measure noise floor
    -> MachineFingerprint::capture()                pipeline_main.cpp:619
    -> arrival_offsets()            de-quantize the millisecond grid
                                    (43% of trades share a ms with the previous one)
    -> probe cost A/B:              StageTrace<false> vs StageTrace<true>,
                                    unpaced burst each -> ns/tick difference
    -> measure_schedule_jitter()    the SAME schedule with no pipeline -> host floor
    -> main pass, per tick:
         trace.open(due_ticks)                    stamps[0]
         ingest                     -> trace.mark(Stage::Ingest)     stamps[1]
         L2OrderBook::update_level  -> trace.mark(Stage::Book)       stamps[2]
         AdvisoryView + FlowPolicy  -> trace.mark(Stage::Signal)     stamps[3]
         RiskManager::check_order   -> trace.mark(Stage::Risk)       stamps[4]
         SPSCQueue::try_push        -> trace.mark(Stage::Order)      stamps[5]
         PipelineHistograms::fold(trace, expected_interval)
    -> optional --sweep: replay at 125x .. 64000x
    -> LatencyFormatter::cell()     prints "under floor" below 3x noise floor
    -> write_json()                 titans.ticktotrade.v1
    -> exit 5 if service p99 > --budget-ns
```

`[CODE]` `src/tools/pipeline_main.cpp`, `include/titans/bench/stage_trace.hpp`

### 4.4 The regression-gate path

```
scripts/bench_series.sh <out_dir> 20
  -> ./build/titans_benchmark --json <out_dir>/bench_NNNN_<epoch>.json   x20
       (lexical order == chronological order, by construction)

./build/titans_regression <dir> [--reference other_session.json]
  src/tools/regression_main.cpp : main()
    -> expand() dir -> *.json, sorted
    -> load_run() each                       core/json.hpp
    -> build one series per metric, KEEPING ONLY metrics present in every run
    -> for each metric:
         eval::evaluate_gate(series, cfg)    change_point.hpp:496
           -> find_change_points()           E-Divisive, recursive best_split
                -> best_split()              O(n^2) via sorted prefix sums
                -> permutation_p()           999 shuffles, add-one estimator
           -> residual_sigma()               robust sigma = 1.4826 * MAD of residuals
           -> assess_modality()              sorted 2-means + Wald-Wolfowitz runs test
           -> verdict: significant AND shift > effect_sigmas * sigma
    -> exit 0 clean | 2 refused (series too short) | 6 regression
```

`[CODE]` `src/tools/regression_main.cpp`, `include/titans/eval/change_point.hpp`

The **modality guard** is the load-bearing subtlety. An earlier version sorted
the series before clustering, so a planted +20% regression looked like "two
modes 20.6% apart" and the gate passed. The fix labels the series *in original
order*, counts runs, and computes a Wald–Wolfowitz z; a low z means the labels
are time-ordered, i.e. a step change, not two interleaved modes. `[CODE]`
`change_point.hpp:assess_modality` `[TEST]` `test_a_step_is_not_mistaken_for_two_modes`
and `test_interleaved_modes_are_not_gated` pin both directions.

### 4.5 The context-cost path (the only path that touches a live model)

```
./build/titans_context_cost --data <csv> --model llama3.1:8b --contexts 8,32,128,512,1024
  src/tools/context_cost_main.cpp : main()
    PART A (no model needed, runs under --skip-model):
      -> for delay in --delays:
           eval::run_policy_delivered(trades, labels, cfg, delay_ms)
              -> index_after_delay()     first trade STRICTLY after t + delay
           informedness at that delay
    PART B (needs a backend):
      -> stratify decision points on the at-context label
      -> for each context size:
           build prompt from the last N trades
           OllamaBackendImpl::complete()   ollama_backend.hpp -> core/http_client.hpp
              sends num_ctx; parses prompt_eval_duration -> prefill_ms
           score TWICE: at-context, and delivered (against the trade current when
                        the answer would have landed)
      -> eval::check_context_growth()  fit overhead + rate from the two SMALLEST
                                       arms; refuse an arm below that line -> exit 4
      -> degeneracy check on the WHOLE answer (one_sided, direction) -> exit 3
      -> paired bootstrap against the smallest arm
      -> write_json()                  titans.context_cost.v1
```

`[CODE]` `src/tools/context_cost_main.cpp`, `include/titans/eval/delivered.hpp`

### 4.6 The subset-selection path

```
./build/titans_subset data/raw/BTCUSDT-aggTrades-*.csv --max-rows 300000 --points 20000
  src/tools/subset_main.cpp : main()
    for each day CSV:
      -> load + label
      PART A  solver agreement: n=20 k=8, 200 real windows
                solve_exact() vs solve_greedy() vs solve_annealing()
      PART B  warm start across 2000 rolling windows (cold vs warm annealing)
      PART C  the external test, n=64 k=16, --points decision points
                for each method (recent-16, random-16, greedy x5 lambda, sa x5 lambda):
                  build_problem() -> relevance = |signed size| / max, RBF similarity
                  solve -> selection -> sum of selected signed sizes
                  calibrate a per-method threshold on the first 20% of points
                  score the remaining 80% against forward toxic labels
                  paired bootstrap vs recency over the SHARED points
      PART D  price the solve time on P4's freshness decay curve
    ACROSS DAYS:
      -> eval::bootstrap_mean_ci(per-day differences)   a DAY is one observation
      -> eval::sign_flip_test(per-day differences)
      -> eval::deflate(best method, trials = #methods)
      -> write_json()                                   titans.subset.v2
```

`[CODE]` `src/tools/subset_main.cpp`, `include/titans/opt/qubo.hpp`

### 4.7 Paths that exist but are not on any measured line

- **`titans_engine`** (`src/main.cpp`): `--config` → `load_config_file()` →
  `RiskLimits`/`StrategyConfig` → an event loop fed by `rand()` at line 384.
  Demonstrates the pipeline. `[CODE]`
- **`titans_replay`** (`src/tools/replay_main.cpp`): reads a `.bin` produced by
  `BinaryLogger`, walks it with an `IRecordVisitor`. The only `.bin` files
  present are seven 128-byte stubs under `data/logs/`, which is gitignored.
  `[RUN]`
- **`titans_experiment`, `titans_llm_experiment`, `titans_distributed_experiment`,
  `titans_config_experiment`** (`examples/`): the contamination research
  framework's drivers.

---

## 5. Repository map

45 `.hpp` headers plus 3 `.cuh` (~19,400 lines total), 31 `.cpp`/`.cu` under
`src/`, 4 under `examples/`, 20 test modules, 8 Python files (~2,070 lines).
`[RUN]`

### 5.1 `include/titans/lanes/` — the boundary. **ACTIVE, load-bearing.**

| File | Lines | Role | Depended on by |
|---|---:|---|---|
| `advisory.hpp` | 374 | `Advisory` POD, `AdvisorySlot` (versioned ring), `AdvisoryView` (cached read + expiry + freshness gate) | `titans_lanes`, `titans_ticktotrade`, `test_lane_isolation`, `test_freshness` |
| `flow_policy.hpp` | 190 | `FlowPolicy` (trailing signed order-flow rule), `FlowCalibrator` (strided quantile) | `titans_lanes`, `titans_walkforward`, `titans_ticktotrade`, `titans_context_cost` |
| `freshness.hpp` | 186 | `FreshnessPolicy`, `FreshnessMonitor` (offered vs acted-on distributions), `CadenceBudget` | `advisory.hpp`, `titans_lanes` |
| `lane_bridge.hpp` | 110 | `LaneObservation`, `LaneBridge<Capacity>` (drop-on-full) | `titans_lanes`, `titans_ticktotrade` |
| `latency_budget.hpp` | 150 | `LatencyBudget`, `BudgetScope` (RAII), `BudgetReport` | `titans_lanes` |

**`flow_policy.hpp` exists as its own file for a specific reason.** The policy
used to live inside the slow-lane thread lambda; evaluating it out-of-sample
meant writing a second copy in the walk-forward tool, and two copies of a
decision rule drift. Now four tools share one definition. `[CODE]` file header.

### 5.2 `include/titans/bench/` — measurement. **ACTIVE, load-bearing.**

| File | Lines | Role |
|---|---:|---|
| `cycle_timer.hpp` | 230 | `rdtsc_start`/`rdtsc_end`, `do_not_optimize`, `TscClock` (calibration + noise floor + `is_resolvable`) |
| `harness.hpp` | 328 | `Harness`, `BenchmarkResult`, `MeasurementMode {Amortized, PerOp}` |
| `platform.hpp` | 290 | `CpuTopology::detect()`, `pin_to_cpu()`, `MachineFingerprint::capture()` |
| `histogram.hpp` | 168 | HdrHistogram-layout `Histogram` with `record_corrected()` |
| `stage_trace.hpp` | 303 | `Stage` enum, `StageTrace<bool Instrumented>`, `PipelineHistograms`, `LatencyFormatter` |

### 5.3 `include/titans/eval/` — statistics. **ACTIVE, load-bearing.**

| File | Lines | Role |
|---|---:|---|
| `metrics.hpp` | 114 | `auc()` (Mann-Whitney U, ties averaged), `PolicyOutcome` (TPR/FPR/informedness/`degenerate()`) |
| `bootstrap.hpp` | 239 | `Rng` (splitmix64 + Lemire), `bootstrap_mean_ci`, `sign_flip_test` (exact ≤ 20, add-one estimator above) |
| `walk_forward.hpp` | 255 | `DaySummary`, `Fold`, `DayRun`, `run_policy_over_day`, `split_by_embargo`, `check_fold`, `day_label_from_path` |
| `change_point.hpp` | 548 | E-Divisive: `sorted_pair_sum`, `e_statistic`, `best_split`, `permutation_p`, `find_change_points`, `robust_sigma`, `assess_modality`, `evaluate_gate` |
| `deflated.hpp` | 206 | `normal_cdf`, `normal_sf` (erfc, so the far tail never underflows to 0), `normal_quantile` (Acklam + Halley), `expected_max_z`, `sidak_p`, `deflate` |
| `delivered.hpp` | 228 | `index_after_delay`, `run_policy_delivered`, `check_context_growth` |

### 5.4 `include/titans/opt/` — subset selection. **ACTIVE (P5 only).**

`qubo.hpp` (407 lines): `SubsetProblem`, `Qubo`, `build_subset_qubo`,
`solve_exact` (C(n,k) enumeration, refuses above `kMaxExactCombinations = 5e7`),
`solve_greedy` (MMR), `solve_annealing` (**swap moves**, so |z| = K is preserved
exactly and the cardinality penalty never binds), `select_recent`,
`select_random`, `diversity_of`, `relevance_of`.

The penalty term is retained even though it never binds, because it is what
makes the matrix an unconstrained binary quadratic form a QPU could accept
unchanged. `[TEST]` `test_penalty_never_binds_under_swap_moves` asserts every
feasible assignment carries the same penalty and therefore cannot rank anything.

### 5.5 `include/titans/core/` — primitives. **ACTIVE (partially).**

| File | Status | Notes |
|---|---|---|
| `types.hpp` | active | `Symbol`, `Price`, `Quantity`, `Timestamp`, `Side`, `Order`, `PriceLevel`, event types. Fixed-point price/quantity. |
| `spsc_queue.hpp` | active | `SPSCQueue<T, Capacity>` (cache-line separated head/tail), `MPSCQueue`. Benchmarked, tested. |
| `memory_pool.hpp` | active | `ObjectPool`, `Arena`, `ScopedArena`, `ThreadLocalPool`, `RingBuffer`. Benchmarked, tested. |
| `event_bus.hpp` | active | Benchmarked; `publish` vs `publish_prestamped` is a published finding (36% of the cost is an unconditional `clock_gettime`). |
| `json.hpp` | active | Hand-rolled parser/serializer. Used by every JSON-writing tool and by `--config`. Tested. |
| `event_loop.hpp` | dormant | epoll/io_uring loop. Used only by `titans_engine`. |
| `http_client.hpp` | active-when-used | `HttpClient`, `ConnectionPool`, `URLParser`, `SocketConnection`. Included by `context/ollama_backend.hpp` and `context/distributed_experiment.hpp`; this is how every LLM call leaves the process. |
| `debug.hpp` | **orphan / legacy** | `Logger`, `Profiler`, `MemoryTracker`, `Dumper`, `DebugConsole`. Included by nothing; compiled by CI's header-hygiene and ODR jobs only. |
| `gpu_monitor.hpp` | dormant | Shells out to `nvidia-smi`. Used by `distributed_experiment.hpp`. |

### 5.6 `include/titans/context/` — the LLM research framework. **DORMANT.**

Largest single subtree (~5,700 lines). Present, compiling, tested in part, but
its headline experiment has no positive result and nothing in P0–P5 depends on
it except `binance_dataset.hpp`.

| File | Status | Notes |
|---|---|---|
| `binance_dataset.hpp` | **ACTIVE, load-bearing** | `AggTrade`, `ToxicFlowLabelConfig`, `BinanceToxicFlowDataset`. Used by six tools. This is the one file from this subtree that the current work depends on. |
| `llm_interface.hpp` | active-when-used | `LLMBackend` interface, `PromptBuilder`, `ResponseParser`, `OllamaConfig` (incl. `num_ctx`), `LLMResponse` (incl. `prefill_ms`). |
| `ollama_backend.hpp` | active-when-used | `OllamaBackendImpl`, `VLLMBackendImpl`, `OllamaStreamingBackend`, `LLMBackendFactory`. |
| `experiment_harness.hpp` | dormant | 1064 lines. `SyntheticDataGenerator`, `AssumedDegradationModel`, eight `BaselineContextManager` subclasses, `ExperimentRunner`, `AblationRunner`. |
| `context_contaminator.hpp` | dormant | Mutates the context actually serialized into the prompt. Added in `ee67f28` after it was discovered the injector was constructed and never called. |
| `versioned_entity.hpp` | dormant | `Provenance`, `TemporalValidity`, `VersionedEntity`, `SelectiveForgetter`. |
| `experiment_persistence.hpp` | dormant | `ExperimentStore` → `experiments/`. |
| `distributed_experiment.hpp` | dormant | Multi-process experiment sharding + GPU monitoring. |
| `experiment_config_io.hpp` | dormant | Reads `config/experiment.json`. |
| `contamination_injector.hpp` | dormant | `ContaminationConfig`, `InjectionResult`, `ContaminationInjector`. Included by `experiment_harness.hpp`. |
| `evaluation_metrics.hpp` | dormant | `ModelOutput`, `MetricCalculator`, `ContextMethod`. Included by `experiment_harness.hpp`, `llm_interface.hpp`, `experiment_persistence.hpp`. |
| `data_adapters.hpp` | **orphan / legacy** | LOBSTER + Binance + binary adapters. `BinanceTradeAdapter` is the *leaky* labeller that `d2f1859` replaced. Included by nothing. |

### 5.7 `include/titans/trading/`, `market_data/`, `strategy/`, `cuda/`

| Subtree | Status | Notes |
|---|---|---|
| `trading/order_book.hpp` | **ACTIVE** | `L2OrderBook` is a real stage in `titans_ticktotrade` and is benchmarked. |
| `trading/risk_manager.hpp` | **ACTIVE** | `RiskManager::check_order` is a real stage in `titans_ticktotrade`. Contains 3 `TODO`s (cancel-all on breach, publish alert, publish position update). |
| `trading/matching_engine.hpp`, `shadow_engine.hpp` | dormant | Used only by `titans_engine`. |
| `market_data/*` | dormant | `websocket_client.hpp` has no TLS and cannot reach Binance's `wss://` endpoint. `binary_logger.hpp` + `replay_engine` back `titans_replay`. |
| `strategy/strategy_base.hpp` | dormant | `MomentumStrategy`, `StrategyEngine`. Used only by `titans_engine`. |
| `cuda/*.cuh`, `src/cuda/*.cu` | **NOT BUILT** | `TITANS_ENABLE_CUDA:BOOL=OFF` in the checked-in `build/CMakeCache.txt`. The `.cu` files are 19-21 line stubs; all kernel code is in the `.cuh` headers. Explicitly outside the measurement rewrite. `[DOC]` |

### 5.8 The 13-line `.cpp` files

`src/core/*.cpp`, `src/trading/*.cpp`, `src/market_data/*.cpp`,
`src/strategy/*.cpp` are each 13 lines: a comment saying "Implementation is
header-only for performance / This file exists for build system compatibility",
plus one `#include`. They exist so `add_library(... STATIC ...)` has translation
units. `[CODE]` They are not dead code but they are not code either. `[RUN]`

### 5.9 Orphan headers

Exactly **two** headers are `#include`d by no `.cpp`, no test, and no other
header. Verified by matching real `#include "…"` directives across `src`,
`tests`, `examples` and `include`. `[RUN]`

| Header | Lines | Assessment |
|---|---:|---|
| `core/debug.hpp` | 638 | `legacy`. A complete logging / profiling / memory-tracking / dump toolkit that nothing uses. `docs/DEBUGGING_GUIDE.md` documents it as if it were live. Its only consumer is CI: the `header-hygiene` job compiles it standalone and the ODR job links it into two translation units. |
| `context/data_adapters.hpp` | 502 | `legacy`. Contains `BinanceTradeAdapter`, the **leaky labeller** (`quantity > 10.0 \|\| \|dP\|/P > 0.001`) that commit `d2f1859` replaced with `binance_dataset.hpp`. Deleting it would remove the last copy of the defect the project spent a commit removing; keeping it risks someone including it. Nothing currently does. |

Three headers that a naive basename search flags but which are **not** orphans:
`core/http_client.hpp` (included by `context/ollama_backend.hpp` and
`context/distributed_experiment.hpp`), `context/contamination_injector.hpp` and
`context/evaluation_metrics.hpp` (both included by
`context/experiment_harness.hpp`). `[RUN]`

Every header under `include/titans/` is compiled standalone by CI's
`header-hygiene` job, so an orphan still has to keep compiling. `[CODE]`
`.github/workflows/ci.yml`

### 5.10 `src/tools/` — the entry points that matter

| File | Lines | Binary | Roadmap item |
|---|---:|---|---|
| `pipeline_main.cpp` | 933 | `titans_ticktotrade` | P1 |
| `lanes_demo_main.cpp` | 881 | `titans_lanes` | P0 |
| `context_cost_main.cpp` | 852 | `titans_context_cost` | P4 |
| `walkforward_main.cpp` | 846 | `titans_walkforward` | pre-roadmap + P3 |
| `subset_main.cpp` | 817 | `titans_subset` | P5 |
| `regression_main.cpp` | 409 | `titans_regression` | P2 |
| `dataset_main.cpp` | 265 | `titans_dataset` | data audit |
| `replay_main.cpp` | 169 | `titans_replay` | legacy |

### 5.11 `tests/` — 20 modules

Covered: SPSC queue, memory pool, event bus, order book, versioned entity,
contamination, JSON, **lane isolation**, **task design**, statistics, Binance
dataset, context contamination, **walk-forward protocol**, **freshness
contract**, **pipeline latency**, **change point**, **deflated statistics**,
**delivered scoring**, **QUBO**. The bolded ones are the load-bearing ones.

**Not covered:** `matching_engine`, `shadow_engine`, `websocket_client`,
`binary_logger`, `replay_engine`, `strategy_base`, `event_loop`, `http_client`,
`debug`, `gpu_monitor`, all CUDA, `src/main.cpp`, and every one of the eight
`src/tools/*.cpp` drivers as a whole (their *library* pieces are tested; the
drivers are exercised only through `scripts/reproduce.sh`). `[RUN]`

### 5.12 `scripts/`

| Script | Role | Safe to run? |
|---|---|---|
| `reproduce.sh` | 11-stage reproduction of every README number. | Mostly — **but see [§21](#21-dangerous-operations)**: it overwrites two tracked artifacts. |
| `bench_series.sh` | Run `titans_benchmark` N times into a directory, named so lexical order == chronological order. | Yes. Writes only where told. |
| `inject_regression.py` | Copy a benchmark series and slow one metric in the last N runs, to prove the gate fires. Deliberately leaves `rep_spread_pct` alone, because a real regression does not make a run internally noisier. | Yes; writes to a destination directory it clears first. |

### 5.13 `python/`

| Path | Role | Status |
|---|---|---|
| `data/fetch_binance.py` | Downloads from `data.binance.vision`, verifies the published SHA256 (fatal on mismatch), writes a `.manifest`. `--dates START:END` stops on the first missing day rather than leaving a hole. | **ACTIVE, load-bearing** |
| `serving/cpu_shim.py` | Minimal OpenAI-compatible `/v1/chat/completions` on CPU via `transformers`. Exists so the LLM path stays exercisable in CI and on GPU-committed workstations. | active-when-needed |
| `research/generate_figures.py` | 6 figures + 1 LaTeX table. Raises `NoResultsError` rather than fabricating; the prior version drew `np.random` data around hand-picked accuracies. | **degraded — see §29 D6** |
| `research/gpu_analysis.py` | GPU metrics plots. | dormant |
| `analytics/llm_analyzer.py` | Ollama-backed PnL attribution sidecar. | dormant |
| `visualizer/app.py` | Streamlit dashboard. Says its data is fake (commit `f6da5e4`). | dormant |

### 5.14 `results/` — 62 tracked files (40 of them the benchmark baseline series). See [§8](#8-data-flow-and-artifact-lifecycle) and [§27](#27-result-provenance).

### 5.15 Directories that are empty or stubs

- `figures/` — empty. Generated output only.
- `data/logs/` — seven 128-byte `.bin` stubs from `titans_engine`; gitignored.
- `build-asan/`, `build-tsan/` — stale local sanitizer build trees from
  2026-08-30; gitignored. CI creates its own.
- `experiments/` — referenced by `config/experiment.json` (`output_dir`) and by
  `docs/RESEARCH_FRAMEWORK.md`; **does not exist** in the checkout. `[RUN]`

---

## 6. Core abstractions

### 6.1 `titans::lanes::Advisory` — the only thing that crosses the boundary

`include/titans/lanes/advisory.hpp`

| | |
|---|---|
| **Responsibility** | Carry one slow-lane conclusion, valid for a bounded window, in a form that cannot hurt a reader. |
| **Lifecycle** | Constructed by the slow lane, copied byte-for-byte into an `AdvisorySlot` ring slot, copied out by a fast-lane reader, cached in `AdvisoryView`. Never heap-allocated, never freed under a reader. |
| **Invariant** | `static_assert(std::is_trivially_copyable_v<Advisory>)`. Adding a `std::string` member would break the whole design and the build. `[CODE]` |

Key fields, and why each exists:

| Field | Type | Why |
|---|---|---|
| `stance` | `AdvisoryStance` | `None` doubles as "nothing published yet"; a default-constructed `Advisory` is inert. |
| `size_multiplier` | `float` | The fast lane's position-limit multiplier. |
| `risk_direction` | `int8_t` | **+1 buy-side, −1 sell-side, 0 undirected.** Dropping the sign was a measured defect: an undirected advisory scored +0.013 ± 0.020 (indistinguishable from zero) where the same feature reaches AUC 0.76 with the sign aligned. `[CODE]` field comment. |
| `issued_at`, `valid_until` | `Timestamp` | Wall-clock expiry. Bounds a latency budget. |
| `signal_horizon_ns` | `Timestamp` | **How far into the future the claim reaches.** Not a lifetime. `valid_until` cannot express "advice about the next second is worthless at 900 ms; advice about the next hour is not". `0` means undeclared, and a gate with nothing to measure against must let the advisory through. |
| `context_generation` | `uint64_t` | The fast lane bumps this on a book reset / halt / reconnect. An advisory behind the current generation was reasoned from facts that no longer hold and is rejected **regardless of `valid_until`**. |
| `parameter` | `double` | **The field that fixed the project.** A slow-moving calibrated quantity the fast lane applies itself. See [§1.6](#16-the-most-important-architectural-idea). |
| `source[32]` | `char[]` | Fixed-size, because a pointer would break trivial copyability. |

Methods: `is_valid_at(now, gen)`, `age_at(now)` (clamped at 0 for clock skew),
`is_fresh_at(now, policy)`.

### 6.2 `titans::lanes::AdvisorySlot`

| | |
|---|---|
| **Concurrency** | Single writer, multiple readers. `publish()` is wait-free: no loop, no allocation, never waits on a reader. `try_read()` is bounded at `kMaxReadRetries = 4`. |
| **Constants** | `kRing = 16` (power of two, static_asserted), `kMaxReadRetries = 4`. |
| **Error condition** | `try_read` returns `false` when the writer lapped the reader on every attempt. The caller keeps its cached advisory. **Returning false is normal and safe, and is never a reason to wait.** `[CODE]` |
| **Formal caveat** | The lapped copy races under the C++ memory model. The result is detected and discarded, never used — the same trade the Linux kernel, folly, and Rust's `seqlock` crate make. `.github/tsan.supp` suppresses exactly two symbols and says so. |
| **Persistence** | None. Pure in-memory. |

### 6.3 `titans::lanes::AdvisoryView` — the fast lane's read path

The subtle part is **ordering inside `current()`**:

```cpp
const Timestamp age = cached_.age_at(now);
if (cached_.stance != AdvisoryStance::None) monitor_.record_offered(age);   // BEFORE
if (!cached_.is_valid_at(now, gen))   { ++rejected_;       return neutral_for(gen); }
if (!cached_.is_fresh_at(now, fresh_)){ ++rejected_stale_; return neutral_for(gen); }
monitor_.record_acted(age);
```

Recording the offered age *after* the expiry check would make the freshness SLO
tautological whenever the TTL is tighter than the horizon: the distribution
would be truncated at the TTL and the contract would report MET because it had
thrown away every sample that would have failed it. **That is exactly what the
first version did.** `[CODE]` in-file comment.

Two rejection counters are kept apart on purpose: `rejected()` means the advice
ran out of time; `rejected_stale()` means it was never going to be useful.

### 6.4 `titans::lanes::FreshnessPolicy` / `FreshnessMonitor` / `CadenceBudget`

`FreshnessPolicy` holds two *fractions of the signal horizon*, not durations:
`max_age_fraction` (0 disables the gate — the setting every pre-gate result was
measured under) and `slo_p99_fraction` (default 1.0: advice should not be older
than the horizon it predicts over).

`FreshnessMonitor` keeps **two distributions and conflating them would hide the
whole point**: OFFERED age (the slow lane's delivery property, what the SLO is
declared on) and ACTED-ON age (bounded by the gate by construction, so an SLO on
it is a tautology).

`CadenceBudget` measures publish intervals in **market time**, on the timestamp
of the newest observation folded into each publish — not wall time. Under a
replay the two differ by `--speed`. `have_last_` is a separate bool rather than
`last_publish_ns_ != 0`, because a publish at market time zero is a legitimate
timestamp. `[CODE]`

### 6.5 `titans::lanes::FlowPolicy` and `FlowCalibrator`

`FlowPolicy` is a pure function over a `std::deque<double>` of the last `window`
signed quantities (`aggressor_sign * quantity`).

- `observe(signed_quantity)` — `contaminate` flips the sign and halves it,
  standing in for a corrupted view of order flow.
- `warm()` — true once the window is full. Deciding before that compares a
  partial sum against a threshold calibrated on full ones.
- `decide()` — **recomputes the sum rather than maintaining it incrementally.** A
  running sum over 1.4M updates accumulates float drift, and the calibrated
  threshold would then be compared against a slightly different quantity than
  the one calibrated. Fifty adds is not the bottleneck on this side. `[CODE]`

`FlowCalibrator` collects `|net|` and returns a quantile. It is separate from the
policy because **where the samples come from is the whole question a walk-forward
asks**: `titans_lanes` draws them from a prefix of the same session; the
walk-forward draws them from earlier *days*. Samples are strided down to
`cap_per_feed = 200000` so an expanding window over a month stays bounded.
`quantile()` returns 0 on an empty sample — callers must check `size()`, because
a threshold of 0 makes `|net| > 0` fire on almost every trade.

### 6.6 `titans::bench::TscClock`

Constructed **once per process, after pinning**. Calibration spins for
`calibration_ms` (default 200) against `CLOCK_MONOTONIC`; the noise floor is the
median of 20,000 empty `rdtsc_start`/`rdtsc_end` pairs after a 2,000-iteration
warm-up.

`is_resolvable(ns) := ns > 3.0 * noise_floor_ns()`. **Everything that refuses to
print a number refuses through this predicate.**

Requires x86-64 with `constant_tsc` + `nonstop_tsc`. Falls back to
`clock_gettime` on other architectures, which silently destroys the resolution
argument — there is no runtime check that the fallback is not in use. `[CODE]`
`[INFERRED]` risk.

### 6.7 `titans::bench::Histogram`

HdrHistogram bucket layout: powers of two, 256 linear sub-buckets per octave
(`kSubBucketBits = 8`), 7,424 counters, 58 KB, allocated once. Worst-case
relative error 1/128 = 0.8%, returned by `relative_error()` rather than left for
the reader to assume.

`percentile(p)` returns the **bucket's upper bound**, deliberately, because a
latency percentile reported low is the failure mode that matters.

`record_corrected(value, expected_interval)` implements Gil Tene's coordinated-
omission correction. `expected_interval == 0` disables it and the call behaves
exactly like `record`.

**Measured limitation, and it is a headline result:** on the tick-to-trade
workload the correction recovers **under 2%** of the gap between service and
response p99 while synthesising 8,741 samples, because it infers omission from a
slow *service* time and here no single call is slow — the queue builds from
bursty arrivals. `[ARTIFACT]` `results/latency/ticktotrade_2024-01-15.json`

### 6.8 `titans::bench::StageTrace<bool Instrumented>`

`include/titans/bench/stage_trace.hpp`

The design choice: **N+1 boundary stamps for N stages, not 2N bracketing
reads.** Consequences:

1. Half the probe cost.
2. The stages **partition the interval exactly**, so stage means sum to the
   end-to-end mean. Five nested RAII scopes do not have this property — time
   leaks between them. `[TEST]` `test_a_chain_partitions_and_scopes_lose_time`
   and `test_stage_means_sum_to_the_service_mean`.

Templating on `Instrumented` means a probed and an unprobed fast path are both
compiled with **no branch in either**, so an unpaced burst of each gives the
per-tick probe cost directly. Measured: 23.3 ns per boundary, 139.7 ns per tick,
printed in the header of every run. `[ARTIFACT]`

Three end-to-end quantities: `service()` (handler work), `response()` (from when
the tick was *due*, clamped to 0 if handled early), `queueing()` = the
difference. A replay knows each tick's due time, so `response` is **exact** and
`corrected` is an *estimate* of it — printing both says how good the estimate is.

`LatencyFormatter::cell()` returns the literal string `"under floor"` below
`3 × noise_floor`. `[TEST]` `test_percentile_below_the_floor_is_refused`.

### 6.9 `titans::eval::PolicyOutcome`

Four counters — `avoided_toxic`, `missed_toxic`, `forgone_benign`,
`kept_benign` — because a market maker's two costs are asymmetric and both must
be visible.

`informedness()` = TPR − FPR (Youden's J). **Chosen over accuracy because it is
zero for any policy that ignores the label**, including one that sizes down on
everything or on nothing. With a 1.6% toxic rate a policy that never sizes down
is 98.4% accurate and worth nothing.

`degenerate()` is true when `action_rate()` is 0 or 1 — informedness is then zero
by construction, not by measurement, and `titans_walkforward` exits 4 rather
than presenting it as a null result.

J is also **prevalence-invariant** (TPR and FPR are class-conditional), which is
what licenses the stratified sampling in `titans_context_cost` Part B. `[DOC]`
`docs/CONTEXT_COST.md`

### 6.10 `titans::eval::Rng`, `bootstrap_mean_ci`, `sign_flip_test`

`Rng` is splitmix64 plus Lemire's bounded reduction, written out in the header,
**because `std::uniform_int_distribution` is not specified to produce the same
values across standard libraries** and a result file produced here would not
reproduce elsewhere. `[CODE]`

`bootstrap_mean_ci` **refuses** below `kMinSamplesForCI = 5` and **flags**
`underpowered` below `kUnderpoweredCI = 12`. `[TEST]`
`test_bootstrap_refuses_samples_too_small_to_interval`, and
`test_bootstrap_covers_at_the_rate_it_claims` runs 300 synthetic trials and
asserts nominal-95% coverage lands between 85% and 99% (it measures 93.0% at
n = 20, the known under-coverage of a percentile bootstrap at that size).

`sign_flip_test` enumerates all `2^n` assignments for `n ≤ 20` (exact) and above
that uses the **add-one estimator** `(1 + extreme)/(1 + samples)`, so it can
never print `p = 0`. `[TEST]` `test_sampled_permutation_p_is_never_zero`.

### 6.11 `titans::eval::evaluate_gate` and `assess_modality`

`GateResult` fires only when a change point is **both** significant under
permutation **and** larger than `effect_sigmas` (default 3) × the series' own
robust dispersion with its steps removed. The second bar is the one that
matters: a 2% injection into the real 40-run baseline is already *significant*
(p = 0.0050) at 1.3 sd, and a gate built on significance alone fires there,
three times more sensitive than the noise justifies. `[ARTIFACT]`
`results/regression/sensitivity.txt`

`assess_modality` returns `bimodal` only when the minority mode is large enough,
the separation wide enough, **and** the labels are *not* time-ordered
(`order_z > max_order_z`, default −2.0, from a Wald–Wolfowitz runs test on the
labels in original order). Without that last clause a planted step looks like
two modes and the gate goes silent. `[CODE]` `[TEST]`

`kMinSeriesForGate = 12` — a series shorter than that is **refused (exit 2)**,
not passed. `[TEST]` `test_short_series_is_refused_not_passed`.

### 6.12 `titans::eval::deflate`

Implements the Deflated-Sharpe-style correction: given `trials`, compute
`expected_max_z(n) = (1−γ)Φ⁻¹(1−1/n) + γΦ⁻¹(1−1/(n·e))`, subtract the resulting
bar from the observed effect, and report whether it survives.

`normal_sf(z)` uses `0.5 * erfc(z/√2)` rather than `1 − Φ(z)`, because
`1 − Φ(10.10)` is exactly zero in double precision and the tool would print
"p 0". `[TEST]` `test_the_far_tail_does_not_underflow_to_zero` asserts the direct
form stays positive *and* that the cancelling form does underflow at z = 20.

`test_correction_controls_the_best_of_n_false_discovery_rate` is the one that
justifies the whole file: best-of-20 on pure noise is called significant
**65.2%** of the time uncorrected and **0.8%** after deflation. `[DOC]`
`docs/SELECTION.md`

### 6.13 `titans::eval::run_policy_delivered` and `check_context_growth`

`run_policy_delivered(trades, labels, cfg, delay_ms)` scores each decision
against whichever trade is current when the answer would have *landed*.
`index_after_delay` returns the first trade **strictly after** `from`, so a delay
of 0 is not a no-op index lookup. `[TEST]`
`test_delivery_is_strictly_after_its_own_trade`.

**Critical property:** at `delay_ms == 0` it reproduces `run_policy_over_day`
counter for counter. Two evaluators that disagreed by a little would produce a
"cost of delay" that was really the difference between two pieces of code.
`[TEST]` `test_zero_delay_reproduces_the_undelayed_evaluator`.

`check_context_growth` fits fixed overhead + per-unit rate from the **two
smallest arms** and refuses any later arm whose prefilled tokens fall below that
line. Comparing raw token counts does not work: an 8→64 trade sweep grows 8× in
content but only 2.5× in total tokens because a 201-token system prompt
dominates the smallest arm. The first version did exactly that and called a
healthy sweep truncated. `[TEST]` `test_growth_check_tolerates_a_fixed_overhead`,
`test_growth_check_catches_a_plateau`.

### 6.14 `titans::opt::SubsetProblem` / `Qubo`

The objective, taken unchanged from the source formulation:

```
min_z  − Σ_i r_i z_i  +  λ Σ_{i<j} S_ij z_i z_j  +  P (Σ_i z_i − K)²
```

`build_subset_qubo` maps it to a matrix:

```cpp
q.at(i,i) = -p.relevance[i] + p.penalty * (1.0 - 2.0 * kk);
q.at(i,j) = q.at(j,i) = 0.5 * (p.lambda * p.sim(i,j) + 2.0 * p.penalty);
```

`[TEST]` `test_matrix_reproduces_the_written_objective` checks the matrix against
the formulation written out directly, on **feasible and infeasible** assignments.

| Solver | Cost (n=20, k=8) | Notes |
|---|---|---|
| `solve_exact` | 22.9 ms median | C(n,k) enumeration; refuses above `kMaxExactCombinations = 5e7`. `[TEST]` `test_exact_refuses_an_unreasonable_search`. |
| `solve_greedy` | 2.2 µs median | MMR. |
| `solve_annealing` | 43 µs median | **Swap moves** (one selected ↔ one unselected), so `|z| = K` is preserved exactly. |

Baselines: `select_recent` (the last K — the thing to beat) and `select_random`
(the control that makes the result readable).

### 6.15 `titans::context::BinanceToxicFlowDataset`

| | |
|---|---|
| **Responsibility** | Parse an `aggTrades` CSV and attach a forward adverse-selection label. |
| **Lifecycle** | `load(path, max_rows)` → `build_labels()` (or `build_events()`, which is written in terms of it). Two-pass by necessity: the label depends on the future. |
| **Memory** | ~1.4M trades ≈ 60 MB for a day of BTCUSDT. Loads, does not stream. |
| **Invariants** | Input must be in time order; out-of-order input is **rejected**, because it would silently corrupt every forward label. `[TEST]` `test_unordered_input_is_rejected`. Binance convention `is_buyer_maker == true` ⇒ the *seller* was the aggressor; getting it backwards inverts every label, so it is asserted rather than commented. `[TEST]` `test_aggressor_side_convention`. |
| **Label** | Toxic iff, within `horizon_ms`, the mid price moved ≥ `threshold_bps` in the aggressor's favour, **after subtracting the drift estimate**. Trades in the final `horizon_ms` have no future and are **dropped (label −1)**, not labelled benign — labelling them benign would append a block of guaranteed negatives to every run. |
| **Danger** | `truncated()` is true when `max_rows` stopped the read early. The drift correction is a **sample-mean** statistic, so on a prefix it removes the prefix's mean and leaves the local trend standing. Measured on 2024-01-15: aggressor-side AUC is 0.5012 over the full day, 0.5897 over the first 200k trades, **0.6979** over the first 20k — leakage bad enough that the audit rejects the dataset. `[CODE]` `[DOC]` |

---

## 7. Configuration reference

There is **no unified configuration system.** Three separate mechanisms exist and
they do not share a schema, a loader, or a precedence rule.

### 7.1 `titans_engine`: `config/engine.json` + flags

The only place a config file overlays defaults.

**Precedence: file first, flags second.** This required a *pre-scan* for
`--config` in `parse_args` — processing arguments in `argv` order would make the
outcome depend on where the flag appeared. `[CODE]` `src/main.cpp:158-171`

| Key | Default | Meaning | Used by | Dangerous values |
|---|---|---|---|---|
| `mode` | `"shadow"` | `shadow` / `replay` / `backtest` | `src/main.cpp:552` | An unrecognised mode falls through. |
| `symbols[]` | `["BTCUSDT","ETHUSDT"]` | Instruments. `--symbols` **replaces**, does not append. | `src/main.cpp:129` | — |
| `log_path` | `"./data/logs"` | Binary log output. | `BinaryLogger` | A path on a slow disk stalls the logger thread. |
| `risk.max_position_size` | 10.0 | | `RiskLimits` | Any change silently alters what `check_order` rejects. |
| `risk.max_order_size` | 1.0 | | `RiskLimits` | |
| `risk.max_daily_loss_usd` | 1000.0 | | `RiskLimits` | |
| `risk.max_drawdown_pct` | 5.0 | | `RiskLimits` | |
| `strategy.name` | `"Momentum_20"` | | `StrategyConfig` | |
| `strategy.lookback` | 20 | | `StrategyConfig` | |
| `strategy.threshold` | 0.02 | | `StrategyConfig` | |
| `strategy.initial_capital` | 100000.0 | | `StrategyConfig` | |
| `strategy.max_position_pct` | 0.1 | | `StrategyConfig` | |

**An unreadable or malformed `--config` is FATAL (exit 2), not a warning.**
Falling back to defaults while the operator believes their file is in effect is
how a risk limit ends up ten times larger than intended. `[CODE]`
`src/main.cpp:103-121` `[CODE]` asserted in `scripts/reproduce.sh`.

> **History:** `config/engine.yaml` and `config/strategy.yaml` used to sit here
> documenting risk limits and websocket endpoints. **No code ever read either
> one — the project has no YAML parser and never did**, while the values they
> described were hardcoded twenty lines below in `main.cpp`. Commit `4fc4cd6`
> deleted both. `[GIT]` The `_comment` array at the top of `engine.json` records
> this.

### 7.2 `config/experiment.json` — the contamination experiment batch

Read by `titans_config_experiment` via
`include/titans/context/experiment_config_io.hpp`.

| Key | Value | Meaning |
|---|---|---|
| `batch_id` | `"paper_main_comparison"` | Names the output subdirectory. |
| `base.num_events` | 5000 | Events per arm. |
| `base.num_entities` | 50 | |
| `base.event_interval_ns` | 1000000 | 1 ms spacing. |
| `base.contamination_rate` | 0.15 | |
| `base.seed` | 42 | |
| `methods[]` | 8 context strategies | `NoHistory`, `FullHistory`, `FixedWindow`, `RollingSummary`, `VectorRetrieval`, `TimeFilter`, `VersionedContext`, `VersionedIntegrity` |
| `contamination_rates[]` | `[0.05, 0.15, 0.30]` | Sensitivity sweep. |
| `seeds_per_config` | 5 | |
| `output_dir` | `"experiments"` | **This directory does not exist in the checkout.** `[RUN]` |

### 7.3 Every other tool: command-line only

`titans_lanes`, `titans_walkforward`, `titans_ticktotrade`, `titans_regression`,
`titans_context_cost`, `titans_subset`, `titans_dataset`, `titans_benchmark`,
`titans_llm_experiment` take **no config file**. All state is in flags with
in-source defaults. See [§18](#18-cli-reference).

### 7.4 Environment variables

| Variable | Read by | Default |
|---|---|---|
| `BUILD_DIR` | `scripts/reproduce.sh`, `scripts/bench_series.sh` | `build` |
| `RESULTS_DIR` | `scripts/reproduce.sh` | `results` |
| `LLM_PORT` | `scripts/reproduce.sh` | `8000` |
| `LLM_MODEL` | `scripts/reproduce.sh` | empty (skips the context-cost stage) |
| `DATA_DATE` | `scripts/reproduce.sh` | `2024-01-15` |
| `SYMBOL` | `scripts/reproduce.sh` | `BTCUSDT` |
| `WF_RANGE` | `scripts/reproduce.sh` | `2024-01-08:2024-02-04` |
| `TSAN_OPTIONS`, `UBSAN_OPTIONS` | CI sanitizer jobs | see `.github/workflows/ci.yml` |
| `OLLAMA_URL`, `LLM_MODEL` | `python/analytics/llm_analyzer.py` | `http://localhost:11434`, `llama3` |
| `TITANS_DATA_PATH`, `TITANS_LOG_PATH`, `PYTHONPATH` | set by the Dockerfile; **read by nothing in the C++ tree** `[RUN]` | |

### 7.5 CMake options

| Option | Default | Effect |
|---|---|---|
| `TITANS_ENABLE_CUDA` | `ON` (auto-off if no `nvcc`) | Builds `titans_cuda`. **`OFF` in the checked-in build tree.** |
| `TITANS_ENABLE_TESTS` | `ON` | Adds `tests/`. |
| `TITANS_ENABLE_BENCHMARKS` | `ON` | Builds `titans_benchmark` **and** defines `TITANS_BUILD_FLAGS`, which is how the benchmark JSON records its own compile flags. |
| `TITANS_USE_IO_URING` | `OFF` | Needs `liburing`; falls back to epoll with a warning. |
| `CMAKE_BUILD_TYPE` | `Release` | Release = `-O3 -DNDEBUG -march=native -flto`. **Debug = `-g -O0 -DDEBUG -fsanitize=address,undefined`** — note that Debug is a *sanitizer* build here, which is unusual and slow. |

### 7.6 Parameters that make results incomparable

Change any of these and **every committed artifact stops being a valid
comparison**:

| Parameter | Where | Why |
|---|---|---|
| `--threshold-bps` | dataset/walkforward/lanes/subset (5.0), context_cost (**2.0**) | Changes the positive class. At 5 bps the tape is ~1.5% toxic; at 2 bps ~11%. `titans_context_cost` deliberately uses 2.0 so a few hundred model calls contain enough positives, which is **why its absolute levels are not comparable with Part A's**. `[DOC]` |
| `--horizon-ms` | all (1000) | Changes both the label and the declared signal horizon. |
| `--quantile` | lanes/walkforward (0.95) | Changes the action rate. |
| `--window` | flow policy (50) | Changes what the threshold was calibrated against. |
| `--drift-window-ms` | walkforward (0 = session mean) | Changes the label. Swept and reported; 0 is the default and the headline. |
| `--max-rows` | everywhere | **See §6.15 — truncation breaks the drift correction and injects leakage.** |
| `--seed` | 42 everywhere | Bootstrap/permutation reproducibility. |
| `--speed` | lanes (1000), ticktotrade (1000) | Changes the ratio of model latency to inter-trade interval, which is the physics. |

### 7.7 Parameters that exist and do nothing

None found. `[RUN]` The `_comment` block in `config/engine.json` is the only
non-functional key and is documented as such.

---

## 8. Data flow and artifact lifecycle

### 8.1 The pipeline, end to end

```
  data.binance.vision (external, public, no API key)
        |  python/data/fetch_binance.py --dates 2024-01-08:2024-02-04
        |    - GET  BASE/daily/aggTrades/BTCUSDT/BTCUSDT-aggTrades-<date>.zip
        |    - GET  the published .CHECKSUM
        |    - SHA256 verify -> MISMATCH IS FATAL
        |    - unzip
        v
  data/raw/BTCUSDT-aggTrades-<date>.csv        ~100 MB/day, 28 days = 2.8 GB
  data/raw/BTCUSDT-aggTrades-<date>.manifest   symbol, period, source_url,
        |                                      archive_sha256, rows, columns
        |    .gitignore:  *.csv EXCLUDED, *.manifest TRACKED
        v
  BinanceToxicFlowDataset (in-memory only; no intermediate file is ever written)
        |  labels: +1 toxic / 0 benign / -1 no horizon
        v
  the tools
        |
        v
  results/<area>/<name>.json     machine-readable, schema-versioned
  results/<area>/<name>.txt      captured stdout, hand-committed
```

**There is no intermediate representation on disk.** Every tool re-parses the
CSV and re-derives the labels. A 300k-row day parses in a few seconds; a 28-day
`titans_subset` run takes minutes. `[INFERRED]` from the two-pass loader and the
absence of any cache path.

### 8.2 File formats and schemas

| Artifact | Format | Schema field |
|---|---|---|
| `data/raw/*.csv` | Headerless CSV, 8 columns: `agg_trade_id, price, quantity, first_trade_id, last_trade_id, transact_time(ms), is_buyer_maker, is_best_match`. Column order is load-bearing and recorded in `fetch_binance.py:AGG_TRADE_COLUMNS`. | — |
| `data/raw/*.manifest` | `key=value` lines. | — |
| `results/benchmark_*.json` | `{schema, machine, results[]}` | `titans.benchmark.*` |
| `results/benchmark_history/*/bench_NNNN_<epoch>.json` | same | same |
| `results/walkforward/*.json` | `{schema, protocol, config, trials, embargo_sweep[], deflation, days[28], folds[27], out_of_sample, in_sample_minus_out_of_sample, sign_flip_test}` | `titans.walkforward.v1` |
| `results/latency/*.json` | `{schema, machine, resolvable_above_ns, probe_cost_ns_per_tick, speed, ticks, ticks_already_late, trades_sharing_a_millisecond, synthesised_by_correction, orders_sent, risk_rejected, stages{5}, end_to_end{5}}` | `titans.ticktotrade.v1` |
| `results/regression/*.json` | `{schema, runs, alpha, effect_sigmas, regressions, metrics[]}` | `titans.regression.v1` |
| `results/context/*.json` | `{schema, model, skip_model, num_ctx, points, threshold_bps, horizon_ms, truncated, degenerate_arms, decay[], arms[]}` | `titans.context_cost.v1` |
| `results/subset/*.json` | `{schema, window, budget, points_per_day, days, threshold_bps, methods[12], per_day[28]}` | `titans.subset.v2` |
| `results/llm/*.json` | `{schema, backend, model, events_per_arm, entities, contamination_rate, seed, arms[]}` — **includes every raw model response** | — |

**Naming rule for LLM results** (commit `c3b48b5`):
`llm_<model with / replaced>_seed<N>_n<events>.json`. So
`llm_Qwen_Qwen2.5-3B-Instruct_seed42_n400.json` is self-describing.

**Naming rule for benchmark results:** `benchmark_$(hostname)_$(date +%Y%m%d).json`.
This is why `results/benchmark_GW-X570-Taichi_20260907.json` and `..._20260908.json`
appeared in the working tree — every `reproduce.sh` run on a new day creates one.

### 8.3 The machine fingerprint

Every benchmark JSON carries a `machine` block: `cpu_model`, `physical_cores`,
`logical_cpus`, `smt_active`, `governor`, `boost_enabled`, `clocksource`,
`invariant_tsc`, `isolcpus_set`, `kernel`, `compiler`, `build_flags`,
`hostname`, `pinned_cpu`, `tsc_ghz`, `noise_floor_ns`, and **`caveats[]`** — a
list of what was *not* controlled. On the reference host:

```
"No isolcpus/nohz_full: the kernel schedules other work on the measured core.
 Tail percentiles (p99.9+) include unrelated interference and should be treated
 as upper bounds."
"SMT is enabled: the sibling hyperthread shares execution resources..."
"Turbo/boost is enabled: sustained-load and burst measurements run at different clocks."
```

CI asserts the `caveats` block is present and that no amortized result claims a
distribution. `[CODE]` `.github/workflows/ci.yml` `benchmark-self-check`.

> **Gap:** `results/latency/ticktotrade_2024-01-15.json` has the same `machine`
> schema but `build_flags: "unrecorded"`, `pinned_cpu: -1`, `tsc_ghz: 0`,
> `noise_floor_ns: 0` — `pipeline_main.cpp:619` calls
> `MachineFingerprint::capture()` and never fills those four in, even though the
> same run prints `TSC 3.3999 GHz | floor 20.00 ns` to stdout. See §29 D7.

### 8.4 Which files can be deleted and regenerated

| Category | Files | Rule |
|---|---|---|
| **Source of truth — never delete** | `data/raw/*.manifest`, everything under `include/`, `src/`, `tests/`, `scripts/`, `config/`, `docs/` | The manifests are what make a run reproducible without shipping 2.8 GB of CSV. |
| **Regenerable from the manifest** | `data/raw/*.csv` | `python/data/fetch_binance.py`. Gitignored. |
| **Committed derived artifacts — regenerable but expensive, and the docs cite them** | `results/**` (62 tracked files; 40 are the benchmark baseline series, 22 are experiment outputs) | Regenerating requires the data and, for `results/context/context_cost_llama31_8b.*` and `results/llm/*`, a **live model**. Two of them are *overwritten* by `reproduce.sh` — see §21. |
| **Pure cache / scratch** | `results/benchmark_history/reproduce/` | Gitignored; `rm -rf`'d and rebuilt on every `reproduce.sh` run. |
| **Local build output** | `build/`, `build-asan/`, `build-tsan/` | Gitignored. |
| **Manually produced** | `results/*.txt` (captured stdout), `docs/*.md` | The `.txt` files are hand-committed transcripts. They pair with the `.json` in the same directory. |
| **Junk** | `data/logs/*.bin` (seven 128-byte stubs), `figures/` (empty) | Gitignored / empty. |

### 8.5 Overwrite behaviour

- `titans_benchmark --json PATH` — **truncates** `PATH`.
- `titans_walkforward --json PATH`, `titans_ticktotrade --json PATH`,
  `titans_subset --json PATH`, `titans_context_cost --json PATH` — all truncate.
- `scripts/bench_series.sh <dir> N` — **appends** files into `<dir>` without
  clearing it. Running it twice into the same directory silently produces a
  mixed series. `[CODE]`
- `scripts/inject_regression.py <src> <dst>` — **clears `<dst>/*.json` first.**
- `scripts/reproduce.sh` — see §21.

**No file is written atomically.** There is no temp-file-then-rename anywhere.
Two concurrent runs writing the same `--json` path will interleave. `[RUN]`

---

## 9. Experiment inventory

Nine experiments have committed artifacts. For each: what it asks, how to run
it, and whether its result is still the one the documentation cites.

---

### Experiment 1 — Fast-path microbenchmarks

| | |
|---|---|
| **Purpose** | Establish per-operation cost of the lock-free primitives, and prove the measurement method is valid before reporting anything. |
| **Hypothesis** | Pool allocation is an order of magnitude below `malloc`; queue ops are single-digit ns; `EventBus::publish` is dominated by its clock read. |
| **Entry point** | `./build/titans_benchmark [--quick] [--json PATH]` |
| **Config** | 15 repetitions, pinned to physical core 4, 200 ms TSC calibration. Hardcoded in `Harness`'s defaults. |
| **Data / model** | None; synthetic in-process workloads. |
| **Hardware** | AMD Ryzen 9 5950X, 16C/32T, `performance` governor, GCC 15.2, `-O3 -march=native -flto`. |
| **Seed** | N/A. |
| **Output** | `results/benchmark_<host>_<YYYYMMDD>.json` |
| **Metrics** | `cost_ns` (median across reps), `min_ns`, `rep_spread_pct`. **No percentiles**, by refusal. |
| **Formal result** | The README table (§"Measured performance"). |
| **Known anomaly** | **The README table matches no benchmark JSON at HEAD.** It matches `results/benchmark_GW-X570-Taichi_20260830.json` exactly *as that file existed at commit `24dc066`*; commit `f5e59b4` overwrote it. See §29 D1. |
| **Reproducible?** | The *ordering* and order of magnitude, yes, anywhere. The third significant digit, no — and §"The second digit does not reproduce either" says so. |
| **Still cited?** | Yes, prominently. |

---

### Experiment 2 — Lane isolation

| | |
|---|---|
| **Purpose** | Prove that a hung slow lane cannot degrade the fast lane, and that the advisory slot never hands back a torn value. |
| **Entry point** | `./build/tests/titans_tests` → `run_lane_isolation_tests()` |
| **Config** | Slow lane sleeps 200 ms per item — the latency of a real model. |
| **Output** | stdout only; no artifact file. |
| **Formal result** | fast-lane p99: **610.0 ns draining vs 40.0 ns hung (0.07×)**; bridge drop rate 99.9% (199,743 of 200,000); adversarial writer 2,564,325 reads, **0 torn**, 650 lapped give-ups; realistic writer 7,008,703 reads, 0 torn, 99.9912% success. `[DOC]` README |
| **Reproducible?** | Yes; runs in the default test suite in seconds. `[RUN]` |
| **Note** | This is the result `docs/RELATED_WORK.md` singles out as unusual: *systems papers state* that the LLM does not interfere; this one executes the claim. |

---

### Experiment 3 — Real-data label construction and leakage audit

| | |
|---|---|
| **Purpose** | Establish that the task requires context (event fields must not predict the label) and is learnable (trailing flow must). |
| **Entry point** | `./build/titans_dataset data/raw/BTCUSDT-aggTrades-2024-01-15.csv` |
| **Config** | `--horizon-ms 1000 --threshold-bps 5 --flow-window 50`, drift = session mean. |
| **Data** | One full day, 1,364,603 trades. |
| **Metrics** | AUC per feature; limit `|AUC − 0.5| ≤ 0.10`. |
| **Formal result** | quantity 0.5383, price 0.5735, **aggressor side 0.5012**, trailing signed flow 0.7072. Both gates pass. |
| **Known anomaly** | **Must run on the whole session.** First 20k trades → aggressor AUC 0.6979 (rejected); first 200k → 0.5897 (flagged `[near]`); all → 0.5012. `--max-rows` prints a warning saying exactly this. |
| **Exit code** | Non-zero on a dataset that fails either gate, so a pipeline notices. |
| **Reproducible?** | Yes, `scripts/reproduce.sh --with-data`. |

---

### Experiment 4 — The lane replay: decision vs parameter (ROADMAP P0)

| | |
|---|---|
| **Purpose** | Determine why a policy worth +0.16 out of the lane is worth ~0 through it, and fix it. |
| **Hypotheses tested** | (1) advisories expire before use — **dead**, TTL 250 ms→60 s moved rejection 34.4%→10.9% and the outcome not at all; (2) the bridge sheds observations — **dead**, 0 of 99,972; (3) the advice is too old, so gate it — **dead**, unresolved at every `--max-age-frac` setting; (4) the advice is *misaligned* — **this is the answer**. |
| **Entry point** | `./build/titans_lanes <csv> --max-rows 100000 --speed 500 --ttl-ms 1000 --repeat 5 --advisory {decision,parameter}` |
| **Output** | `results/lanes/advisory_decision_2024-01-15.txt`, `results/lanes/advisory_parameter_2024-01-15.txt` |
| **Formal result** | decision: agreement 40.1%, informedness median −0.0102, sd 0.0255, **SLO BREACHED, exit 5**. parameter: agreement **89.8%**, median **+0.2426**, sd **0.0028**, **SLO MET, exit 0**. `[ARTIFACT]` |
| **Known anomaly** | The lane acts more often than the reference (5254 vs 3320) because lane and reference calibrate on different subsamples — per-publish vs per-trade. **The informedness *levels* of lane and reference are therefore not directly comparable**; agreement and the decision-vs-parameter contrast are. The tool says so in its own output. `[DOC]` `docs/FRESHNESS.md` |
| **Reproducible?** | Yes, and **both exit codes are asserted** in `reproduce.sh`. |

---

### Experiment 5 — Walk-forward, 28 days (pre-roadmap + ROADMAP P3)

| | |
|---|---|
| **Purpose** | Does the flow policy work on a day it has never seen? |
| **Protocol** | Expanding window: fit on days `[0, t)`, test on day `t`. Days sorted by **first trade timestamp**, not argv order. Every fold's boundary checked on timestamps; a non-causal fold aborts with exit 3. |
| **Entry point** | `./build/titans_walkforward data/raw/BTCUSDT-aggTrades-*.csv --json results/wf.json` |
| **Data** | 28 days, 2024-01-08 → 2024-02-04 ⇒ 27 folds. |
| **Seed** | 42; 10,000 bootstrap resamples; 20,000 sign-flip assignments. |
| **Output** | `results/walkforward/BTCUSDT_2024-01-08_2024-02-04.json` + `run_2024-01-08_2024-02-04.txt`; sweeps in `btcusdt_embargo_sweep.{json,txt}` and `btcusdt_rollingdrift_300s.json`. |
| **Formal result** | out-of-sample **+0.1676, 95% CI [+0.1385, +0.1966]**; in-sample counterfactual +0.1725; in-sample − OOS **+0.0049 [−0.0032, +0.0144]** (contains zero); sign-flip p < 5e−05. `[ARTIFACT]` |
| **Null results, both kept** | (a) **3 of 28 days fail the label audit** (worst 0.157 on 2024-01-23) and the intraday-trend hypothesis was tested at 1 min / 5 min / 30 min / 2 h rolling drift and **rejected** — short windows are worse, long ones converge to the session mean. (b) **The embargo does nothing**: 0 s → 6 h moves informedness by +0.0006, a fiftieth of the interval's half-width. |
| **Sensitivity, reported below the headline** | Dropping the 3 leaky days leaves 24 folds at +0.1528 [+0.1253, +0.1797]. **Explicitly labelled post-hoc and never substituted for the headline** — "removing days by a criterion computed from the same data is how a result gets selected into existence". `[ARTIFACT]` |
| **Multiple testing** | 5 embargo trials ⇒ bar +0.0177 against observed +0.1676, deflated z +10.10, survives. |
| **Reproducible?** | Yes: `scripts/reproduce.sh --with-walkforward` (~8 min, 2.8 GB download). |
| **Caveat** | This number is measured **without** advisory staleness. It is an **upper bound** on what the live lane can reach. |

---

### Experiment 6 — Tick-to-trade latency (ROADMAP P1)

| | |
|---|---|
| **Purpose** | Produce a composite tick-to-trade figure with per-stage attribution, with the strategy logic *inside* the measured region. |
| **Entry point** | `./build/titans_ticktotrade --data data/raw/BTCUSDT-aggTrades-2024-01-15.csv --rows 400000 --speed 2000 --core 2 --slow-core 4 --sweep` |
| **Data** | 400,000 trades = 658.9 minutes of tape. |
| **Output** | `results/latency/ticktotrade_2024-01-15.{json,txt}` |
| **Formal result** | Per stage: ingest *under floor* (p99 110.3 ns, 6.6% share), book **170.0 ns mean / p99 780.9 / 40.4% share**, signal 104.6 / 210.3 / 24.9%, risk 90.8 / 130.3 / 21.6%, order *under floor* / 6.5%. End to end: **service p99 1100.9 ns**, **response p99 78,909.6 ns (72×)**, host-floor control p99 3,595.1 ns. Probe 23.3 ns/boundary, 139.7 ns/tick. `[ARTIFACT]` |
| **The finding** | The coordinated-omission correction **recovers under 2% of the gap** while synthesising 8,741 samples. Under a saturation sweep from 125× to 64000×, **service p99 falls 1844 → 621 ns while response p99 rises 36.6 µs → 2.17 ms** — the naive metric improves as the system degrades. |
| **Caveat inside the artifact** | "aggTrades carries no book depth. The book stage is fed levels synthesised from trades and trimmed to 64 per side. The **TIMING is real** … but the book's CONTENTS are not a real book, and no claim here depends on them being one." `[ARTIFACT]` |
| **Reproducible?** | The 100k smoke run is stage 8 of `reproduce.sh`. The committed 400k + sweep artifact is **not** regenerated by `reproduce.sh` (deliberately — the smoke JSON goes to `/tmp`). |

---

### Experiment 7 — Benchmark regression gate (ROADMAP P2)

| | |
|---|---|
| **Purpose** | Answer "is it slower than last week?", which nothing in the repository could do. |
| **Entry point** | `scripts/bench_series.sh results/benchmark_history/local 40` then `./build/titans_regression results/benchmark_history/local --reference results/benchmark_GW-X570-Taichi_20260906.json` |
| **Output** | `results/regression/gate_local_40.{json,txt}`, `gate_injected_20pct.txt`, `sensitivity.txt` |
| **Formal result — the sensitivity curve** | 2% injection: p 0.0050, 1.3 sd, **clean**. 5%: p 0.0010, 2.8 sd, **clean**. **6%: 3.2 sd, REGRESSION.** 20%: 9.8 sd, REGRESSION. `[ARTIFACT]` The point: a 2% injection is already *significant*; a significance-only gate fires there and gets switched off within a week. |
| **Two unplanned findings** | (a) `rep_spread_pct` is the wrong noise model — two committed runs of the same binary 7 days apart differ **45%** on `operator new/delete` while each claims 1.4% internal consistency. (b) **Six of nine metrics are bimodal across processes** — `try_push` takes 1.02 ns or 1.48 ns, decided at process start and stable for the run. |
| **Consequence** | The effect-size bar is meaningless on a bimodal metric, so **six of nine metrics are effectively ungated**. |
| **Known anomaly, documented and kept** | `reproduce.sh` once flagged a genuine +4.9% level shift in a series built moments earlier, because 20 back-to-back runs after eight heavy stages are not a stationary baseline. The fix was to assert against the **committed** baseline and merely *report* the fresh series. `[DOC]` `docs/REGRESSION.md` §"A false positive worth keeping". |
| **Reproducible?** | Yes; asserted three ways in both `reproduce.sh` and CI (quiet on the baseline, fires on a planted 25%, refuses a 6-run series with exit 2). |

---

### Experiment 8 — Context length priced in staleness (ROADMAP P4)

| | |
|---|---|
| **Purpose** | Put "more context is better" and "fresher advice is better" on the same axis. |
| **Entry point (Part A, no model)** | `./build/titans_context_cost --data <csv> --max-rows 300000 --skip-model` |
| **Entry point (Part B, live model)** | `... --model llama3.1:8b --contexts 8,32,128,512,1024 --points 400 --json results/context/context_cost_llama31_8b.json` |
| **Data / model / hardware** | 2024-01-09, 300,000 trades; llama3.1:8b via Ollama on an RTX 3090; `--threshold-bps 2.0` (not the repository's usual 5.0). |
| **Output** | `results/context/context_cost_llama31_8b.{json,txt}`, `results/context/decay_2024-01-15.json` |
| **Part A result** | informedness by delay: 0 ms +0.0759, 100 ms +0.0409, **250 ms +0.0195**, 1000 ms +0.0086, 4000 ms +0.0009. **Half the value is gone by 250 ms** against a 1000 ms horizon. |
| **Part B result** | Paired against the 8-trade arm: at-context, **no interval excludes zero at any context**. Delivered: 128 trades **−0.1864 [−0.3419, −0.0302]**, 512 trades **−0.2970 [−0.5044, −0.0964]**. |
| **The sharpest number** | At the smallest context the model spends **30 ms on prefill and 383 ms on everything else**. That floor alone exceeds the 250 ms half-life before context buys anything. |
| **Stability across runs** | Four runs are tabulated (2 × 200 points, 2 × 400 points). **The 512 arm is resolved negative in all four.** The at-context column is *identical* between runs at temperature 0; only latency varies, which is why the delivered column is the noisy one by construction. |
| **Two guards, both wrong on the first attempt** | (a) truncation detection compared raw token counts and called a healthy sweep truncated; (b) degeneracy was measured on `one_sided` alone (99–100% ⇒ "4 of 5 arms degenerate") when the decision rule is `one_sided && direction == the target's aggressor side`. Judged on the whole answer, nothing is degenerate. Both corrections are documented rather than quietly applied. `[DOC]` `docs/CONTEXT_COST.md` |
| **Reproducible?** | Part A yes, in `reproduce.sh --with-data`. Part B needs Ollama + the model; the committed artifact is a 400-point run and `reproduce.sh --with-llm` runs a 200-point one to a **different** path. |

---

### Experiment 9 — QUBO subset selection (ROADMAP P5)

| | |
|---|---|
| **Purpose** | Take a published subset-selection formulation unchanged and score it against labels the objective never sees. |
| **Entry point** | `./build/titans_subset data/raw/BTCUSDT-aggTrades-*.csv --max-rows 300000 --points 20000 --json results/subset/subset_28days.json` |
| **Config** | n = 64 items, K = 16 budget, λ ∈ {0, 0.1, 0.5, 1.0, 2.0}, 200 annealing sweeps, seed 42, threshold 5 bps. Relevance = `|signed size|` normalised by the window max; similarity = dense RBF over (position, signed size). |
| **Data** | All 28 days, ~21,400 decision points per day, first 20% for per-method threshold calibration. |
| **Output** | `results/subset/subset_28days.{json,txt}` (843 lines of transcript) |
| **Formal result** | Every selection method's 95% CI over days excludes zero. Best: **greedy λ = 0, +0.0576 [+0.0323, +0.0827], better on 21/28 days**, sign test p = 0.00045, deflated for 11 methods (bar +0.0209, z +2.85) — **survives**. Control: random-16 **−0.0009 [−0.0230, +0.0213]**, 12/28. `[ARTIFACT]` |
| **The finding** | **λ = 0 wins and raising λ is monotonically worse** (+0.0576 → +0.0414). The redundancy term — the source formulation's headline — costs 28% of the effect. Greedy λ=0 takes 2.0 µs; the annealer takes 119 µs to reach the same answer; exhaustive takes 23 ms. |
| **Solver agreement (Part A)** | At n=20, k=8 over 200 real windows, annealing matched exhaustive on 155/200 at λ=0 rising to 196/200 at λ=2 — at λ=0 the objective is flat across many near-tied top-K sets. Greedy and exact coincide exactly at λ=0. |
| **Known anomaly** | **This experiment nearly shipped the opposite conclusion**, written from 2024-01-09 alone. See §2.6. |
| **Second known anomaly** | Part B's warm-start verdict is **not trustworthy as printed** — see §29 D2. |
| **Reproducible?** | The 28-day run, yes, given the data (minutes). `reproduce.sh` runs a **single-day** version and asserts only single-day-supportable properties. |

---

### Experiment 10 (dormant) — LLM context contamination

| | |
|---|---|
| **Purpose** | Does versioned/provenance-tracked context make a model robust to contaminated context? |
| **Entry point** | `./build/titans_llm_experiment --backend vllm --port 8000 --events 400 --entities 12 --contamination 0.4 --seed 42 --out results/llm` |
| **Output** | `results/llm/llm_Qwen_Qwen2.5-1.5B-Instruct_seed42_n50.json`, `llm_Qwen_Qwen2.5-3B-Instruct_seed42_n400.json` — **raw responses included**. |
| **Formal result** | **Neither run supports a conclusion.** Qwen2.5-1.5B (CPU, 50 events) answered `anomaly` to *every* event in all 6 arms ⇒ exactly 0.5000 six times, which is a constant classifier, not a null. Qwen2.5-3B (GPU, 400 events) had 5 of 6 arms ≥98% one class. Exit code 3. |
| **The diagnosis, which is more interesting than the failure** | Reading the raw responses, the 3B model *was* applying the rule but comparing against records belonging to **other entities**. Entity levels spread across [50, 500], so a foreign reference makes almost any value look extreme. **That is the entity-binding failure mode this project studies, occurring spontaneously on clean context.** |
| **What was tried and stopped** | Three prompt revisions (state the numeric tolerance, require filtering to matching `entity`, add a worked example) changed the reasoning text on **45 of 60 trials and not one classification**. A fourth revision is on the "explicitly not doing" list, because tuning until the number comes out right is the failure mode the repository exists to prevent. |
| **Standing conclusion** | The task needs a more capable model than this hardware can host. The infrastructure is verified correct (context sizes track the strategy, contamination reaches the prompt, latency scales with context, every raw response recorded). |
| **Reproducible?** | Only with a backend. Not run in this audit. |

---

### Experiment 11 (stand-in) — Assumed-degradation ablations

`./build/titans_experiment` runs the eight context strategies against
`AssumedDegradationModel`, which computes accuracy from **hardcoded
multipliers** (0.5 for entity binding, 0.7 for stale state) plus one Bernoulli
draw. **It cannot answer "which contamination type hurts a model most", because
its ranking is exactly the ranking of those constants**, and the program says so
in its own output. `[DOC]`

It does produce one genuine output: the `INERT` label. `no_temporal` is
identical to the full system because selective forgetting already caps context
age at the per-entity revisit interval (~50 ms for 50 entities at 1 ms spacing),
inside the 2.0 s temporal window. **An identical row is not a result saying the
component does not matter; it is the experiment saying it cannot tell**, and it
is labelled that way with the reason. `[DOC]` README

---

## 10. Quick start

**Goal: in 5–10 minutes, know whether the repository works on your machine.**

```bash
git clone git@github.com:GeoffreyWang1117/High-Performance-Hybrid-Trading-System.git
cd High-Performance-Hybrid-Trading-System
git checkout main          # REQUIRED: origin/HEAD points at an older branch
cmake -B build -DCMAKE_BUILD_TYPE=Release -DTITANS_ENABLE_CUDA=OFF
cmake --build build -j"$(nproc)"
./build/tests/titans_tests
```

Expected tail:

```text
=== Test Summary ===
Passed: 20
Failed: 0
```

Then the measurement self-check, which is the single most informative thing you
can run:

```bash
./build/titans_benchmark
```

Expected shape (numbers will differ; the *verdict* should not):

```text
 SECTION 0 - MEASUREMENT METHODOLOGY SELF-CHECK

TSC 3.3999 GHz | single-shot floor: 20.00 ns (p99 30.00 ns) | resolvable above 60.00 ns

Naive clock-bracketed timing, as used before this rewrite:
  cost of now_ns() itself, measured with now_ns():      40.00 ns (p50)
  cost of an integer increment, same method:            20.00 ns (p50)
Same integer increment, amortized batch timing:          0.239 ns/op

VERDICT
  [confirmed] The naive method measures the clock, not the operation.
  [confirmed] Amortized timing resolves the same increment at 0.239 ns/op.
```

### What failure means

| Symptom | Diagnosis |
|---|---|
| `Failed: 1` or more, with a `✗` line | A real regression. Find the module name on the `✗` line and read that test file. |
| `titans_benchmark` exits non-zero | **The harness is not calibrated on this host.** Every latency number you produce here is meaningless until this passes. Usual causes: no invariant TSC, a non-x86-64 host, or a virtualized clocksource. Check `grep -o 'constant_tsc\|nonstop_tsc' /proc/cpuinfo \| sort -u` and `cat /sys/devices/system/cpu/current_clocksource`. |
| Build fails on `-march=native` | You are cross-compiling or on an unusual CPU. Override `CMAKE_CXX_FLAGS_RELEASE`. |
| `GoogleTest` errors at configure time | `find_package(GTest QUIET)` found a broken install. Either fix it or hide it; the suite does not need GTest. See §29 D9. |
| Tests hang | Most likely `test_lane_isolation`, which spawns threads and sleeps 200 ms per item. Give it 30 s before concluding. |

### The 30-second version if you only want to see the headline results

```bash
cat results/latency/ticktotrade_2024-01-15.txt        # per-stage + end-to-end
cat results/lanes/advisory_parameter_2024-01-15.txt   # the lane that works
tail -40 results/walkforward/run_2024-01-08_2024-02-04.txt
tail -30 results/subset/subset_28days.txt
```

---

## 11. Reproducing from a fresh machine

Assumes Git, a shell, and nothing else.

### Step 1 — Clone and select the right branch

```bash
git clone git@github.com:GeoffreyWang1117/High-Performance-Hybrid-Trading-System.git
cd High-Performance-Hybrid-Trading-System
git checkout main
git log --oneline -1        # expect 7aea82f or later
```

> **This step is not optional.** `origin/HEAD` resolves to
> `claude/hybrid-trading-system-dVT8H`, an ancestor of `main` that is **9
> commits behind** and contains none of P0–P5. A default clone gives you a
> repository without the tick-to-trade tool, the regression gate, the context
> cost tool, or the subset selector. `[RUN]`

### Step 2 — System dependencies

```bash
# Ubuntu / Debian
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build python3 python3-pip git
```

Minimum versions, from `README.md` and `CMakeLists.txt`: **GCC 13+ or Clang
16+** (C++20), **CMake 3.20+**, **Python 3.10+**. The reference host runs GCC
15.2.0, CMake 4.2.3, Python 3.14.6. `[RUN]`

### Step 3 — Python environment

The README suggests conda; pip works for everything except the model shim.

```bash
python3 -m pip install --user numpy pandas matplotlib seaborn scipy requests pyarrow
```

`seaborn` is needed **only** by `python/research/generate_figures.py` and
`gpu_analysis.py`. It is **not installed on the reference host**, which has a
consequence for one `reproduce.sh` assertion — see §29 D6. `[RUN]`

For the CPU model shim only:

```bash
python3 -m pip install --user torch transformers fastapi uvicorn
```

### Step 4 — Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DTITANS_ENABLE_TESTS=ON -DTITANS_ENABLE_BENCHMARKS=ON \
      -DTITANS_ENABLE_CUDA=OFF
cmake --build build -j"$(nproc)"
ls build/titans_*        # expect 13 executables + build/tests/titans_tests
```

`-DTITANS_ENABLE_CUDA=OFF` is what the checked-in build tree uses and what CI
uses. Turning it ON requires `nvcc` and builds kernels that **no measured result
depends on**.

### Step 5 — Credentials

**None.** `data.binance.vision` is a public archive with no API key and no rate
limit — that is why it was chosen. `[CODE]` `python/data/fetch_binance.py`
docstring.

### Step 6 — Smoke test

```bash
./build/tests/titans_tests           # expect Passed: 20, Failed: 0
./build/titans_benchmark             # expect a passing self-check
```

### Step 7 — Data (~18 MB for one day, ~2.8 GB for the walk-forward)

```bash
# One day, for most stages
python3 python/data/fetch_binance.py --symbol BTCUSDT --date 2024-01-15 --out data/raw

# 28 days, for the walk-forward and the subset experiment
python3 python/data/fetch_binance.py --symbol BTCUSDT --dates 2024-01-08:2024-02-04 --out data/raw
```

Each download is SHA256-verified against the archive's published `.CHECKSUM`;
a mismatch is fatal. `--dates` skips days already present and **stops on the
first missing day rather than leaving a hole**, because a gap would silently
turn an expanding window into a discontinuous one.

### Step 8 — The full reproduction

```bash
scripts/reproduce.sh                      # 6 stages, no network, no GPU
scripts/reproduce.sh --with-data          # 11 stages, needs 2024-01-15
scripts/reproduce.sh --with-walkforward   # + 28 days, ~8 min, 2.8 GB
scripts/reproduce.sh --with-llm           # + a live model on $LLM_PORT
```

Exit code = number of failed stages. Expected on the reference host with
`--with-data`: `All 11 stages reproduced.` `[DOC]`

> **Read §21 before running this with `--with-data` or `--with-walkforward`.**
> Both overwrite tracked artifacts.

### Step 9 — Individual experiments

```bash
# Label audit                (Experiment 3)
./build/titans_dataset data/raw/BTCUSDT-aggTrades-2024-01-15.csv

# The lane, both payloads    (Experiment 4)
for K in decision parameter; do
  ./build/titans_lanes data/raw/BTCUSDT-aggTrades-2024-01-15.csv \
      --max-rows 100000 --speed 500 --ttl-ms 1000 --repeat 5 --advisory $K
  echo "exit $?"     # expect 5 for decision, 0 for parameter
done

# Walk-forward              (Experiment 5)
./build/titans_walkforward data/raw/BTCUSDT-aggTrades-*.csv \
    --embargo-ms 0,1000,60000,3600000,21600000 --json /tmp/wf.json

# Tick to trade             (Experiment 6)
./build/titans_ticktotrade --data data/raw/BTCUSDT-aggTrades-2024-01-15.csv \
    --rows 400000 --speed 2000 --core 2 --slow-core 4 --sweep --json /tmp/t2t.json

# Regression gate           (Experiment 7)
BUILD_DIR=build scripts/bench_series.sh /tmp/series 40
./build/titans_regression /tmp/series --reference results/benchmark_GW-X570-Taichi_20260906.json

# Freshness decay, no model (Experiment 8, Part A)
./build/titans_context_cost --data data/raw/BTCUSDT-aggTrades-2024-01-15.csv \
    --max-rows 300000 --skip-model

# Subset selection          (Experiment 9)
./build/titans_subset data/raw/BTCUSDT-aggTrades-*.csv \
    --max-rows 300000 --points 20000 --json /tmp/subset.json
```

### Step 10 — With a live model

```bash
# GPU, vLLM
vllm serve Qwen/Qwen2.5-7B-Instruct --port 8000
./build/titans_llm_experiment --backend vllm --port 8000 --events 1000 --out results/llm

# No spare GPU (also what CI uses)
python3 python/serving/cpu_shim.py --model Qwen/Qwen2.5-1.5B-Instruct --port 8011
./build/titans_llm_experiment --backend vllm --port 8011 --events 1000

# Ollama, for the context-cost experiment
ollama serve                      # default port 11434
ollama pull llama3.1:8b
./build/titans_context_cost --data data/raw/BTCUSDT-aggTrades-2024-01-09.csv \
    --max-rows 300000 --model llama3.1:8b --contexts 8,32,128,512,1024 \
    --points 400 --json /tmp/ctx.json
```

`titans_llm_experiment` **will not run without a reachable backend.** There is
no stand-in fallback, and CI asserts the refusal message.

### Step 11 — Figures

```bash
python3 python/research/generate_figures.py --results results
```

`Unverified.` **This was not successfully run during the audit** — `seaborn` is
not installed on the reference host and the script fails at import. Whether it
produces sensible figures from the current `results/` tree is unknown; the
loader `rglob`s *every* `*.json` under `results/`, which now includes benchmark,
walk-forward, latency, regression and subset files that share no schema with the
contamination results it expects. `[RUN]` `[UNKNOWN]`

### Step 12 — Validation checklist

| Check | Command | Expected |
|---|---|---|
| Tests | `./build/tests/titans_tests` | `Passed: 20, Failed: 0` |
| Harness calibration | `./build/titans_benchmark` | exit 0, `[confirmed]` twice |
| Label audit | `./build/titans_dataset <full day csv>` | exit 0, two `[pass]` lines |
| Lane contract | `titans_lanes --advisory decision` | **exit 5** |
| Lane contract | `titans_lanes --advisory parameter` | **exit 0** |
| Walk-forward protocol | `titans_walkforward ...` | exit 0 (3 = protocol violation, 4 = degenerate) |
| Gate, quiet | `titans_regression results/benchmark_history/local` | exit 0 |
| Gate, fires | inject 25% then re-run | **exit 6** |
| Gate, refuses | 6-file series | **exit 2** |
| Everything | `scripts/reproduce.sh --with-data` | `All 11 stages reproduced.` |

---

## 12. Environment and dependencies

### 12.1 Verified reference environment

Measured on the machine the repository lives on. `[RUN]`

| | |
|---|---|
| OS | Linux 7.0.0-29-generic (Ubuntu) |
| CPU | AMD Ryzen 9 5950X, 16 physical cores, 32 logical, SMT on, boost on, `performance` governor |
| Clocksource | `tsc`, invariant |
| `isolcpus` | **not set** |
| GPU | 2 × NVIDIA GeForce RTX 3090, 24 GB each |
| Compiler | g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0 |
| CMake | 4.2.3 |
| CUDA | `nvcc` present at `/usr/local/cuda/bin/nvcc`; **the build has CUDA OFF** |
| Python | 3.14.6 |
| Python packages present | numpy 2.5.2, pandas 3.0.5, matplotlib 3.11.1, scipy 1.18.1, requests 2.34.2, pyarrow 25.0.1, uvicorn 0.52.4 |
| Python packages **absent** | **seaborn**, torch, transformers, fastapi, streamlit, plotly |
| GTest | present in `~/miniconda3/envs/titans` — which changes how tests register with ctest (see §29 D9) |

### 12.2 Hard requirements

| Requirement | Why | What breaks without it |
|---|---|---|
| **x86-64 with `constant_tsc` + `nonstop_tsc`** | `bench/cycle_timer.hpp` uses `rdtsc`/`rdtscp` and calibrates against `CLOCK_MONOTONIC`. | Falls back to `clock_gettime`, silently destroying the resolution argument the whole measurement plane is built on. **There is no runtime check that the fallback is not in use.** `[CODE]` |
| **C++20** | Designated initializers, `<numbers>`, concepts-adjacent template usage. `CMAKE_CXX_STANDARD_REQUIRED ON`. | Configure fails. |
| **`unsigned __int128`** | `eval::Rng::below` uses Lemire's multiply-shift. | Compile error on a compiler without it. |
| **POSIX `sched_setaffinity`, `sched_getcpu`** | `bench/platform.hpp` pinning. | Linux-only; no macOS/Windows path. `[CODE]` |
| **`/sys/devices/system/cpu/...` and `/proc/cpuinfo`** | `MachineFingerprint::capture()` reads governor, boost, clocksource, topology. | Fingerprint fields come back empty; CI's `caveats` assertion may fail. |
| **`-march=native`** | Release flags. | Not portable to a different microarchitecture; a binary built here may `SIGILL` elsewhere. |

### 12.3 Soft / optional

| Optional | Needed for | Fallback |
|---|---|---|
| CUDA 11+ | `titans_cuda` | Auto-disabled with a warning if `nvcc` is absent; **nothing measured depends on it**. |
| `liburing` | `TITANS_USE_IO_URING=ON` | Falls back to epoll with a warning. |
| GoogleTest | Nothing — the suite has its own runner. | Without it, one `add_test`. With it, `gtest_discover_tests` misbehaves; see §29 D9. |
| Ollama / vLLM | `titans_context_cost` Part B, `titans_llm_experiment`, `--slow llm` | Tools **refuse to run** rather than substituting. |
| `seaborn` | `generate_figures.py`, `gpu_analysis.py` | None; import fails. |
| `streamlit`, `plotly` | `python/visualizer/app.py` | None. |
| `torch`, `transformers`, `fastapi` | `python/serving/cpu_shim.py` | None. |

### 12.4 External services

| Service | Used by | Notes |
|---|---|---|
| `https://data.binance.vision/` | `python/data/fetch_binance.py` | Public archive, no key, no rate limit. Monthly archives publish after month end; daily files exist for roughly the last few months — **so a 2024 daily URL may 404 in the future**. `[CODE]` docstring. |
| Ollama HTTP (`localhost:11434`) | `titans_context_cost`, `--slow llm`, `llm_analyzer.py` | |
| vLLM / OpenAI-compatible HTTP (`localhost:8000`) | `titans_llm_experiment` | |
| `nvidia-smi` | `core/gpu_monitor.hpp` | Shells out. |

**No database. No message queue. No cloud service. No secrets.** `.gitignore`
excludes `.env`, `*.pem`, `*.key`, `credentials.json` defensively; none exist.
`[RUN]`

### 12.5 Docker

`Dockerfile` is a three-stage build (`builder` on `nvidia/cuda:12.2.0-devel-ubuntu22.04`,
`runtime`, `dev`). `docker-compose.yml` brings up `titans-engine`,
`titans-dashboard` (Streamlit), `ollama`, and a `dev` profile.

**The runtime image is out of date with the CMakeLists.** It copies 7 of the 14
executables — `titans_engine`, `titans_replay`, `titans_benchmark`,
`titans_dataset`, `titans_lanes`, `titans_experiment`, `titans_llm_experiment` —
and **omits `titans_walkforward`, `titans_ticktotrade`, `titans_regression`,
`titans_context_cost`, `titans_subset`**, i.e. every tool built for P1–P5. It
also does not copy `scripts/` or `data/`, so `reproduce.sh` cannot run in the
image. `[CODE]` See §29 D8.

---

## 13. Hardware requirements

Only what the repository's own scripts, logs, docs and artifacts support.

### 13.1 Minimum — build and smoke test

| | |
|---|---|
| CPU | Any x86-64 with invariant TSC. Fewer cores just makes the build slower. |
| RAM | 8 GB `[DOC]` `docs/RESEARCH_FRAMEWORK.md` |
| Disk | ~2 GB for the source, build tree and one day of data. |
| GPU | None. |
| Network | None, if you skip `--with-data`. |

The test suite runs in seconds. `[RUN]` `titans_benchmark` takes about a minute.

### 13.2 Recommended — normal development

| | |
|---|---|
| CPU | ≥ 8 physical cores. `titans_lanes` and `titans_ticktotrade` pin **two distinct physical cores** (`--fast-core`/`--slow-core`, `--core`/`--slow-core`). |
| RAM | 16 GB. A full day of BTCUSDT is ~1.4M trades ≈ 60 MB in `BinanceToxicFlowDataset`; the tools hold one day at a time. `[CODE]` |
| Disk | 5 GB (one day of data + build trees). |
| Governor | `performance`. Every committed benchmark was taken under it, and the fingerprint records the governor in force. |

### 13.3 Full experiment reproduction

| | |
|---|---|
| Disk | **~2.8 GB for 28 days of BTCUSDT `aggTrades`.** `[RUN]` `du -sh data/raw` |
| Time | `--with-walkforward` ≈ 8 min after the download. `[DOC]` `scripts/reproduce.sh` header. |
| CPU | The 5950X reference host. Numbers from a different CPU will differ in absolute terms; the README says only the *ordering* and order of magnitude should carry. |
| GPU (for Experiment 8) | **RTX 3090, 24 GB**, for llama3.1:8b via Ollama. `[ARTIFACT]` |
| GPU (for Experiment 10) | 24 GB+ VRAM for a 7–8B model at bf16. `[DOC]` `docs/RESEARCH_FRAMEWORK.md` |

Model-size table, from the docs (**what the code supports, not what has been
run**): 1.5B ≈ 3 GB (any backend, or the CPU shim); 3B ≈ 6 GB; 7–8B ≈ 16 GB
(vLLM); 32B ≈ 64 GB or ~20 GB quantized. `[DOC]`

### 13.4 Not required, contrary to appearances

- **CUDA.** The kernels are not on any measured path and the checked-in build
  has them off.
- **A tuned trading server.** Every result was taken on a workstation *without*
  `isolcpus`/`nohz_full`, with SMT and turbo on, and every results file says so.
  "A p99 from this host is not a p99 from a tuned trading server." `[DOC]`
- **Two GPUs.** The reference host has two RTX 3090s; nothing uses more than one.
  `[INFERRED]`

---

## 14. Concurrency and system assumptions

### 14.1 Process and thread model

**Single process, at most two worker threads.** There is no worker pool, no
scheduler, no IPC, and no network server.

| Tool | Threads |
|---|---|
| `titans_lanes` | main (fast lane, pinned to `--fast-core`) + 1 slow lane (`--slow-core`) |
| `titans_ticktotrade` | main (fast lane, `--core`) + 1 slow lane (`--slow-core`) |
| `titans_walkforward`, `titans_subset`, `titans_regression`, `titans_dataset`, `titans_context_cost` | **single-threaded** |
| `titans_benchmark` | single-threaded, pinned to physical core 4 by default |
| `titans_engine` | an `EventLoop` on epoll |
| `titans_tests` | `test_lane_isolation` spawns threads |

`titans_walkforward` is deliberately single-threaded: *"this evaluates the
policy deterministically: same rule, same window, no threads, no advisory
expiry"*, because `titans_lanes`'s own verdict on the same policy is that
scheduling noise swamps the effect. `[CODE]` `walk_forward.hpp` header.

### 14.2 Synchronisation primitives — the complete list

| Primitive | Where | Discipline |
|---|---|---|
| `SPSCQueue<T,N>` | outbound orders, `LaneBridge` | one producer, one consumer, cache-line-separated head/tail, `try_push`/`try_pop` never block |
| `AdvisorySlot` | slow → fast | single writer, many readers, versioned ring, bounded read (≤ 4 attempts) |
| relaxed `std::atomic<uint64_t>` counters | `LaneBridge`, `LatencyBudget`, `AdvisorySlot::read_retry_exhausted_` | read from a reporting thread; **must not introduce ordering cost on the fast path** |
| `std::thread` + join | tool `main()`s | |

**There is no mutex, no condition variable, and no lock anywhere on a measured
path.** `[RUN]`

### 14.3 Memory ordering

`AdvisorySlot::publish` does a **relaxed** load of `generation_`, writes the
ring slot, then a **release** store. `try_read` does an **acquire** load, copies,
then an acquire load again and compares. `[CODE]`

The lapped copy is a formal data race. It is detected (`g1 - g0 >= kRing - 1`)
and discarded, never returned. `.github/tsan.supp` suppresses exactly
`titans::lanes::AdvisorySlot::try_read` and `::publish` and states that
correctness rests on `test_advisory_slot_never_tears` (0 torn in 2,564,325
reads), **not on TSan's silence** — "If that test is ever weakened, this
suppression stops being justified." `[CODE]`

### 14.4 Backpressure, drops, and retry

Three "failure" paths that are all designed behaviours with metrics:

1. `LaneBridge::offer` returns false and increments `dropped_`. A drop rate near
   1.0 means the slow lane is effectively blind and its advisories should be
   distrusted — **which is why it is reported rather than absorbed**.
2. `AdvisorySlot::try_read` returns false; the caller keeps its cached advisory
   and increments `stale_reads_`. **Never a reason to wait.**
3. `AdvisoryView::current` rejects an expired or generation-stale advisory
   (`rejected_`) or a too-old one (`rejected_stale_`) and returns the
   `fallback` stance. Kept apart on purpose.

**There is no retry-with-backoff anywhere and no timeout except the HTTP client's.**
`[RUN]`

### 14.5 Hidden assumptions

These are not stated in one place and each one will bite:

1. **`titans_lanes` and `titans_ticktotrade` assume two free physical cores.**
   On a busy machine the pinning succeeds and the measurement is garbage. The
   fingerprint records `isolcpus_set: false`, which is the only warning.
2. **The benchmark assumes physical core 4 exists and is idle** (`Harness`
   default `pin_core_index = 4`). Core 0 is avoided because Linux puts most
   kernel work there. `[CODE]`
3. **Two runs writing the same `--json` path corrupt each other.** No atomic
   write, no lock file. `scripts/bench_series.sh` timestamps filenames to
   seconds, so two series started in the same second into the same directory
   will collide on name.
4. **`bench_series.sh` appends into an existing directory** without clearing it.
   A second run silently produces a mixed series and `titans_regression` will
   read it as one continuous history.
5. **Back-to-back benchmark runs share thermal and DVFS state**, so a series
   built by `bench_series.sh` is a **lower bound** on the dispersion CI will see
   across commits and days. The script says so in its own banner. `[CODE]`
6. **The measurement plane assumes the TSC is invariant and does not check at
   runtime.** On a host without it, `cycle_timer.hpp` silently uses
   `clock_gettime` and every "resolvable" verdict becomes wrong.
7. **`titans_ticktotrade` de-quantizes the millisecond grid.** 43% of trades
   share a millisecond with the previous one; left as recorded, everything after
   the first in a cluster is trivially late and the response tail would measure
   Binance's timestamp resolution. The tool spreads each millisecond's trades
   evenly across it — order-preserving and maximum-entropy given what the feed
   records. **Anyone comparing `response` figures to another tool's must know
   this happened.** `[ARTIFACT]` transcript header.
8. **Replay speed is a physics parameter, not a convenience.** At `--speed 1000`
   a 50 ms model behaves like a 50 µs one against a tape running 1000× faster.
   Changing `--speed` changes the experiment, and the first version of
   `titans_lanes` proved it by finishing 5.2 h of tape in tens of milliseconds
   with 97% of advisories expired.

### 14.6 Idempotency

| Operation | Idempotent? |
|---|---|
| `fetch_binance.py` | Yes — skips days already present. |
| Any `--json PATH` write | Yes in effect (truncate + rewrite), but **not atomic**. |
| `bench_series.sh` | **No** — appends. |
| `inject_regression.py` | Yes — clears the destination first. |
| `reproduce.sh` | **No** — writes a new dated benchmark JSON each day, and overwrites two tracked artifacts. |

---

## 15. Failure modes and debugging

### 15.1 Exit codes — the primary diagnostic surface

Exit codes here are **semantic**, not just success/failure. Several are asserted
by `reproduce.sh` and CI, so changing one breaks the build.

| Tool | 0 | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|---|
| `titans_lanes` | ran, SLO met | data error | usage | — | — | **freshness SLO breached** | — |
| `titans_walkforward` | ran and reported | data error | usage | **protocol violation — non-causal fold** | **policy degenerate on every fold** | — | — |
| `titans_ticktotrade` | ok | data error | usage | — | — | **service p99 over `--budget-ns`** | — |
| `titans_regression` | clean | — | **refused: series too short** | — | — | — | **REGRESSION** |
| `titans_context_cost` | ok | error | usage | **every model arm degenerate** | **prompt was truncated** | — | — |
| `titans_subset` | ok | error | usage | — | — | — | — |
| `titans_dataset` | audit passes | audit fails | usage | — | — | — | — |
| `titans_llm_experiment` | ok | **no backend** | — | **degenerate model** | — | — | — |
| `titans_engine` | ok | — | **unreadable/malformed `--config`** | — | — | — | — |
| `titans_benchmark` | self-check passed | **self-check failed** | — | — | — | — | — |

### 15.2 Specific failures

#### Build fails: "header does not compile standalone"

CI's `header-hygiene` job requires every file under `include/titans/` to compile
alone. A new header that relies on a transitive include will fail there and not
locally. **Check:** `g++ -std=c++20 -fsyntax-only -Iinclude <(echo '#include "titans/x/y.hpp"
int main(){}')`.

#### Build fails: ODR violation

CI links two translation units that both include
`context/distributed_experiment.hpp`, `context/ollama_backend.hpp` and
`core/debug.hpp`. A non-`inline` free function in a header fails here.

#### `titans_benchmark` exits non-zero

The self-check failed. Everything downstream is meaningless. Diagnose in order:

```bash
grep -o 'constant_tsc\|nonstop_tsc' /proc/cpuinfo | sort -u   # need both
cat /sys/devices/system/cpu/current_clocksource               # want: tsc
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor      # want: performance
```

#### `titans_dataset` exits 1 — leakage

**First check whether `--max-rows` is set.** Truncation breaks the sample-mean
drift correction; the aggressor side then scores 0.5897 at 200k rows and 0.6979
at 20k, against a 0.10 deviation limit. Run on the whole session.

If it fails on the whole session, that day genuinely leaks. Three of the 28
committed days do, and nobody knows why. Do not silently drop them.

#### `titans_walkforward` exits 3 — protocol violation

A fold's training data reaches past its test day's first trade. Causes: a
duplicated or re-published archive; two overlapping days; a corrupt CSV. The
message names the two timestamps. **Do not work around this by reordering the
argument list** — the tool sorts by first trade timestamp precisely so argument
order cannot be a claim.

#### `titans_walkforward` exits 4 — degenerate policy

The policy took the same action on every trade, so informedness is zero by
construction. Almost always a threshold of 0 from an empty `FlowCalibrator`, or
`--quantile` at an extreme.

#### `titans_lanes --advisory decision` exits 0

**This is a failure, not a success.** It means the freshness contract stopped
being enforced. `reproduce.sh` asserts `exit == 5` here.

#### `titans_context_cost` exits 4 — prompt truncated

The serving stack held less context than was asked for and truncated silently.
Raise `--num-ctx` (default 16384) or lower `--contexts`. The guard fits overhead
plus rate from the two smallest arms; note it **finds where truncation becomes
large, not where it begins** — a tenth of a prompt lost is inside tolerance.

#### `titans_context_cost` exits 3 / `titans_llm_experiment` exits 3 — degenerate model

The model answered the same thing regardless of input. **Do not interpret the
deltas.** This is the failure that produced six exact `0.5000`s and looked like
a clean null. Use a more capable model.

#### `titans_llm_experiment` exits 1 — no backend

By design. There is no stand-in. CI asserts both the exit code and the message
"will not fall back to a stand-in".

#### `titans_regression` exits 6 on a series you just built

Probably not a regression. Twenty back-to-back runs on a busy machine genuinely
contain level shifts. `reproduce.sh` reports this case and does **not** count it
as a failure; the pass/fail assertions run against the committed baseline
instead. `[CODE]` `[DOC]` `docs/REGRESSION.md`

#### A metric is never gated

It is probably one of the six bimodal ones. The effect-size bar is computed
against a dispersion that a bimodal distribution inflates, so nothing clears it.
`titans_regression --list` prints the series.

#### `titans_ticktotrade` segfaults

**Historical, fixed.** An `SPSCQueue<Order, 65536>` held by value in a
stack-local `Pipeline` overflowed the 8 MB stack. The queue is now heap-allocated
at 4096 slots via `std::unique_ptr`, mirroring `EventBus`. `[GIT]` If you enlarge
a queue held by value, re-check the stack.

#### `generate_figures.py` fails

On this host: `ModuleNotFoundError: No module named 'seaborn'`. **This exit is
indistinguishable from the intended refusal as far as `reproduce.sh` is
concerned** — see §29 D6.

#### `ctest` takes eleven minutes

You have GoogleTest installed. See §29 D9. Run `./build/tests/titans_tests`
directly instead.

### 15.3 Recommended debugging order

1. **Does `./build/tests/titans_tests` pass?** If not, nothing else matters.
2. **Does `./build/titans_benchmark` exit 0?** If not, every timing number on
   this host is meaningless.
3. **Read the exit code**, then §15.1. These tools say what is wrong in the code
   itself.
4. **Read the tool's own stdout.** Every tool prints its caveats, its refusals
   and its verdict inline. The committed `.txt` artifacts are what a healthy run
   looks like — diff against them.
5. **Check `--max-rows`.** Half the surprising results in this project's history
   were truncation.
6. **Check pinning and load.** `taskset -c` conflicts and a busy machine both
   look like regressions.
7. **Sanitizers**: `cmake -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -g -O1"`,
   or TSan with `TSAN_OPTIONS="suppressions=.github/tsan.supp"`.

---

## 16. Testing and validation

### 16.1 What exists

| Layer | Where | What it covers |
|---|---|---|
| **Unit** | `tests/test_{spsc_queue,memory_pool,event_bus,order_book,json,versioned_entity}.cpp` | Data structures and the JSON parser. |
| **Property / statistical** | `test_walk_forward`, `test_deflated`, `test_change_point`, `test_statistics` | Bootstrap coverage, permutation exactness, the far-tail underflow, the best-of-n false-discovery rate. |
| **Contract** | `test_lane_isolation`, `test_freshness` | Isolation under a hung slow lane; the offered-vs-acted-on distinction; cadence breach. |
| **Instrument self-check** | `test_pipeline_latency` | The stamp chain partitions exactly; scopes lose time; a stage below the floor is refused. |
| **Design gate** | `test_task_design`, `test_binance_dataset` | The task must be unsolvable without context and solvable with it. |
| **Algorithm cross-check** | `test_qubo` | Every solver checked against exhaustive search, not against each other. |
| **Reproduction** | `scripts/reproduce.sh` | 11 stages, including three refusals that must fail. |
| **CI** | `.github/workflows/ci.yml` | 6 jobs. |

### 16.2 Running

```bash
./build/tests/titans_tests            # the real runner; seconds
cd build && ctest --output-on-failure  # see §29 D9 before using this
scripts/reproduce.sh --with-data       # the full gate
```

### 16.3 The tests that are load-bearing

These encode findings, not just correctness. **Breaking one silently invalidates
a published number.**

| Test | What it protects |
|---|---|
| `test_advisory_slot_never_tears` | The TSan suppression is only justified while this passes. 0 torn in 2,564,325 reads. |
| `test_hung_slow_lane_does_not_slow_fast_lane` | The central isolation claim. |
| `test_a_chain_partitions_and_scopes_lose_time` | Why `StageTrace` is a stamp chain and not five RAII scopes. |
| `test_stage_means_sum_to_the_service_mean` | The property that lets the share column exist. |
| `test_percentile_below_the_floor_is_refused` | The refusal that keeps `under floor` honest. |
| `test_zero_delay_reproduces_the_undelayed_evaluator` | Makes P4's "cost of delay" a cost of delay and not a difference between two evaluators. |
| `test_a_step_is_not_mistaken_for_two_modes` + `test_interleaved_modes_are_not_gated` | Both directions of the modality guard. Without the first, a planted regression passes. |
| `test_the_far_tail_does_not_underflow_to_zero` | Asserts the direct form stays positive **and** that the cancelling form does underflow. |
| `test_correction_controls_the_best_of_n_false_discovery_rate` | 65.2% → 0.8%. The justification for `deflated.hpp` existing. |
| `test_matrix_reproduces_the_written_objective` | Checks feasible **and infeasible** assignments against the formulation written out directly. |
| `test_penalty_never_binds_under_swap_moves` | Stops the cardinality penalty becoming a tuned parameter reported as a result. |
| `test_bootstrap_covers_at_the_rate_it_claims` | 300 trials, 85–99% coverage band, measures 93.0% at n=20. |
| `test_task_is_not_solvable_without_context` | The design gate: 0.5575 balanced accuracy context-free vs 0.9592 with per-entity context. |
| `test_a_delayed_decision_misaligns_a_delayed_parameter_does_not` | The P0 finding, in a test. |

### 16.4 The three "honesty gates"

`reproduce.sh` asserts three things **must fail**, because each is a way the
repository used to produce numbers from nothing:

```bash
titans_llm_experiment --backend vllm --port 59999   # must exit non-zero
generate_figures.py --results /nonexistent          # must exit non-zero
titans_engine --config /nonexistent.json            # must exit non-zero
```

CI additionally greps the *message* for the first two. `reproduce.sh` does not,
which matters for the second — see §29 D6.

### 16.5 CI jobs

| Job | Matrix | What it does |
|---|---|---|
| `build-and-test` | Release, Debug | Build, `ctest`, `titans_experiment` smoke, the two honesty gates **with message greps**. |
| `thread-sanitizer` | — | RelWithDebInfo + TSan, suppressions scoped to two symbols. |
| `address-sanitizer` | — | ASan + UBSan, `halt_on_error=1`. |
| `benchmark-self-check` | — | `titans_benchmark --quick`, then asserts the JSON carries `caveats`, `cpu_model`, `compiler`, and that **no amortized result claims a distribution**. |
| `regression-gate` | — | Gate quiet on the committed baseline; fires on a planted 25%; **refuses a 6-run series with exit 2**; then *reports* (never fails on) the runner's own noise. |
| `header-hygiene` | — | Every header compiles standalone; no ODR violation across two TUs. |

**What CI explicitly does not claim:** that this commit is slower than its
parent. *"A shared runner's variance is several times the gate's sensitivity
floor, and pretending otherwise is how a performance gate becomes something
people rerun until it goes green."* `[CODE]` `.github/workflows/ci.yml`

### 16.6 What is NOT protected by any automated test

This is the important half of this section.

1. ~~**The README's headline latency table.**~~ **Now protected** by
   `scripts/check_readme_table.py`, which compares every row against the named
   artifact on cost, minimum and throughput, and runs both as a `reproduce.sh`
   stage and as a CI step. It is the **only** prose-versus-artifact check in the
   repository. §29 D1.
2. ~~**`docs/SUBSET.md`'s warm-start claim.**~~ **Now true**, and the verdict it
   describes is a paired bootstrap interval rather than an absolute epsilon.
   Still not *tested* — no test asserts the tool refuses to announce a direction
   on an unresolved difference. §29 D2, §30.2 Q28.
3. **The 28-day subset result.** `reproduce.sh` deliberately asserts only
   single-day-supportable properties. The +0.0576 headline rests on the
   committed artifact alone.
4. **The 400k-row tick-to-trade artifact.** `reproduce.sh` runs a 100k smoke
   test to `/tmp`.
5. **The four-run context-cost stability table.** Needs a model.
6. **Every `src/tools/*.cpp` driver as a whole.** Argument parsing, JSON writing
   and report formatting have no unit tests; they are exercised only through
   `reproduce.sh`.
7. **`matching_engine`, `shadow_engine`, `websocket_client`, `binary_logger`,
   `replay_engine`, `strategy_base`, `event_loop`, `http_client`, `debug`,
   `gpu_monitor`, all CUDA, and `src/main.cpp`.**
8. **Anything in Python.** There is no `pytest` suite despite `pytest` being in
   `python/requirements.txt`. `[RUN]`

---

## 17. Logging, monitoring, diagnostics

### 17.1 There is no logging framework in use

`include/titans/core/debug.hpp` (638 lines) defines `Logger` with levels and
categories, `Profiler`, `ProfiledScope`, `MemoryTracker`, `Dumper` and a
`DebugConsole`. **Nothing includes it.** `[RUN]` `docs/DEBUGGING_GUIDE.md`
documents it as if it were live; treat that document as aspirational.

Every tool writes to **stdout via `std::printf`**, in a report format designed to
be read by a person and captured to a `.txt` artifact. There is no log level, no
`--verbose`, and no log file.

### 17.2 What the tools report about themselves

This is the actual monitoring surface, and it is unusually rich:

| Signal | Source | Meaning |
|---|---|---|
| `caveats[]` | `MachineFingerprint` | What was **not** controlled during the measurement. In every benchmark JSON. |
| `rep_spread_pct` | `BenchmarkResult` | (median − min)/min across repetitions. **A within-process interference indicator, and explicitly the wrong noise model for a regression gate.** |
| `probe_cost_ns_per_tick` | `titans_ticktotrade` | What the instrument itself costs. Printed in every run's header. |
| `resolvable_above_ns` | `TscClock` | Below this, the tool prints `under floor` rather than a number. |
| `host_floor_control` | `titans_ticktotrade` | The same arrival schedule with **no pipeline attached** — the machine's own jitter. p99 3595.1 ns on the reference host. If your response tail does not clear this, you measured the machine. |
| `LaneBridge::drop_rate()` | `titans_lanes` | Fraction the slow lane never saw. Expected to be high. |
| `AdvisoryView::stale_reads/rejected/rejected_stale` | `titans_lanes` | Three distinct reasons advice did not reach a decision. |
| `FreshnessMonitor` offered vs acted-on | `titans_lanes` | Two distributions kept apart on purpose. The SLO is declared on **offered**. |
| `CadenceBudget::violations()` | `titans_lanes` | The slow lane failing to publish often enough, even when nothing expires. |
| `LatencyBudget::violations()` | `titans_lanes` | Per-stage breaches, counted, never averaged away. |
| `trades_sharing_a_millisecond` | `titans_ticktotrade` | 171,995 of 400,000 (43.0%). Explains the response tail. |
| `synthesised_by_correction` | `titans_ticktotrade` | 8,741 samples the omission correction invented. |
| `degenerate_arms`, `truncated` | `titans_context_cost` | Both guards' verdicts, in the JSON. |
| `INERT` | `titans_experiment` | An ablation that changed nothing, with the reason. |
| `NOT RESOLVED` | `titans_lanes` | The tool declining to score a policy whose median sits inside two run-to-run standard deviations. |

### 17.3 GPU monitoring

`include/titans/core/gpu_monitor.hpp` shells out to `nvidia-smi` and offers
`GPUMonitor` / `GPUMetricsLogger` / `GPUMetricsReporter`. Used only by
`context/distributed_experiment.hpp`. `python/research/gpu_analysis.py` plots the
result. Not on any measured path. `[RUN]`

### 17.4 Profiling

No `perf` integration, no flamegraph tooling, no `--profile` flag. The intended
profiling instrument is `titans_ticktotrade` itself: it attributes end-to-end
cost across five stages with a probe whose cost is measured and printed.
`docs/REGRESSION.md` names `perf` counters as the missing tool for explaining
the bimodality, and says so as an open item rather than shipping a guess.

### 17.5 Suggested triage order for a suspicious number

1. Is there a `caveats` block, and what does it say?
2. Did the tool print `under floor`, `NOT RESOLVED`, `INERT`, `refused`, or a
   non-zero exit? Those are answers, not errors.
3. For a latency figure: which of `service`, `corrected`, `response`? They differ
   by 72× on this workload and only `response` is tick-to-trade.
4. For a policy figure: was `--max-rows` set? Was the label audit run on the
   whole session?
5. For a comparison: was it paired? Every within-day comparison in this
   repository resamples the shared decision points jointly.
6. For a "best" figure: how many things were tried? `--trials` and
   `eval/deflated.hpp` exist because best-of-20 on pure noise is called
   significant 65.2% of the time.

---

## 18. CLI reference

Every binary is `./build/<name>`. Flags below are transcribed from `--help` on
the built binaries. `[RUN]`

### `titans_benchmark`

```
titans_benchmark [--quick] [--json PATH]
```
Fast-path microbenchmarks with a methodology self-check. **Exits non-zero if the
self-check fails.** `--json` writes `{schema, machine, results[]}`.

### `titans_dataset`

```
titans_dataset <aggTrades.csv> [--horizon-ms N] [--threshold-bps X]
               [--max-rows N] [--flow-window N]
```
Labels one day and audits the label. Exit 1 = the audit failed. **Run without
`--max-rows`**; truncation breaks the drift correction.

### `titans_lanes`

```
titans_lanes <aggTrades.csv> [options]
  --slow heuristic|llm          slow-lane implementation
  --contaminate                 corrupt the slow lane's input
  --max-rows N                  trades to replay (default 400000)
  --horizon-ms N                toxic-flow horizon (default 1000)
  --threshold-bps X             toxic-flow threshold (default 5)
  --ttl-ms N                    advisory lifetime (default 250)
  --speed X                     replay speed vs real time (default 1000)
  --repeat N                    independent replays (default 3)
  --quantile Q                  warn above this |flow| quantile (default 0.95)
  --advisory decision|parameter what the slow lane ships (default decision)
  --max-age-frac F              reject advice older than F x horizon (0 = off)
  --slo-p99-frac F              declared SLO on offered-age p99 (default 1.0)
  --cadence-frac F              slow lane must publish every F x horizon (0.25)
  --budget-ns N                 fast-lane per-event budget (default 500)
  --fast-core N --slow-core N   physical cores to pin to
```

Example, and the exit code is the assertion:

```bash
./build/titans_lanes data/raw/BTCUSDT-aggTrades-2024-01-15.csv \
    --max-rows 100000 --speed 500 --ttl-ms 1000 --repeat 5 --advisory parameter
# exit 0 (SLO MET);  the same with --advisory decision must exit 5
```

### `titans_walkforward`

```
titans_walkforward <day1.csv> <day2.csv> ... [options]
  --window N            trailing trades in the flow window (default 50)
  --quantile Q          warn above this |net flow| quantile (default 0.95)
  --horizon-ms N        toxic-flow horizon (default 1000)
  --threshold-bps X     toxic-flow threshold (default 5)
  --drift-window-ms N   rolling drift estimate (default 0 = session mean)
  --max-rows N          truncate each day (SMOKE ONLY)
  --contaminate         corrupt the policy's view of order flow
  --resamples N         bootstrap replicates (default 10000)
  --seed N              deterministic seed (default 42)
  --embargo-ms LIST     comma-separated; the FIRST is the headline,
                        the rest are sensitivity
  --trials N            configurations already evaluated elsewhere
  --json PATH           write the full result
```

Days are sorted by first trade timestamp, **not argument order**, and every
fold's boundary is checked against timestamps before any number is reported.

### `titans_ticktotrade`

```
titans_ticktotrade --data <aggTrades.csv> [options]
  --rows N          trades to replay (default 400000)
  --probe-rows N    trades for the probe-cost A/B (default 200000)
  --speed X         replay speed multiplier (default 1000)
  --core N          pin the fast lane here
  --slow-core N     pin the slow lane here
  --depth N         book levels held per side (default 64)
  --budget-ns N     fail (exit 5) if end-to-end service p99 exceeds this
  --sweep           also replay at 125x..64000x to find saturation
  --json PATH       write the distributions as JSON
```

### `titans_regression`

```
titans_regression <dir | file.json ...> [options]
  --reference FILE  a run from another session, for the timescale report
  --metric NAME     gate only this metric (default: all)
  --alpha X         permutation significance (default 0.05)
  --sigmas K        shift must exceed K x series dispersion (default 3)
  --permutations N  shuffles per test (default 999)
  --seed S          permutation seed (default 42)
  --min-segment N   points required on each side of a split (default 5)
  --list            print the series and exit, no gating
  --json PATH       write the verdict as JSON

exit: 0 clean, 2 refused (series too short), 6 regression
```

### `titans_context_cost`

```
titans_context_cost --data <aggTrades.csv> [options]
  --model NAME        Ollama model (default llama3.1:8b)
  --port N            Ollama port (default 11434)
  --contexts LIST     trades of context per arm (default 8,32,128,512)
  --points N          decision points per arm (default 240)
  --delays LIST       delay sweep for the reference curve, ms
  --threshold-bps X   toxic-flow label threshold (default 2.0)
  --horizon-ms N      toxic-flow horizon (default 1000)
  --num-ctx N         Ollama context window (default 16384)
  --skip-model        reference curve only, no model calls
  --json PATH         write the result

exit: 0 ok, 1 error, 3 every model arm degenerate,
      4 the prompt was truncated before it reached the model
```

Note the **default `--threshold-bps 2.0`**, not the repository's usual 5.0. See
§7.6.

### `titans_subset`

```
titans_subset <day1.csv> [day2.csv ...] [options]
  --window N        items the slow lane chooses from (default 64)
  --budget K        items it may keep (default 16)
  --lambdas LIST    redundancy weights to sweep
  --points N        decision points (default 40000)
  --sweeps N        annealing sweeps (default 200)
  --threshold-bps X toxic-flow label threshold (default 5)
  --json PATH       write the result

exit: 0 ok, 1 error
```

*"More than one day aggregates the per-day differences with a bootstrap interval
and a sign test, the same protocol the walk-forward uses. **One day is an
anecdote.**"* — the tool's own help text, added after P5's near-miss.

### `titans_llm_experiment`

```
titans_llm_experiment --backend vllm|ollama --host H --port N [--model M]
                      --events N --entities N --contamination R --seed S
                      --out DIR
```
Exit 1 with no backend (asserted in CI), exit 3 on a degenerate model.

### `titans_engine`

```
titans_engine [--mode shadow|replay|backtest] [--config FILE] [--symbols LIST]
```
`--config` is applied **first**, flags override. An unreadable or malformed file
is fatal. Feeds itself a random walk.

### `titans_replay`

```
titans_replay <log_file> [--info] [--stats] [--verbose]
              [--speed n] [--start ts] [--end ts]
```
Reads a `BinaryLogger` `.bin`. Legacy.

### `titans_experiment` / `titans_distributed_experiment` / `titans_config_experiment`

```
titans_experiment                          # assumed-degradation stand-in + ablations
titans_distributed_experiment              # multi-process sharding + GPU metrics
titans_config_experiment config/experiment.json
```

### Python entry points

```bash
python3 python/data/fetch_binance.py --symbol BTCUSDT --date 2024-01-15 --out data/raw
python3 python/data/fetch_binance.py --symbol BTCUSDT --dates 2024-01-08:2024-02-04
python3 python/data/fetch_binance.py --symbol BTCUSDT --month 2024-01
python3 python/serving/cpu_shim.py --model Qwen/Qwen2.5-1.5B-Instruct --port 8011
python3 python/research/generate_figures.py --results results       # Unverified, see §29 D6
python3 scripts/inject_regression.py <src> <dst> --metric NAME --pct 25 --last 12
```

### There is no network API

No server, no endpoint, no authentication. The only HTTP is **outbound** to a
local model backend. `[RUN]`

---

## 19. Important design decisions

Each decision, the evidence for why, and the trade-off accepted.

### 19.1 Why a versioned ring instead of a seqlock

**Evidence:** `[GIT]` `[CODE]` `[TEST]`. The header states a seqlock was tried
first and `test_seqlock_never_tears` caught it handing back **414 torn values in
2.8M reads**.

**Decision:** remove the hazard structurally rather than order around it. The
writer at generation `g` writes slot `(g+1) % 16` and only then publishes `g+1`,
so it never touches a slot a reader could be reading unless it **laps** — which
needs 15 generations during a 72-byte copy, and is detected by re-reading the
generation.

**Trade-off:** 16 × `sizeof(Advisory)` of memory instead of one slot, and a
formal race in the lapped case that TSan reports and a scoped suppression
silences. **The C++ fence formulation of a seqlock is easy to get subtly
wrong**, because a release fence stops earlier accesses sinking below it but not
following payload writes being hoisted above the marker.

### 19.2 Why the bridge drops instead of blocking

**Evidence:** `[CODE]` header of `lane_bridge.hpp`; `[TEST]` isolation test
measures a 99.9% drop rate under a hung consumer.

**Decision:** invert the usual queue default. Blocking or growing is correct for
a batch pipeline and catastrophic here — a 200 ms LLM call would stall the
market-data thread.

**Trade-off:** the slow lane samples the market rather than mirroring it. The
drop rate is therefore a **first-class metric**, not a swallowed error: *"if the
slow lane is losing 90% of observations, that is a capacity fact the operator
must see."* Oversizing the ring only delays and hides the drop signal.

### 19.3 Why ship a parameter instead of a decision

**Evidence:** `[ARTIFACT]` the decision/parameter pair of transcripts, plus
`[TEST]` `test_a_delayed_decision_misaligns_a_delayed_parameter_does_not`.

**Decision:** put the slow-moving quantity (a 5000-sample quantile) on the slow
lane; evaluate the fast-moving rule where the state is fresh.

**Trade-off:** the fast lane now carries the rule, so the "slow lane" is no
longer where the intelligence lives. **This is the whole finding.** The
generalisation the repository draws is the split online feature stores make when
they assign each feature its own staleness budget instead of one global
freshness target.

### 19.4 Why amortized timing refuses to emit percentiles

**Evidence:** `[CODE]` `harness.hpp` header; `[GIT]` commit `24dc066` records
that the previous suite reported an SPSC push at 23 ns using a clock that cost
44 ns.

**Decision:** two explicit modes. Amortized is the only valid mode below ~3× the
noise floor and **by construction has no per-operation distribution**, so
`BenchmarkResult::has_distribution` stays false and CI asserts it.

**Trade-off:** no p99 for the cheapest operations, ever. The repository treats
that as the honest outcome and prints the median/minimum spread instead.

### 19.5 Why a boundary stamp chain instead of nested scopes

**Evidence:** `[TEST]` `test_a_chain_partitions_and_scopes_lose_time`.

**Decision:** N+1 stamps for N stages. Half the probe cost, and the stages
partition the interval **exactly**, so stage means sum to the end-to-end mean.

**Trade-off:** you cannot nest. Every stage must be a contiguous span.

### 19.6 Why the label comes from the future

**Evidence:** `[GIT]` commit `d2f1859`. The previous adapter labelled
`quantity > 10.0 || |dP|/P > 0.001` — a deterministic function of fields the
event carries, so the label was recoverable by arithmetic and no context
strategy could beat another.

**Decision:** toxic iff the price moves ≥ `threshold_bps` in the aggressor's
favour within `horizon_ms`.

**Trade-off:** the label uses the future and whole-sample statistics (the drift
mean). That is legitimate: **the constraint is that the PREDICTOR must not see
the future, not that the TARGET must avoid it.** `[CODE]` `walk_forward.hpp`
header states this explicitly.

### 19.7 Why the drift correction, and why the session mean stayed the default

**Evidence:** `[ARTIFACT]` the drift sweep in the walk-forward transcript.

**Decision:** subtract the session's mean forward return before thresholding.
Without it, BTCUSDT trended on 2024-01-15 and the **aggressor side alone reached
AUC 0.604** — leakage.

**Trade-off:** it does not fix everything. Three of 28 days still fail. A rolling
estimate was implemented, unit-tested on a rise-then-fall fixture, and swept: 1
min → 6 failing days, 5 min → 5, 30 min → 3, 2 h → 3, session mean → 3. **The
estimator works and the hypothesis is wrong.** Default unchanged; the three days
stand as a known defect.

### 19.8 Why splitmix64 instead of `<random>`

**Evidence:** `[CODE]` `bootstrap.hpp` header. `std::uniform_int_distribution`
is not specified to produce the same values across standard libraries, so a
result file produced here would not reproduce elsewhere.

**Trade-off:** hand-written RNG code to maintain. Lemire's bounded reduction is
used so the draw is unbiased rather than modulo-skewed.

### 19.9 Why E-Divisive rather than a percentage threshold

**Evidence:** `[CODE]` `change_point.hpp` header; `[ARTIFACT]`
`results/regression/sensitivity.txt`.

**Decision:** non-parametric change-point detection (Matteson & James 2014, as
deployed at MongoDB) plus a **second, effect-size bar**.

**Trade-off:** O(n²) per split scan × permutations — irrelevant at tens to
hundreds of points. The second bar costs sensitivity: a genuine 5% regression
passes. That is deliberate — a 2% injection is already *significant*, and a gate
that fires three times more often than the noise justifies gets switched off
within a week.

### 19.10 Why the QUBO keeps a penalty that never binds

**Evidence:** `[CODE]` `qubo.hpp`; `[TEST]` `test_penalty_never_binds_under_swap_moves`.

**Decision:** keep it, because it is what makes the matrix an unconstrained
binary quadratic form a QPU could accept unchanged. The annealer moves by
**swapping**, which preserves `|z| = K` exactly, so the penalty contributes the
same constant to every feasible point and cannot rank anything.

**Trade-off:** a term in the objective that does nothing at runtime. The
alternative — tuning the penalty weight until answers are feasible and then
reporting the tuning as a result — is the failure this avoids.

### 19.11 Why `--speed` and time-faithful replay

**Evidence:** `[GIT]` commit `a03caf9`. The first version replayed as fast as
the CPU allowed; the fast lane finished 5.2 h of tape in tens of milliseconds
and 97% of advisories were rejected as expired. **The rejection was correct —
the design working — and the run measured nothing.**

**Decision:** pace the fast lane to the trades' own timestamps divided by
`--speed`, and divide the slow lane's simulated latency by the same factor. What
is preserved is the **ratio** of model latency to inter-trade interval.

### 19.12 Why `titans_walkforward` has no threads

**Evidence:** `[CODE]` `walk_forward.hpp` header.

`titans_lanes` scores the same policy under a two-thread replay and its verdict
is `NOT RESOLVED`, because whether an advisory happens to be fresh when a toxic
trade arrives depends on when the slow lane last woke. **That is an honest
measurement of the ARCHITECTURE and a useless instrument for the POLICY.** So
the two questions get two tools, and the walk-forward number is explicitly an
**upper bound** on what the live lane can achieve.

### 19.13 Why JSON rather than YAML

**Evidence:** `[GIT]` commit `4fc4cd6`. The project has no YAML parser and never
did, while `config/engine.yaml` and `config/strategy.yaml` sat unread beside
hardcoded values. `core/json.hpp` is a parser the engine already links.

### 19.14 Decisions whose reason is not recorded

- **Why `kRing = 16`** rather than 8 or 32. The comment says "sized so a
  fast-path reader is never lapped in practice"; no measurement is cited.
  `[UNKNOWN]`
- **Why `--window 50`** for the flow policy, and `--quantile 0.95`. No sweep of
  either is committed. `[UNKNOWN]`
- **Why `n = 64, K = 16`** for the subset problem. `docs/SUBSET.md` lists "one
  budget" under threats to the numbers, so it is a known un-swept choice.
  `[DOC]`
- **Why physical core 4** is the benchmark default. The comment explains why not
  core 0; not why 4 specifically. `[UNKNOWN]`
- **Why the first commit is dated 2025-12-16 and the second 2026-07-18**, a
  seven-month gap. The repository contains no evidence about what happened in
  between. `[UNKNOWN]`

---

## 20. System invariants

Recovered from `static_assert`s, tests, guard clauses, scripts and CI. **Each
one, if violated, invalidates something published.**

### 20.1 Structural

| # | Invariant | Enforced by |
|---|---|---|
| I1 | `Advisory` is trivially copyable. | `static_assert` in `advisory.hpp` `[CODE]` |
| I2 | `LaneObservation` is trivially copyable. | `static_assert` in `lane_bridge.hpp` `[CODE]` |
| I3 | `AdvisorySlot::kRing` is a power of two. | `static_assert` `[CODE]` |
| I4 | A fast-lane advisory read is bounded at 4 attempts and never waits. | `[CODE]` `[TEST]` |
| I5 | The fast lane never allocates, blocks or syscalls on the measured path. | Convention + `LatencyBudget` violation counts. **Not statically enforced.** `[INFERRED]` |
| I6 | Every header under `include/titans/` compiles standalone. | CI `header-hygiene` `[CODE]` |
| I7 | No ODR violation across two TUs including the heavy headers. | CI `header-hygiene` `[CODE]` |

### 20.2 Measurement

| # | Invariant | Enforced by |
|---|---|---|
| I8 | A per-operation figure at or below `3 × noise_floor` is **not reported as a number**. | `TscClock::is_resolvable`, `LatencyFormatter::cell` `[TEST]` |
| I9 | An amortized result never carries a distribution. | `BenchmarkResult::has_distribution`; **CI asserts it** `[CODE]` |
| I10 | Every benchmark results file carries `machine.caveats`, `cpu_model` and `compiler`. | CI `benchmark-self-check` `[CODE]` |
| I11 | Stage means sum exactly to the end-to-end service mean. | `[TEST]` `test_stage_means_sum_to_the_service_mean` |
| I12 | The benchmark exits non-zero if its own calibration fails. | `[CODE]` `benchmark_main.cpp` |
| I13 | The probe cost is measured and printed in every tick-to-trade run. | `[CODE]` `[ARTIFACT]` |

### 20.3 Data and labelling

| # | Invariant | Enforced by |
|---|---|---|
| I14 | Trades must arrive in time order; out-of-order input is **rejected**. | `[TEST]` `test_unordered_input_is_rejected` |
| I15 | `is_buyer_maker == true` ⇒ the **seller** was the aggressor. | `[TEST]` `test_aggressor_side_convention` |
| I16 | Trades without a full horizon are **dropped (−1)**, never labelled benign. | `[TEST]` `test_trades_without_a_full_horizon_are_dropped` |
| I17 | Every downloaded archive is SHA256-verified; a mismatch is fatal. | `[CODE]` `fetch_binance.py` |
| I18 | A date range stops at the first missing day rather than leaving a hole. | `[CODE]` `fetch_binance.py` |
| I19 | No single event field may clear `|AUC − 0.5| > 0.10`; trailing flow must beat chance. | `titans_dataset` exit code `[CODE]` |

### 20.4 Protocol

| # | Invariant | Enforced by |
|---|---|---|
| I20 | The predictor never sees the future. The decision scoring trade *i* is taken from a window that closed before *i*. | `[TEST]` `test_the_decision_scoring_a_trade_never_saw_it` |
| I21 | Every walk-forward fold's training data ends **strictly before** its test day's first trade, checked on **timestamps**, not filenames. | `check_fold`, exit 3 `[TEST]` |
| I22 | Days are ordered by first trade timestamp, not argv order. | `[CODE]` `walkforward_main.cpp` |
| I23 | Below 5 observations, no bootstrap interval is reported at all. | `[TEST]` |
| I24 | A permutation p-value can never be exactly 0 (add-one estimator). | `[TEST]` |
| I25 | Below 12 points, the regression gate **refuses (exit 2)** rather than passing. | `[TEST]` |
| I26 | A degenerate policy exits 4 rather than presenting a structural zero as a null. | `[CODE]` `walkforward_main.cpp` |
| I27 | Offered advisory age is recorded **before** the expiry check. | `[TEST]` `test_offered_age_is_recorded_before_expiry` |
| I28 | Trial count is derived from what the tool did, not declared by the author. A single-configuration run records `trials = 1` and **says** no correction was applied. | `[CODE]` asserted in `reproduce.sh` |
| I29 | `run_policy_delivered` at delay 0 reproduces `run_policy_over_day` counter for counter. | `[TEST]` |

### 20.5 The three refusals CI protects

| # | Invariant | Enforced by |
|---|---|---|
| I30 | `titans_llm_experiment` exits non-zero with no backend, with the message "will not fall back to a stand-in". | CI greps both `[CODE]` |
| I31 | `generate_figures.py` fails rather than fabricating, with the message "no synthetic fallback". | CI greps both. **`reproduce.sh` checks only the exit code** — §29 D6. |
| I32 | `titans_engine` treats an unreadable `--config` as fatal. | `reproduce.sh` `[CODE]` |

### 20.6 Lane contract (asserted by `reproduce.sh`)

| # | Invariant |
|---|---|
| I33 | `--advisory decision` **must exit 5** (declares a 1000 ms horizon, delivers at p99 ~1530 ms ⇒ SLO breached). |
| I34 | `--advisory parameter` **must exit 0** (declares the 789,282 ms stretch it was estimated over ⇒ SLO met). |
| I35 | The regression gate must be quiet on the committed baseline, fire on a planted 25% (exit 6), and refuse a 6-run series (exit 2). |
| I36 | The freshness decay curve must be monotone enough to be a decay, **and zero delay must be the best point** — if a later delay wins, the delivery index is off by one and the whole curve measures the wrong trade. |
| I37 | Two of five tick-to-trade stages must print `under floor`; a build where every cell carries a figure has lost the rule, not gained precision. |
| I38 | The response tail must clear the host-jitter control. |
| I39 | Subset diversity must be monotone in λ, and selection must raise relevance above recency's by more than 2×. |

---

## 21. Dangerous operations

### 21.1 `scripts/reproduce.sh --with-data` overwrites a tracked artifact

> **WARNING.** Stage "Freshness decay" writes
> `results/context/decay_${DATA_DATE}.json`, which is **a committed file**
> (`results/context/decay_2024-01-15.json`). `[RUN]` It happens to be
> deterministic and model-free, so it usually rewrites byte-identically — but
> change `--max-rows`, `--delays`, or the policy, and you have silently replaced
> a published artifact.

### 21.2 `scripts/reproduce.sh --with-walkforward` overwrites a tracked artifact

> **WARNING.** It writes
> `results/walkforward/${SYMBOL}_$(echo "$WF_RANGE" | tr ':' '_').json`, which
> resolves to the committed
> `results/walkforward/BTCUSDT_2024-01-08_2024-02-04.json`. `[CODE]` Run
> `git status` afterwards.

### 21.3 `scripts/reproduce.sh` used to create a benchmark JSON in `results/` — **fixed**

> **Historical.** Stage 3 wrote `results/benchmark_$(hostname)_$(date +%Y%m%d).json`
> and **destroyed one published artifact doing it**: commit `f5e59b4`, which
> introduced `reproduce.sh`, overwrote
> `results/benchmark_GW-X570-Taichi_20260830.json` — the exact file the README's
> nine-row latency table was taken from. `[GIT]` See §29 D1.
>
> Since 2026-09-09 the stage writes to
> `/tmp/titans_benchmark_$(hostname)_$(date +%Y%m%d).json` and cannot reach
> `results/`. The recovered artifact lives at
> `results/benchmark_GW-X570-Taichi_20260830_readme.json`, a name no run of the
> script can generate, and `scripts/check_readme_table.py` now asserts the README
> still matches it.
>
> **Leftovers:** `results/benchmark_GW-X570-Taichi_20260907.json` and
> `..._20260908.json` remain untracked in the working tree. Nothing generates them
> any more; they can be deleted. Two of the four dated files are tracked and two
> are not, still with no stated rule.

### 21.4 Paths that were deliberately redirected to `/tmp` — do not "fix" them

Three `reproduce.sh` stages write to `/tmp` **on purpose**, each with a comment
explaining why. Redirecting them back into `results/` would replace a published
artifact with a differently-parameterised smoke run:

| Stage | Writes to | Because the committed artifact is |
|---|---|---|
| Tick-to-trade | `/tmp/titans_t2t.json` | a 400k-row run **with** the saturation sweep; this stage is 100k rows |
| Subset selection | `/tmp/titans_subset.json` | the **28-day** run; this stage is one day |
| Embargo sweep | `/tmp/titans_wf_sweep.json` | the committed 5-point sweep |

Both the tick-to-trade and subset redirections were added **after** the
corresponding artifact had already been clobbered once. `[GIT]` `[CODE]`

### 21.5 `scripts/bench_series.sh` appends

> **WARNING.** It does **not** clear the output directory. Running it twice into
> `results/benchmark_history/local` silently merges two series, and
> `titans_regression` will read the join as a level shift. `reproduce.sh`
> `rm -rf`s the `reproduce/` directory before calling it; the `local/` baseline
> has no such guard.

### 21.6 `results/benchmark_history/local/` is the regression gate's ground truth

> **WARNING.** Forty runs, committed, and **both `reproduce.sh` and CI assert
> pass/fail against it.** Regenerating it on different hardware changes what the
> gate considers normal and what "a planted 25% regression" looks like. Treat it
> as data, not as output.

### 21.7 Operations that call a paid or heavyweight service

- **None are paid.** `data.binance.vision` is free and unauthenticated; the model
  backends are local.
- `titans_llm_experiment --events 1000` issues **1000 model calls per arm × 6
  arms**. On the CPU shim at single-digit tokens/sec that is hours.
- `titans_context_cost --contexts 8,32,128,512,1024 --points 400` issues **2000
  model calls**, the largest at 5321 prompt tokens.
- `python/data/fetch_binance.py --dates 2024-01-08:2024-02-04` downloads
  **~2.8 GB**.
- `scripts/reproduce.sh --with-walkforward` reads all 2.8 GB and takes ~8 min.

### 21.8 Operations that consume a GPU

`vllm serve` and `ollama serve` take VRAM. `python/serving/cpu_shim.py` exists
specifically because *"a shared workstation whose GPUs are fully committed to
other jobs, where grabbing memory would OOM someone else's run"* is an expected
situation. `[CODE]`

### 21.9 Things that are safe

- `./build/tests/titans_tests` — writes nothing.
- `./build/titans_benchmark` without `--json` — writes nothing.
- Every tool without `--json` — stdout only.
- `scripts/inject_regression.py` — clears only its own destination.
- `scripts/reproduce.sh` with **no** flags — 6 stages, and since the D1 fix it
  writes nothing outside `/tmp`.

### 21.10 No destructive operation exists

There is no script that deletes data, no database migration, no deployment, no
`rm -rf` outside `/tmp` and `results/benchmark_history/reproduce/`, and no
outbound network write of any kind. `[RUN]`

---

## 22. Cost model

### 22.1 Money

**Zero.** No paid API, no cloud, no managed service. The Binance archive is free
and unauthenticated. The models are local. `[RUN]`

`docs/ROADMAP.md` mentions "$9.83 of QPU time" — that is a figure quoted **from
the source paper** under "Explicitly not doing", not a cost this project has
incurred. `[DOC]`

### 22.2 Storage

| Item | Size |
|---|---|
| 28 days of BTCUSDT `aggTrades` CSV | **2.8 GB** `[RUN]` |
| One day | ~100 MB |
| `results/` (tracked) | small; the largest single file is the 843-line subset transcript |
| `data/logs/` | 32 KB of stubs |
| Build tree | a few hundred MB per configuration |

### 22.3 Wall-clock

| Operation | Cost | Source |
|---|---|---|
| Full build, 32 threads | ~1–2 min | `[INFERRED]` |
| `titans_tests` | seconds | `[RUN]` |
| `titans_benchmark` | ~1 min (15 reps × 9 metrics + 200 ms calibration) | `[INFERRED]` |
| `bench_series.sh <dir> 20` | ~20 min | `[INFERRED]` from 20 × ~1 min |
| `titans_walkforward` over 28 days | part of the ~8 min `--with-walkforward` stage | `[DOC]` |
| `titans_ticktotrade --rows 400000 --speed 2000` | 658.9 min of tape ÷ 2000 ≈ 20 s, plus the probe A/B and the sweep | `[ARTIFACT]` |
| `titans_subset` over 28 days | minutes; Part A alone is 200 windows × 5 λ × 23 ms exhaustive ≈ 23 s per day | `[ARTIFACT]` |
| `ctest` **with GTest installed** | ~11 min (234 entries × 2.9 s), all of them the same suite | `[RUN]` — see §29 D9 |

### 22.4 Model inference

| Arm | Prompt tokens | Prefill | Total latency |
|---|---:|---:|---:|
| 8 trades | 241 | 30 ms | 413 ms |
| 32 | 361 | 83 ms | 516 ms |
| 128 | 841 | 250 ms | 708 ms |
| 512 | 2761 | 963 ms | 1608 ms |
| 1024 | 5321 | 1988 ms | 2887 ms |

`[ARTIFACT]` `results/context/context_cost_llama31_8b.json` — llama3.1:8b on an
RTX 3090 via Ollama.

Fitted: **201 tokens of fixed overhead + 5.00 tokens per trade**, and **0.385 ms
of prefill per token** ≈ 1.9 ms per trade of context.

A 5-arm × 400-point run is 2000 calls; at these latencies that is roughly
**20–25 minutes of pure inference**, plus Part A's decay sweep.

### 22.5 The cost that actually matters in this project

Not dollars — **freshness**. The repository's own exchange rate:

| Delay | Informedness remaining |
|---|---|
| 0 ms | +0.0759 (100%) |
| 250 ms | +0.0195 (26%) |
| 1000 ms | +0.0086 (11%) |
| 4000 ms | +0.0009 (1%) |

`[ARTIFACT]` So the fixed 383 ms of non-prefill model latency costs more than
half the signal **before context buys anything**, and the exhaustive QUBO
solver's 23 ms costs about a fifth. This is the unit every P4 and P5 cost claim
is denominated in.

---

## 23. Current state of the project

### 23.1 Stable — reliable, tested, and depended on

| Component | Evidence |
|---|---|
| `core/spsc_queue.hpp`, `core/memory_pool.hpp`, `core/event_bus.hpp`, `core/json.hpp` | Benchmarked and unit-tested. |
| `lanes/advisory.hpp`, `lane_bridge.hpp`, `freshness.hpp`, `flow_policy.hpp`, `latency_budget.hpp` | The isolation and freshness contracts are tested; TSan and ASan clean. |
| `bench/cycle_timer.hpp`, `harness.hpp`, `platform.hpp`, `histogram.hpp`, `stage_trace.hpp` | The self-check gates every run; CI asserts the fingerprint. |
| `eval/*` (all six headers) | Six dedicated test modules; the statistical properties are asserted, not assumed. |
| `opt/qubo.hpp` | Every solver cross-checked against exhaustive search. |
| `context/binance_dataset.hpp` | Six tests; the aggressor convention and ordering guard are asserted. |
| `python/data/fetch_binance.py` | Checksum-verified; used for every data claim. |
| `scripts/reproduce.sh`, `bench_series.sh`, `inject_regression.py` | 11 stages pass; three asserted in CI too. |
| `trading/order_book.hpp` (L2 path), `trading/risk_manager.hpp` (`check_order`) | Real stages in `titans_ticktotrade`; benchmarked. |

### 23.2 Active — the current line of work

Nothing is mid-flight. **The last commit closed the last roadmap item.** `[GIT]`
The nine tools under `src/tools/` are the active surface; the six roadmap
documents (`FRESHNESS`, `LATENCY`, `REGRESSION`, `SELECTION`, `CONTEXT_COST`,
`SUBSET`) are the current record.

### 23.3 Experimental — implemented, not validated

| Component | Why it is not validated |
|---|---|
| `context/context_contaminator.hpp` and the eight context strategies | Unit-tested for mechanics; **the experiment they exist for has never produced an interpretable result** because both models tested were degenerate. |
| `context/distributed_experiment.hpp` | 755 lines of multi-process sharding, no committed artifact. |
| `core/gpu_monitor.hpp` | Works by shelling out; nothing depends on its numbers. |
| `cuda/*` | Not built in the checked-in configuration; explicitly outside the measurement rewrite. |
| `python/serving/cpu_shim.py` | Documented as CI's path; its dependencies are absent on the reference host, so it was **not exercised in this audit**. |

### 23.4 Blocked

| Item | Blocker |
|---|---|
| **The LLM contamination result** | Needs a model more capable than this hardware can host. Three prompt revisions changed the reasoning text on 45 of 60 trials and not one classification; a fourth is explicitly ruled out. |
| **Gating the six bimodal benchmark metrics** | Needs `perf` counters and a controlled allocator to separate alignment / page colouring / core placement. "Until then six of nine metrics are ungated." `[DOC]` |
| **A real cross-commit benchmark series** | CI uploads one run per commit as an artifact; **nothing accumulates them**. Described as "a storage decision, not a statistics one". `[DOC]` |
| **Explaining the three leaky days** | The intraday-trend hypothesis is tested and dead. Nothing else has been proposed. |
| **Live market connectivity** | No TLS in `websocket_client.hpp`. |

### 23.5 Deprecated / dormant

| Component | Status |
|---|---|
| `context/data_adapters.hpp` | **legacy.** Holds the leaky labeller `d2f1859` replaced. Included by nothing. |
| `core/debug.hpp` | **legacy.** Included by nothing; `docs/DEBUGGING_GUIDE.md` documents it as live. |
| `market_data/websocket_client.hpp` | dormant; cannot reach the intended endpoint. |
| `market_data/binary_logger.hpp`, `replay_engine`, `titans_replay` | dormant; the only `.bin` files are 128-byte stubs. |
| `trading/matching_engine.hpp`, `shadow_engine.hpp`, `strategy/strategy_base.hpp` | dormant; used only by `titans_engine`. |
| `src/main.cpp` / `titans_engine` | demo only; feeds itself `rand()`. |
| `python/visualizer/app.py`, `analytics/llm_analyzer.py`, `research/gpu_analysis.py` | dormant. |
| `docs/API_REFERENCE.md` | **stale.** Covers none of `lanes/`, `eval/`, `bench/`, `opt/`. |
| `origin/HEAD → claude/hybrid-trading-system-dVT8H` | stale pointer; 9 commits behind `main`. |
| `build-asan/`, `build-tsan/` | stale local build trees from 2026-08-30. |

### 23.6 Unknown

- Whether `python/research/generate_figures.py` produces sensible output from
  the current `results/` tree. Its loader `rglob`s every `*.json` under
  `results/`, which now holds six unrelated schemas. **Not runnable on the
  reference host** (no `seaborn`). `[UNKNOWN]`
- Whether the Docker images build today. Not attempted. `[UNKNOWN]`
- Whether `titans_distributed_experiment` and `titans_config_experiment` still
  work end to end. They compile; no artifact exists. `[UNKNOWN]`
- Whether `data.binance.vision` still serves 2024 **daily** files. The fetch
  script's own docstring warns daily files exist "only for roughly the last few
  months". If they have aged out, `--with-data` and `--with-walkforward` cannot
  run from scratch and the 28 `.manifest` files become the only record.
  **This is the single largest reproducibility risk in the project.** `[CODE]`
  `[INFERRED]`

---

## 24. Technical debt

Ordered by how much damage each could do.

### TD-1. A published artifact was overwritten by the reproduction script — **RESOLVED 2026-09-09**

The README's table matched `results/benchmark_GW-X570-Taichi_20260830.json` as
it existed at `24dc066`; `f5e59b4`, which introduced `scripts/reproduce.sh`,
overwrote it at the same date-derived path. Recovered, prevented and now
checked. Full account in §29 D1.

### TD-2. A document contradicted its own tool and its own artifact — **RESOLVED 2026-09-09**

`docs/SUBSET.md` claimed the warm-start verdict judged against its own scale;
the tool used a `1e-9` absolute epsilon on energies near −1023.7 and announced a
direction with an invented mechanism. Now a paired bootstrap interval that
prints `NOT RESOLVED`. Full account in §29 D2.

### TD-3. The tick-to-trade artifact's provenance block is half empty — **MEDIUM**

`build_flags: "unrecorded"`, `pinned_cpu: -1`, `tsc_ghz: 0`,
`noise_floor_ns: 0`, while the same run prints the TSC frequency to stdout.
`TITANS_BUILD_FLAGS` is defined only for `titans_benchmark`. §29 D7.

**Risk:** a latency artifact that cannot say what compiler flags produced it, in
a project that added `caveats` blocks precisely for this.

### TD-4. `gtest_discover_tests` on a non-GTest binary — **MEDIUM**

Where GoogleTest is installed, CMake links `GTest::gtest_main` (whose `main` is
shadowed by `test_main.cpp`'s) and runs `gtest_discover_tests`, which parses the
suite's own stdout — including the ASCII banner — as test names. Result: **234
ctest entries, each re-running the entire suite, ~11 minutes.** `[RUN]` §29 D9.

**Risk:** `ctest` is what the README and CI tell you to run. CI happens to be
unaffected because it does not install GoogleTest.

### TD-5. `reproduce.sh`'s figure-generation gate passes for the wrong reason — **MEDIUM, still open**

It checks only `exit != 0`. On a host without `seaborn` the script dies at
import and the gate goes green without ever reaching the refusal. CI greps the
message; `reproduce.sh` does not. §29 D6. **Not fixed** — it is the same class
as D1 and D2 (an assertion that does not assert what it names), and the obvious
repair is one `grep -q "no synthetic fallback"`.

### TD-6. Two `reproduce.sh` stages still overwrite tracked artifacts — **MEDIUM, still open**

`results/context/decay_2024-01-15.json` and
`results/walkforward/BTCUSDT_2024-01-08_2024-02-04.json`. **Four** other stages
are now redirected to `/tmp` for exactly this reason — three of them after the
artifact had already been clobbered, and the fourth as part of the D1 fix. These
two remain. §21.1–21.2.

### TD-7. Docs drift from the code — **MEDIUM**

| Doc | Drift |
|---|---|
| `docs/API_REFERENCE.md` | No coverage of `lanes/`, `eval/`, `bench/`, `opt/` — the current active path. |
| `docs/ARCHITECTURE.md` | Component map says "tests/: 15 modules"; there are 20. |
| `docs/DEBUGGING_GUIDE.md` | Documents `core/debug.hpp`, which nothing includes. |
| `docs/RESEARCH_FRAMEWORK.md` | Describes an `experiments/` output tree that does not exist. |
| `scripts/reproduce.sh` | Stage header says "README: 12 modules"; README and reality say 20. |

### TD-8. No atomic writes anywhere — **LOW, until it isn't**

Every `--json` write is a plain truncate-and-write. Two concurrent runs to the
same path interleave. `bench_series.sh` names files to second granularity, so
two series started in the same second collide.

### TD-9. `bench_series.sh` appends into an existing directory — **LOW**

Silently merges two series. `titans_regression` reads the join as a level shift.

### TD-10. Duplicated `auc()` — **LOW**

`src/tools/dataset_main.cpp:~46` has a local `auc()` alongside
`include/titans/eval/metrics.hpp:auc()`. `metrics.hpp`'s header comment says the
duplication was the reason it was extracted — *"a second implementation of a
ranking metric is exactly the kind of duplication that lets two tools quietly
disagree about whether a day's labels are clean"* — but the copy in
`dataset_main.cpp` was never removed. `[RUN]`

### TD-11. `.cu` files that contain nothing — **LOW**

Four 19–21 line stubs whose only purpose is to give CMake translation units.
All kernel code is in `.cuh` headers.

### TD-12. Thirteen-line `.cpp` anchors — **LOW, intentional**

Fifteen files across `core/`, `trading/`, `market_data/`, `strategy/` exist only
so `add_library(... STATIC ...)` has something to compile. Documented in-file.

### TD-13. Three `TODO`s in `risk_manager.hpp` — **LOW**

Lines 272, 273, 450: cancel all open orders on a breach, publish an alert event,
publish a position-update event. **A risk manager that detects a breach and does
not cancel is a real gap** — but `titans_engine` is a demo, and `check_order` is
used in `titans_ticktotrade` only as a timed stage.

### TD-14. `Debug` build type is a sanitizer build — **LOW, surprising**

`CMAKE_CXX_FLAGS_DEBUG = "-g -O0 -DDEBUG -fsanitize=address,undefined"`. CI's
`build-and-test` matrix includes `Debug`, so it runs the suite under ASan/UBSan
twice (once here, once in the dedicated job). Anyone expecting a plain debug
build gets a slow one.

### TD-15. Orphaned dependency declarations — **LOW**

`python/requirements.txt` lists `pytest`, `black`, `mypy`; there is no test
suite, no formatter config and no type-checker config for Python. It also lists
`streamlit` and `plotly` for a dormant dashboard.

### TD-16. Manual result capture — **LOW**

The `.txt` artifacts under `results/` are hand-committed stdout. Nothing checks
that a `.txt` and its neighbouring `.json` came from the same run. The
walk-forward transcript even names a different output filename
(`btcusdt_...json`, lowercase) from the committed one
(`BTCUSDT_...json`). `[RUN]`

---

## 25. If you are taking over this project

### Day 1 — understand the thesis, not the code

**Read, in this order:**

1. `README.md` §"Why a boundary, and where it sits" and §"Measured performance"
   — 20 minutes. Note that the README is 920 lines and is closer to a paper than
   a readme.
2. `docs/ARCHITECTURE.md` — why each mechanism is shaped the way it is.
3. `docs/FRESHNESS.md` — **the single most important document.** It contains the
   finding that reframed the project (ship the parameter, not the decision) and
   three dead hypotheses kept on purpose.
4. `docs/ROADMAP.md` — P0–P5, each with "the original text follows, unedited",
   so you can see what was predicted next to what was measured.

**Run:**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DTITANS_ENABLE_CUDA=OFF
cmake --build build -j"$(nproc)"
./build/tests/titans_tests      # 20 modules
./build/titans_benchmark        # read SECTION 0 carefully
```

**Read one artifact end to end:** `results/latency/ticktotrade_2024-01-15.txt`.
It is the clearest single example of the house style — caveats first, refusals
inline, and an explanation of why the percentile columns do not add.

### First week — the modules that matter

In dependency order:

| Day | Read | Why |
|---|---|---|
| 2 | `include/titans/lanes/*` (all five, ~1000 lines) | This is the architecture. `advisory.hpp`'s header comment is the design document. |
| 3 | `include/titans/bench/cycle_timer.hpp` + `harness.hpp` + `stage_trace.hpp` | Every latency claim rests on `is_resolvable`. |
| 4 | `include/titans/eval/*` | Every outcome claim rests on these. Start with `metrics.hpp`, then `bootstrap.hpp`, then `walk_forward.hpp`. |
| 5 | `include/titans/context/binance_dataset.hpp` | The label. Understand why truncation is unsound before you ever pass `--max-rows`. |
| 6 | `tests/test_lane_isolation.cpp`, `test_freshness.cpp`, `test_walk_forward.cpp` | The tests encode findings. |
| 7 | `scripts/reproduce.sh` | The clearest statement of what the project believes it has proved, and the three things it insists must fail. |

**Do not start with** `src/main.cpp`, `strategy/`, `matching_engine.hpp` or
`market_data/`. They are the pre-pivot layer and will mislead you about what the
project is.

### Before modifying core logic

| If you touch | Verify first |
|---|---|
| `AdvisorySlot` | `test_advisory_slot_never_tears` still runs an adversarial tight-loop writer and asserts **zero** torn reads. `.github/tsan.supp` is only justified while it does. |
| `AdvisoryView::current` | The offered-age recording is still **before** the expiry check. Moving it makes the SLO tautological. |
| `FlowPolicy` | Both `titans_lanes` and `titans_walkforward` use it. Any change invalidates **every** policy number in the repository. |
| `TscClock` or `is_resolvable` | Every "under floor" refusal and every published latency figure. |
| `StageTrace` | `test_stage_means_sum_to_the_service_mean` and `test_a_chain_partitions_and_scopes_lose_time`. |
| `binance_dataset.hpp` | The label. **Every outcome number in the project.** |
| `eval/bootstrap.hpp` | `test_bootstrap_covers_at_the_rate_it_claims` (300 trials, 85–99% band). |
| `change_point.hpp::assess_modality` | Both `test_a_step_is_not_mistaken_for_two_modes` and `test_interleaved_modes_are_not_gated`. Getting this wrong made the gate silently useless once. |

### Before running an expensive experiment

1. `./build/titans_benchmark` exits 0 — otherwise timing numbers are noise.
2. `git status` is clean — so you can tell what the run changed.
3. Two physical cores are free if the tool pins two.
4. `--max-rows` is **unset** for anything that audits labels.
5. You know how many configurations you are about to try, and you are going to
   pass `--trials`. Best-of-20 on pure noise is called significant 65.2% of the
   time.
6. You have **more than one day** of data if you intend to claim an outcome.
   This is the lesson P5 paid for.

### Before publishing or deploying

```bash
scripts/reproduce.sh --with-data          # expect: All 11 stages reproduced.
scripts/reproduce.sh --with-walkforward   # if you touched anything evaluative
git status                                # expect: clean, or you overwrote an artifact
```

Then check, by hand, that the number you are about to publish appears in a
committed artifact under `results/`. **The one thing this project has never
automated is checking its own prose against its own artifacts, and that is
exactly where its two worst defects are** (§29 D1, D2).

### The cultural rule to preserve

Read the roadmap entries. **Four of six items found their own prescription
wrong**, and every one of those is written down next to the original text
unedited. Three README results are refusals. The most-cited failure in the
repository is a doc that was nearly published from one day of data and was
caught by an assertion.

If you make this codebase more optimistic, you have broken it.

---

## 26. Change impact map

### If you change `FlowPolicy` (window, threshold semantics, `decide()`)

**Affected:** `titans_lanes`, `titans_walkforward`, `titans_ticktotrade` (signal
stage), `titans_context_cost` (Part A reference curve), `titans_subset`
(baseline comparison).

**Must rerun:** `tests/test_walk_forward.cpp`, `test_freshness.cpp`,
`test_delivered.cpp`; `reproduce.sh --with-data --with-walkforward`.

**Artifacts invalidated:** `results/lanes/*`, `results/walkforward/*`,
`results/context/*`, `results/subset/*`, and the tick-to-trade signal-stage
figures. **Effectively every outcome number in the project.**

### If you change the label (`binance_dataset.hpp`, `--horizon-ms`, `--threshold-bps`, drift)

**Affected:** everything that scores anything.

**Must rerun:** `test_binance_dataset`, `test_task_design`, then every
data-dependent stage.

**Artifacts invalidated:** all of `results/lanes/`, `results/walkforward/`,
`results/context/`, `results/subset/`, plus the leakage-audit numbers in the
README.

**Also:** the three-days-leak finding and the drift sweep both become stale.

### If you change `AdvisorySlot` or `AdvisoryView`

**Affected:** `titans_lanes`, `titans_ticktotrade` signal stage.

**Must rerun:** `test_lane_isolation` (**including under TSan**),
`test_freshness`; the two `--advisory` exit-code assertions.

**Artifacts invalidated:** `results/lanes/*`. The `.github/tsan.supp`
justification must be re-checked.

### If you change `TscClock`, `Histogram`, or `StageTrace`

**Affected:** `titans_benchmark`, `titans_ticktotrade`, `titans_regression`
(indirectly — the series is benchmark output), `FreshnessMonitor`.

**Must rerun:** `test_pipeline_latency`, `test_freshness`; the benchmark
self-check; the `under floor` and host-floor assertions.

**Artifacts invalidated:** `results/benchmark_*.json`, **the whole 40-run
baseline** (which is the regression gate's ground truth — see §21.6), and
`results/latency/*`.

### If you change `eval/bootstrap.hpp` or the seed

**Affected:** every confidence interval and permutation p-value in the project.

**Must rerun:** `test_walk_forward` (coverage + reproducibility), `test_deflated`.

**Artifacts invalidated:** every CI and p-value in `results/walkforward/`,
`results/context/`, `results/subset/`. Note the RNG is hand-written **precisely
so results reproduce across standard libraries** — changing it breaks that
guarantee.

### If you change `change_point.hpp`

**Affected:** `titans_regression` only.

**Must rerun:** `test_change_point` (all 9 cases), and the three CI gate
assertions.

**Artifacts invalidated:** `results/regression/*`. **Re-derive
`sensitivity.txt`** — the 6% firing threshold is a property of the gate, not of
the data.

### If you change `opt/qubo.hpp`

**Affected:** `titans_subset` only.

**Must rerun:** `test_qubo` (all 10), including
`test_matrix_reproduces_the_written_objective` on infeasible assignments.

**Artifacts invalidated:** `results/subset/*`, and `docs/SUBSET.md`'s solver
agreement table.

### If you change `scripts/reproduce.sh`

**Affected:** the project's only end-to-end check.

**Watch for:** any stage you point back into `results/` will overwrite a
committed artifact. Three stages are in `/tmp` deliberately (§21.4).

### If you change a header's includes

**Affected:** CI `header-hygiene` — every header must compile standalone, and no
ODR violation may appear across two TUs.

### If you regenerate `results/benchmark_history/local/`

**Affected:** the regression gate's pass/fail assertions in both `reproduce.sh`
and CI. **This is data, not output.** See §21.6.

### If you add a new tool under `src/tools/`

**Must also update:** `CMakeLists.txt`, the README's target table, the
Dockerfile's runtime `COPY` list (currently already 5 tools behind), and
`scripts/reproduce.sh` if it produces a claim.

---

## 27. Result provenance

For each headline claim: the chain from prose to code. **Breaks are marked.**

### 27.1 "Selection beats recency: +0.0576 [+0.0323, +0.0827], 21/28 days"

```
README §"A QUBO whose sophisticated half does nothing"  +  docs/SUBSET.md
        v
results/subset/subset_28days.txt  (843 lines, ACROSS-28-DAYS block)   [ARTIFACT]
results/subset/subset_28days.json (schema titans.subset.v2)
        v
eval::bootstrap_mean_ci over 28 per-day differences
eval::sign_flip_test  -> p = 0.00045
eval::deflate(trials = 11) -> bar +0.0209, z +2.85, survives
        v
src/tools/subset_main.cpp  (PART C + ACROSS DAYS)
include/titans/opt/qubo.hpp  (solve_greedy at lambda = 0)
        v
data/raw/BTCUSDT-aggTrades-2024-01-{08..31},2024-02-{01..04}.csv
        v
28 x .manifest  (source_url + archive_sha256 + rows)   [tracked]
        v
commit 7aea82f
```

**COMPLETE.** The numbers in the doc match the artifact line for line.

### 27.2 "Out-of-sample informedness +0.1676 [+0.1385, +0.1966] across 27 folds"

```
README §"Out-of-sample: 28 days"  +  docs/SELECTION.md
        v
results/walkforward/run_2024-01-08_2024-02-04.txt         [ARTIFACT]
results/walkforward/BTCUSDT_2024-01-08_2024-02-04.json    (titans.walkforward.v1)
        v
eval::bootstrap_mean_ci / sign_flip_test / deflate
        v
src/tools/walkforward_main.cpp -> eval::run_policy_over_day
        v
28 CSVs + manifests
        v
commit ab95c55 (tool) + 6c832e2 (embargo, trials, deflation)
```

**COMPLETE**, with one cosmetic wrinkle: the `.txt` transcript ends "Wrote
results/walkforward/**btcusdt**_2024-01-08_2024-02-04.json" (lowercase) while
the committed JSON is uppercase. The two agree on every number; the transcript
predates a filename change. `[RUN]`

### 27.3 "Tick to trade: service p99 1100.9 ns, response p99 78.9 µs, 72×"

```
README §"Tick to trade"  +  docs/LATENCY.md
        v
results/latency/ticktotrade_2024-01-15.{txt,json}   (titans.ticktotrade.v1)
        v
bench::PipelineHistograms  <- bench::StageTrace<true>
        v
src/tools/pipeline_main.cpp  --rows 400000 --speed 2000 --sweep
        v
data/raw/BTCUSDT-aggTrades-2024-01-15.csv + manifest
        v
commit c91abd1
```

**COMPLETE for the numbers. PARTIAL for the environment:** the artifact's
`machine` block has `build_flags: "unrecorded"`, `pinned_cpu: -1`,
`tsc_ghz: 0`, `noise_floor_ns: 0`. The `.txt` transcript recovers the TSC
frequency and floor; the compiler flags are lost. §29 D7.

### 27.4 "Decision 40.1% agreement / parameter 89.8%"

```
README §"Ship the parameter"  +  docs/FRESHNESS.md  +  docs/ROADMAP.md P0
        v
results/lanes/advisory_decision_2024-01-15.txt      [ARTIFACT]
results/lanes/advisory_parameter_2024-01-15.txt     [ARTIFACT]
        v
src/tools/lanes_demo_main.cpp --advisory {decision,parameter}
include/titans/lanes/advisory.hpp (Advisory::parameter)
        v
data/raw/BTCUSDT-aggTrades-2024-01-15.csv + manifest
        v
commit 79aa674
```

**COMPLETE**, and additionally **asserted**: both exit codes (5 and 0) are
re-checked on every `reproduce.sh --with-data` run.

### 27.5 "Half the value is gone by 250 ms" and the context sweep

```
README §"What a richer prompt costs"  +  docs/CONTEXT_COST.md
        v
results/context/context_cost_llama31_8b.{txt,json}  (titans.context_cost.v1)
results/context/decay_2024-01-15.json               (Part A only)
        v
eval::run_policy_delivered / eval::check_context_growth / paired bootstrap
        v
src/tools/context_cost_main.cpp
include/titans/context/ollama_backend.hpp -> core/http_client.hpp -> Ollama
        v
data/raw/BTCUSDT-aggTrades-2024-01-09.csv + manifest
        v
commit 2adf1d4
```

**COMPLETE for Part A** (deterministic, re-derived on every `--with-data` run
and asserted monotone with zero delay best).

**PARTIAL for Part B.** The artifact is one 400-point run; the doc's
four-run stability table (2 × 200 points, 2 × 400 points) cites three runs whose
artifacts are **not committed**. `[ARTIFACT]` shows one; `[DOC]` shows four. The
claim "the 512 arm is resolved negative in all four runs" therefore rests on the
document alone. **This is the largest provenance gap among the roadmap results.**

### 27.6 "6% is where the gate fires"

```
README §"Is it slower than last week?"  +  docs/REGRESSION.md
        v
results/regression/sensitivity.txt        [ARTIFACT]
results/regression/gate_injected_20pct.txt
results/regression/gate_local_40.{txt,json}  (titans.regression.v1)
        v
eval::evaluate_gate  <- eval::find_change_points + robust_sigma + assess_modality
        v
src/tools/regression_main.cpp  <-  scripts/inject_regression.py
        v
results/benchmark_history/local/  (40 committed runs)   [ARTIFACT, tracked]
        v
commit 16c2878
```

**COMPLETE**, and unusually so: the *input* series is committed, not just the
output. This is the only experiment in the project whose raw input is in Git.

### 27.7 "SPSCQueue::try_push 1.21 ns (0.99)" and the other eight rows — **REPAIRED 2026-09-09**

```
README §"Measured performance"   (now names its source file inline)
        v
results/benchmark_GW-X570-Taichi_20260830_readme.json      [ARTIFACT, tracked]
        |   byte-identical to 24dc066:results/benchmark_GW-X570-Taichi_20260830.json
        |   try_push 1.21 (0.99) 826 M ... all nine rows, throughput included
        v
scripts/check_readme_table.py   asserts the table still matches, row by row,
        |                       on cost, minimum AND throughput
        v
   run as a scripts/reproduce.sh stage and as a CI step in benchmark-self-check
```

**The chain was broken between 2026-08-30 and 2026-09-09.** The table was taken
from `results/benchmark_GW-X570-Taichi_20260830.json` at commit `24dc066`;
commit `f5e59b4`, which added `scripts/reproduce.sh`, wrote its own run to
`results/benchmark_$(hostname)_$(date +%Y%m%d).json` — the same path on the same
day — and overwrote it. **The script written to re-derive every number in the
README destroyed the evidence for one of them.** `[GIT]`

The other committed runs still differ, as they should: 20260830 → 1.718,
20260906 → 1.524, 20260907 → 0.979, 20260908 → 0.989, 40-run median → 1.458.
That spread is the README's own point about the second and third significant
digit; the table is one draw from it, and now says which.

### 27.8 "0 torn in 2,564,325 reads" and "fast-lane p99 unchanged under a hung slow lane"

```
README §"Why a boundary"  +  docs/ARCHITECTURE.md  +  .github/tsan.supp
        v
   (no artifact file)
        v
tests/test_lane_isolation.cpp, printed to stdout on every test run
```

**LIVE rather than archived.** These numbers have no `results/` file; they are
re-derived every time the suite runs and are re-printed by `reproduce.sh` stage
2. That is arguably stronger than an artifact — but the *specific* figures
quoted in the README are from one run and will vary. `[RUN]`

### 27.9 The two live-model contamination runs

```
README §"What the live-model runs actually produced"
        v
results/llm/llm_Qwen_Qwen2.5-1.5B-Instruct_seed42_n50.json    [ARTIFACT]
results/llm/llm_Qwen_Qwen2.5-3B-Instruct_seed42_n400.json     [ARTIFACT]
        (raw model responses included in both)
        v
examples/run_llm_experiment.cpp
        v
commits 9573612 (degeneracy gate), c3b48b5 (naming), 25be676 (prompt revisions)
```

**COMPLETE**, and the strongest provenance in the project: the raw responses are
in the artifact, so the diagnosis ("the model compared against records belonging
to other entities") is checkable by a reader.

### 27.10 Provenance summary

| Claim | Doc | Artifact | Code | Data | Commit | Verdict |
|---|---|---|---|---|---|---|
| Subset +0.0576 | ✅ | ✅ | ✅ | ✅ | ✅ | complete |
| Walk-forward +0.1676 | ✅ | ✅ | ✅ | ✅ | ✅ | complete |
| Tick-to-trade | ✅ | ✅ | ✅ | ✅ | ✅ | complete; env block partial |
| Decision vs parameter | ✅ | ✅ | ✅ | ✅ | ✅ | complete + asserted |
| Freshness decay (P4-A) | ✅ | ✅ | ✅ | ✅ | ✅ | complete + asserted |
| Context sweep (P4-B) | ✅ | ⚠️ 1 of 4 runs | ✅ | ✅ | ✅ | **partial** |
| Regression gate | ✅ | ✅ + input | ✅ | ✅ | ✅ | complete |
| Fast-path latency table | ✅ | ✅ recovered | ✅ | n/a | ✅ | complete + **asserted** (§29 D1) |
| Lane isolation figures | ✅ | live only | ✅ | n/a | ✅ | live |
| LLM contamination runs | ✅ | ✅ raw | ✅ | n/a | ✅ | complete |
| Warm-start "no effect" | ✅ | ✅ | ✅ | ✅ | ✅ | complete (§29 D2) |

---

## 28. Sources of truth

| Question | Authoritative source | Not authoritative |
|---|---|---|
| What is the project's status and what is next? | `docs/ROADMAP.md` | The README's tone. |
| What does the architecture do and why? | The **header comments** in `include/titans/lanes/*.hpp`. They are the design document. | `docs/API_REFERENCE.md` (stale), `docs/ARCHITECTURE.md` (accurate but summary-level). |
| What is the engine's runtime configuration? | `config/engine.json`, then flags. | Anything YAML — none exists. |
| What data was used? | `data/raw/*.manifest` — URL, SHA256, row count. | The CSVs (gitignored, regenerable). |
| What is the official result for claim X? | The file under `results/` that the relevant `docs/*.md` links. | The README's summary table, which rounds. |
| What is the regression gate's baseline? | `results/benchmark_history/local/` — **40 committed runs, treated as data.** | Any freshly built series. |
| What must be true for the project to be correct? | `tests/` + `scripts/reproduce.sh` + `.github/workflows/ci.yml`. | Prose. |
| Which branch is current? | **`main` at `7aea82f`.** | `origin/HEAD`, which points at a 9-commit-old branch. |
| Which files must never be hand-edited? | Everything under `results/` — generated or captured. `data/raw/*.manifest` — written by the fetch script. | |
| Which files can be rebuilt from scratch? | `data/raw/*.csv` (from the manifests), `build*/`, `results/benchmark_history/reproduce/`, `figures/`. | |
| What was measured vs what was assumed? | The tool's own stdout. Every one prints its caveats, its refusals and its verdict inline. | |

**Per-claim document ownership:**

| Document | Owns |
|---|---|
| `docs/FRESHNESS.md` | P0 — the advisory-age contract and the parameter-vs-decision finding |
| `docs/LATENCY.md` | P1 — tick to trade, the probe cost, coordinated omission |
| `docs/REGRESSION.md` | P2 — the gate, its two bars, the bimodality |
| `docs/SELECTION.md` | P3 — fold boundary, embargo, multiple testing |
| `docs/CONTEXT_COST.md` | P4 — freshness decay and the context sweep |
| `docs/SUBSET.md` | P5 — subset selection and the λ result |
| `docs/RESEARCH_FRAMEWORK.md` | The contamination study's rules and hardware needs |
| `docs/RELATED_WORK.md` | What here is new and what is a re-implementation |
| `docs/ARCHITECTURE.md` | The mechanism, and why each shape was chosen |
| `docs/API_REFERENCE.md`, `DEBUGGING_GUIDE.md`, `TROUBLESHOOTING.md` | **Stale.** Treat as historical. |

---

## 29. Documentation discrepancies

Ten places where a document, a script or a build file disagrees with the code.
**In every case the code's behaviour is what is described first.**

### D1 — The README's latency table cited an artifact that had been overwritten — **FIXED 2026-09-09**

**What was wrong.** The README's nine-row "Measured performance" table matched
no benchmark JSON in the working tree. It matched
`results/benchmark_GW-X570-Taichi_20260830.json` **exactly — including the
throughput column — as that file existed at commit `24dc066`**. Commit
`f5e59b4`, which introduced `scripts/reproduce.sh`, overwrote it with a
different run at the same path, because the script wrote its own benchmark
output to `results/benchmark_$(hostname)_$(date +%Y%m%d).json` and that was the
same path on the same day. **The script written to re-derive every number in the
README destroyed the evidence for one of them.** `[GIT]`

**What was done.**

1. **Recovered.** `git show 24dc066:results/benchmark_GW-X570-Taichi_20260830.json`
   is now committed as
   `results/benchmark_GW-X570-Taichi_20260830_readme.json`, byte-identical, under
   a name no run of `reproduce.sh` can generate.
2. **Prevented.** `scripts/reproduce.sh` now writes its benchmark JSON to
   `/tmp`, with a comment naming this incident — the same treatment the
   tick-to-trade, subset and embargo-sweep stages already had.
3. **Detected.** `scripts/check_readme_table.py` parses the README's markdown
   table and compares every row against the artifact on cost, minimum **and**
   throughput. It runs as a `reproduce.sh` stage and as a CI step in
   `benchmark-self-check`. Verified in both directions: it passes on the
   recovered artifact and fails with 26 named disagreements on the file that
   overwrote it. `[RUN]`
4. **Documented.** The README now names its source file and says plainly what
   happened to the previous one.

**What remains true:** a latency table is not re-derivable — absolute latency
depends on the host, which the README says at length. The checkable property is
that the prose still agrees with the file it cites, and that is now checked.

### D2 — A document claimed a fix its own tool did not contain — **FIXED 2026-09-09**

**What was wrong.** `src/tools/subset_main.cpp` compared the two mean annealing
energies against an **absolute** epsilon of `1e-9`. On energies near −1023.7
that is the thirteenth significant figure, so the branch always fired, and the
committed transcript read:

```
        cold     -1023.7164         119.9 us
        warm     -1023.7157         118.6 us
  Lower energy is better. Warm starting found WORSE optima, which is a real
  risk: a start inside one basin is a start that may not leave it.
```

A mechanism invented to explain a difference in the seventh significant figure
whose sign flips between runs. Meanwhile `docs/SUBSET.md` asserted *"the tool
now reports the difference against its own scale rather than announcing a
direction for it"*, which was false. `[CODE]` `[ARTIFACT]` `[DOC]`

**What was done.**

1. **The verdict is now a paired interval.** Every one of the 2000 rolling
   windows is solved both ways, so the differences are paired and
   `eval::bootstrap_mean_ci` gives an interval over windows. A direction is
   announced only when that interval excludes zero; otherwise the tool prints
   `NOT RESOLVED`, the same idiom `titans_lanes` already uses.
2. **The difference is reported against the energy scale**, so the reader sees
   the order of magnitude rather than a verdict derived from it.
3. **The artifact was regenerated** over all 28 days. It now reads:

```
  Paired over 2000 windows; warm minus cold, lower is better.
    mean difference        +6.2965e-04
    95% CI over windows    [-7.7119e-04, +2.0793e-03]
    against the energy     6.2e-07 of |-1023.7|
    warm lower on          988 of 2000 windows

  NOT RESOLVED. The interval spans zero and the difference is
  6.2e-07 of the energy. No direction is claimed [...]
```

   988 of 2000 is 49.4% — a coin flip, which is independent confirmation.
4. **Verified minimal.** Diffing the 843-line transcript against the previous
   one with microsecond timings masked leaves **exactly the 13 lines of the
   warm-start verdict**; the JSON is identical apart from `solve_us`. Every
   informedness, interval, sign test and deflation came back the same. `[RUN]`
5. `docs/SUBSET.md` now carries the paired numbers and records the episode.

**What this exposed and did not fix:** nothing in the repository checks a
document against the tool it describes. D1 got such a check
(`scripts/check_readme_table.py`); this class of defect in general did not, and
it is the reason both D1 and D2 survived. See §30.2 Q28.

### D3 — `docs/ARCHITECTURE.md` says 15 test modules — **LOW**

Component map row: *"`tests/` | 15 modules"*. There are **20**. `[RUN]`

### D4 — `docs/API_REFERENCE.md` documents none of the current active path — **MEDIUM**

Its ten sections cover core types, versioned entities, contamination injection,
evaluation metrics, the experiment framework, LLM integration, data adapters,
distributed experiments, GPU monitoring, utilities. **There is no section for
`lanes/`, `eval/`, `bench/` or `opt/`** — i.e. everything P0–P5 built. It also
documents `data_adapters.hpp`, which is an orphan holding the superseded leaky
labeller. Last touched 2026-08-30, before the roadmap work began. `[RUN]` `[GIT]`

### D5 — `scripts/reproduce.sh` stage title said 12 modules — **FIXED 2026-09-09**

The stage header read `"Unit tests (README: 12 modules)"` while the README and
the suite both said 20. Corrected in passing while fixing D1, since the same
stage region was being edited. `[CODE]`

### D6 — `reproduce.sh`'s figure gate passes on any error — **MEDIUM**

**`reproduce.sh`:**
```bash
python3 python/research/generate_figures.py --results /nonexistent >/dev/null 2>&1
check "generate_figures.py refuses to fabricate data" test $? -ne 0
```

**CI, by contrast**, installs `pandas numpy matplotlib seaborn` first and then
`grep -q "no synthetic fallback"`.

On the reference host `seaborn` is absent, so the script dies with
`ModuleNotFoundError` at import and **the gate goes green without ever reaching
the refusal**. `[RUN]` A regression that restored the `np.random` fallback would
still pass this stage on any host missing a plotting dependency.

**Fix:** grep the message in `reproduce.sh` too, or fail loudly when the import
fails.

### D7 — The tick-to-trade artifact's `machine` block is half empty — **MEDIUM**

`results/latency/ticktotrade_2024-01-15.json` carries `build_flags:
"unrecorded"`, `pinned_cpu: -1`, `tsc_ghz: 0`, `noise_floor_ns: 0`, while the
same run's stdout says `TSC 3.3999 GHz | single-shot floor: 20.00 ns`.
`pipeline_main.cpp:619` calls `MachineFingerprint::capture()` and never populates
those four fields; `TITANS_BUILD_FLAGS` is defined only for `titans_benchmark`.
`[CODE]` `[ARTIFACT]`

CI asserts the `caveats` block on *benchmark* JSONs only, so this passes.

### D8 — The Docker runtime image is five tools behind — **MEDIUM**

`Dockerfile` copies `titans_engine`, `titans_replay`, `titans_benchmark`,
`titans_dataset`, `titans_lanes`, `titans_experiment`, `titans_llm_experiment`.
It **omits** `titans_walkforward`, `titans_ticktotrade`, `titans_regression`,
`titans_context_cost`, `titans_subset` — every binary built for P1 through P5.
It also does not copy `scripts/` or `data/`, so `reproduce.sh` cannot run inside
the image, and it installs only `python/requirements.txt`, not
`python/research/requirements.txt`. `[CODE]`

### D9 — `gtest_discover_tests` is applied to a non-GTest binary — **MEDIUM**

`tests/CMakeLists.txt` branches on `find_package(GTest QUIET)`. In the GTest
branch it links `GTest::gtest_main` **and** compiles `test_main.cpp`, which
defines its own `main()`; the object file's `main` shadows the archive's, so the
custom runner wins. It then calls `gtest_discover_tests(titans_tests)`, which
runs the binary expecting `--gtest_list_tests` output. The binary ignores the
flag, runs the whole suite, and CMake parses its **stdout — including the ASCII
banner — as test names**:

```
Test #1: |_   _(_) |_ __ _ _ __  ___|_   _|__  ___| |_ ___.| | | | __/ _` |...
```

Result: **234 ctest entries**, each re-running the entire suite in ~2.9 s
(≈ 11 minutes total), all passing or failing together. `[RUN]`

CI is unaffected because it never installs GoogleTest, so it takes the
`add_test(NAME titans_tests ...)` branch — one test. The README and
`docs/*` tell developers to run `ctest --output-on-failure`, which on a
GTest-equipped workstation is 234× slower than it needs to be and produces
meaningless names.

**Fix:** drop the GTest branch entirely (the suite does not use GTest) or replace
`gtest_discover_tests` with a plain `add_test` in both branches.

### D10 — `docs/DEBUGGING_GUIDE.md` documents dead code — **LOW**

It describes `Logger`, `Profiler`, `MemoryTracker` and `DebugConsole` from
`include/titans/core/debug.hpp`. **Nothing in the repository includes that
header.** `[RUN]`

### D11 — `docs/RESEARCH_FRAMEWORK.md` describes an output tree that does not exist — **LOW**

It documents `experiments/{baseline_comparison,sensitivity_analysis,ablation_study}/`
and a populated `figures/` directory. Neither exists in the checkout; `figures/`
is present and empty, `experiments/` is absent. `[RUN]` `titans_experiment`
writes nothing to disk at all; only `titans_config_experiment` would create
`experiments/`.

---

## 30. Open questions

Questions the repository cannot answer. Split by whether they are *known
unknowns* the project already tracks, or things this audit could not resolve.

### 30.1 Tracked by the project itself

From the "What is still open" sections of the six roadmap documents. `[DOC]`

**Latency (P1)**
1. **Why does the book stage cost 40.4% of the path?** Two candidates are named
   in `docs/LATENCY.md`; the first is described as cheap to settle. Located, not
   explained.
2. **`service` and `response` disagree by 72× and nothing stops a future reader
   quoting the smaller one.** Only `response` is tick-to-trade.
3. **The saturation knee is bracketed, not measured.** The sweep degrades
   monotonically from 125× upward; finding the knee needs a quieter machine, not
   more patience.

**Regression (P2)**
4. **The effect bar has no absolute floor.** A shift should have to clear both
   K·σ *and* the dispersion of the widest timescale there is evidence about. The
   tool measures that timescale when given a `--reference` run and does not feed
   it back into the bar.
5. **The bimodality is measured, not explained.** Alignment, page colouring and
   core placement are the candidates; separating them needs `perf` counters and
   a controlled allocator. **Until then six of nine metrics are ungated.**
6. **No real cross-commit series exists.** CI uploads one run per commit as an
   artifact and nothing accumulates them.

**Selection (P3)**
7. **Trials across sessions are not tracked.** `--trials` covers what the caller
   knows about; nothing accumulates what the project has collectively tried.
   Doing it properly means a registry, not a flag.
8. **No combinatorial purged cross-validation.** Walk-forward is the right
   realism protocol and comparatively weak at false-discovery prevention.
9. **The *effective* number of trials is not estimated.** Correlated sweeps
   could support a much smaller effective count.

**Context cost (P4)**
10. **Prefix caching is the obvious next measurement.** Consecutive decision
    points share almost all their context, so a cache should collapse the
    prefill term and leave the fixed floor — separating "context is expensive"
    from "this serving stack recomputes it".
11. **The 383 ms non-prefill floor is unattributed.** Sampling? Scheduling?
    HTTP? Nothing here says.
12. **One model.** Whether a more capable model's *at-context* accuracy would
    rise with context — making the trade a real trade rather than a one-sided
    loss — is the question the instrument now exists to answer and needs
    hardware this host does not have.

**Subset (P5)**
13. **Why do the big trades carry it?** The entire measured gain comes from
    keeping large `|signed size|` and the mechanism is unexplained.
14. **A consumer that is not a sum.** Choosing *which* K trades enter a prompt
    at a fixed token budget is the same problem with a consumer where redundancy
    might actually matter — the one place λ has a reason to earn its keep.
15. **Larger n.** λ = 0 winning is a boundary result over [0, 2] at n = 64.

**Freshness (P0)**
16. **The lane recovers 89.8%, not 100%.** The residual is the remaining ~3-trade
    delay on the parameter's *first* availability plus the window the fast lane
    rebuilds from scratch.
17. **Lane and reference informedness levels are not directly comparable**
    (per-publish vs per-trade calibration subsamples). The agreement rate is.

**Data**
18. **Why do three of 28 days fail the label audit?** The intraday-trend
    hypothesis is implemented, swept and dead. Nothing else has been proposed.

### 30.2 Raised by this audit and unresolved

19. ~~**Should the README's latency table be restored or regenerated?**~~
    **Answered by action on 2026-09-09: restored**, on the grounds that it
    preserves provenance and that regenerating would churn published numbers for
    no gain. The recovered artifact is committed and checked. Whether the table
    should *also* be refreshed against current hardware is still a judgement
    nobody has recorded. `[UNKNOWN]` §29 D1.
20. **Are the other three context-cost runs recoverable?** `docs/CONTEXT_COST.md`
    tabulates four runs (2 × 200 points, 2 × 400 points) and only one artifact is
    committed. The claim "the 512 arm is resolved negative in all four" rests on
    the document. `[UNKNOWN]` §27.5.
21. **Does `data.binance.vision` still serve 2024 daily files?** The fetch
    script warns daily archives exist "only for roughly the last few months". If
    they have aged out, the 28 manifests are the only surviving record of the
    input and nothing can be re-derived from scratch. **Not tested during this
    audit.** `[UNKNOWN]` **This is the single largest reproducibility risk.**
22. **Does `generate_figures.py` still work?** Its loader `rglob`s every `*.json`
    under `results/`, which now holds six unrelated schemas, and builds a
    DataFrame from them. It cannot run on this host (`seaborn` missing).
    `[UNKNOWN]`
23. **Is `titans_distributed_experiment` still functional?** 755 lines of
    multi-process sharding with no committed artifact and no test. `[UNKNOWN]`
24. **Why is `origin/HEAD` still on `claude/hybrid-trading-system-dVT8H`?**
    Deliberate, or an unchanged GitHub default? A default clone gets a
    repository without P0–P5. `[UNKNOWN]`
25. **What happened between 2025-12-16 and 2026-07-18?** A seven-month gap
    between the first and second commits, with no evidence in the repository.
    `[UNKNOWN]`
26. **Are `results/benchmark_GW-X570-Taichi_20260907.json` and `_20260908.json`
    meant to be committed?** Two of the four dated benchmark files are tracked
    and two are not; there is no rule stated anywhere. `[UNKNOWN]`
27. **Should the two untracked dated benchmark JSONs be deleted?** Nothing
    generates them any more after the D1 fix, and no rule says which dated files
    belong in Git. `[UNKNOWN]`
28. **Should the prose-versus-artifact check generalise?**
    `scripts/check_readme_table.py` covers exactly one table. D1 and D2 were both
    documents disagreeing with the things they described, and both survived
    because nothing compared the two. Whether every headline table in `docs/`
    should be machine-checked against its artifact — and whether that is worth
    the coupling — is undecided. `[UNKNOWN]`
29. **Was `context/data_adapters.hpp` kept deliberately?** It holds the leaky
    labeller the project spent a commit removing, and nothing includes it.
    Keeping it is a hazard; deleting it loses the record. `[UNKNOWN]`

### 30.3 The three questions a new owner should answer first

1. ~~**Fix the two broken provenance chains** (§29 D1, D2).~~ **Done
   2026-09-09.** What is left of it: the *class* of defect is untouched. Both
   were documents disagreeing with the things they described, and only one now
   has a check (§30.2 Q28). TD-5 is the same shape and is still open — a
   `reproduce.sh` gate that passes on any non-zero exit, including a missing
   Python module.
2. **Verify the data source is still live** (Q21). Everything else is
   conditional on it. `python3 python/data/fetch_binance.py --symbol BTCUSDT
   --date 2024-01-15 --out /tmp/check` answers it in a minute.
3. **Decide what the LLM half is for.** It is a third of the code, has no
   positive result, is blocked on hardware, and the "explicitly not doing" list
   already rules out the obvious next move. Either commit to a larger model or
   mark the subtree dormant in the README so a reader does not spend a week
   there.

---

## Appendix A — File and symbol index

Quick lookup for the things named in this guide.

| Symbol / file | Location |
|---|---|
| `Advisory`, `AdvisorySlot`, `AdvisoryView` | `include/titans/lanes/advisory.hpp` |
| `FreshnessPolicy`, `FreshnessMonitor`, `CadenceBudget` | `include/titans/lanes/freshness.hpp` |
| `LaneObservation`, `LaneBridge<N>` | `include/titans/lanes/lane_bridge.hpp` |
| `FlowPolicy`, `FlowCalibrator` | `include/titans/lanes/flow_policy.hpp` |
| `LatencyBudget`, `BudgetScope`, `BudgetReport` | `include/titans/lanes/latency_budget.hpp` |
| `rdtsc_start`, `rdtsc_end`, `TscClock`, `is_resolvable` | `include/titans/bench/cycle_timer.hpp` |
| `Harness`, `BenchmarkResult`, `MeasurementMode` | `include/titans/bench/harness.hpp` |
| `CpuTopology`, `pin_to_cpu`, `MachineFingerprint` | `include/titans/bench/platform.hpp` |
| `Histogram`, `record_corrected` | `include/titans/bench/histogram.hpp` |
| `Stage`, `StageTrace<bool>`, `PipelineHistograms`, `LatencyFormatter` | `include/titans/bench/stage_trace.hpp` |
| `auc`, `PolicyOutcome` | `include/titans/eval/metrics.hpp` |
| `Rng`, `bootstrap_mean_ci`, `sign_flip_test` | `include/titans/eval/bootstrap.hpp` |
| `Fold`, `run_policy_over_day`, `split_by_embargo`, `check_fold` | `include/titans/eval/walk_forward.hpp` |
| `evaluate_gate`, `find_change_points`, `assess_modality` | `include/titans/eval/change_point.hpp` |
| `normal_sf`, `expected_max_z`, `deflate` | `include/titans/eval/deflated.hpp` |
| `index_after_delay`, `run_policy_delivered`, `check_context_growth` | `include/titans/eval/delivered.hpp` |
| `SubsetProblem`, `build_subset_qubo`, `solve_{exact,greedy,annealing}` | `include/titans/opt/qubo.hpp` |
| `AggTrade`, `ToxicFlowLabelConfig`, `BinanceToxicFlowDataset` | `include/titans/context/binance_dataset.hpp` |
| `LLMBackend`, `PromptBuilder`, `ResponseParser`, `OllamaConfig` | `include/titans/context/llm_interface.hpp` |
| `OllamaBackendImpl`, `VLLMBackendImpl`, `LLMBackendFactory` | `include/titans/context/ollama_backend.hpp` |
| `SPSCQueue`, `MPSCQueue` | `include/titans/core/spsc_queue.hpp` |
| `ObjectPool`, `Arena`, `RingBuffer` | `include/titans/core/memory_pool.hpp` |
| `EventBus`, `publish_prestamped` | `include/titans/core/event_bus.hpp` |
| `Value`, `Parser`, `Serializer` | `include/titans/core/json.hpp` |
| `L2OrderBook`, `OrderBookManager` | `include/titans/trading/order_book.hpp` |
| `RiskManager`, `RiskLimits`, `check_order` | `include/titans/trading/risk_manager.hpp` |

## Appendix B — Audit method

What was actually done to produce this document, so a reader can judge its
reliability.

**Read in full:** `CMakeLists.txt`, `tests/CMakeLists.txt`, `.gitignore`,
`Dockerfile`, `docker-compose.yml`, `.github/workflows/ci.yml`,
`.github/tsan.supp`, `scripts/reproduce.sh`, `scripts/bench_series.sh`,
`scripts/inject_regression.py`, `README.md` (920 lines),
`include/titans/lanes/*.hpp` (all five), `include/titans/bench/cycle_timer.hpp`,
`histogram.hpp`, `include/titans/eval/{metrics,bootstrap,walk_forward}.hpp`,
`include/titans/context/binance_dataset.hpp`, `config/engine.json`,
`config/experiment.json`, `docs/{SUBSET,CONTEXT_COST}.md`, and the
"what is still open" section of every other roadmap document.

**Surveyed by structure:** every remaining header (doc comment + type list),
every `src/tools/*.cpp` (file header, argument list, exit codes), every test
module (case names).

**Executed:** `git log/branch/status/merge-base/ls-tree/rev-list`,
`./build/tests/titans_tests`, `ctest -N` and `ctest -I 1,2`, `--help` on six
tools, `titans_lanes --slo-p99-frac`, `generate_figures.py`, a Python module
availability probe, `nvidia-smi`, and an exact-`#include` orphan scan across the
whole tree. Parsed every JSON under `results/` for its schema and headline
values, and compared the README's latency table against **every version of every
benchmark JSON in every commit reachable from every ref** — which is how the
overwritten artifact in §29 D1 was located.

**Not executed:** `scripts/reproduce.sh` (any mode), the Docker build, anything
requiring a model backend, `generate_figures.py` past its import, and any data
download. Claims about those carry `[DOC]` or `[UNKNOWN]` rather than `[RUN]`.

**No source file, config, test, script or result artifact was modified.** This
document is the only file created.
