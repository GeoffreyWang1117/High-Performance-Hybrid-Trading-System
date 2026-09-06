# Roadmap

Ordered by priority, not by appeal. Each item says what industry practice it
comes from, what measurement in this repository demands it, and what has to be
true before it can be called done.

The ordering rule is that a measurement this repository already produced
outranks a technique that merely sounds current. Two items near the bottom are
the ones that would look best on a slide, and they are near the bottom because
the numbers say they would not help yet.

---

## P0 — Freshness SLO: make advisory age a contract term

**Status: DONE. Full account in [FRESHNESS.md](FRESHNESS.md).**

The hypothesis in this item was half right and the useful half was not the half
stated. Advisory age *was* the thing to measure, and gating on it bought
nothing: the gate was built, swept at five settings, and the outcome did not
move at any of them. The instrument that came out of building it — comparing
the delivered decision against the ideal one trade by trade — showed the lane
acting at nearly the right rate on only 40.1% of the right trades. A threshold
crossing is a moment, and the median advisory was three trades old.

The fix was to change what the lane ships. `Advisory::parameter` carries the
calibrated threshold and the fast lane evaluates the rule itself; agreement went
40.1% -> 89.8%, informedness from an unresolved median of -0.0102 to a resolved
+0.2426, and run-to-run spread from 0.0255 to 0.0028. Each payload declares its
own horizon, so the same delivery path breaches the contract shipping a decision
and meets it shipping a parameter. Both are asserted in `scripts/reproduce.sh`.

The original text follows, unedited, because the gate it proposed is the part
that did not work.

---


### What the measurement says

`titans_lanes` acts on advice whose age, in market time, is p50 82 ms, p99
1507 ms, max 4418 ms — against a **1000 ms** prediction horizon. One read in a
hundred acts on advice older than the entire horizon it predicts over. The same
policy scores **+0.19** informedness when the lane machinery is removed and
**≈0** through it. Raising the TTL from 250 ms to 60 s cut rejection 34.4% →
10.9% and moved the outcome not at all.

So `valid_until` bounds the wrong quantity. It expresses a latency budget; what
governs whether the advice is worth anything is its age relative to the
**horizon of the signal it carries**.

### What industry does

This is a solved problem outside trading, under the name *feature freshness*.

- [Tacnode](https://tacnode.io/post/real-time-ml-inference-feature-freshness):
  "Real-time ML is bottlenecked by feature freshness, not model latency — the
  model serves in 8 milliseconds while the features it scored are 40 seconds
  old." Feature pipelines run at propagation timescales 100–1000× the inference
  latency. That ratio is this repository's problem exactly.
- Netflix, via the same body of practice: recommendation quality is model
  quality × feature freshness, the serving budget is 50 ms at p99, and
  **everything is designed backwards from it**.
- [Chalk](https://chalk.ai/blog/why-feature-stores-have-freshness-ceiling): not
  every feature gets the same staleness budget; the feature set is split by
  freshness requirement and each one takes the cheapest path that meets it.
- Feature registries record a **freshness SLO per feature**, next to its owner
  and lineage. It is a declared, monitored property, not an implementation
  detail.

### The change

1. `Advisory` declares the horizon it is valid for, not just a wall-clock
   expiry. The fast lane rejects on `age > horizon_fraction * horizon`, with the
   fraction a configured contract term.
2. Slow-lane **publish cadence** becomes a budget with its own violation count,
   the way `LatencyBudget` already works for the fast path. A slow lane that
   cannot publish inside its cadence budget is failing its contract even if
   every advisory it does publish is inside its TTL.
3. `titans_lanes` reports the age distribution against the horizon as a
   first-class verdict, not a diagnostic.

### Done when

- The age p99 is a declared SLO and the tool fails the run when it is breached.
- Re-running the 27-fold comparison through the lane closes a stated fraction of
  the +0.19 → ≈0 gap, and the residual is attributed rather than shrugged at.
- A test asserts that a slow lane which publishes too slowly is reported as
  contract-breaking even when nothing expires.

---

## P1 — End-to-end tick-to-trade histogram, with per-stage attribution

**Status: DONE. Full account in [LATENCY.md](LATENCY.md).**

`titans_ticktotrade` runs the real path -- ingest, book, signal, risk, order --
over the real tape with the strategy logic inside the measured region, and
prints per-stage and end-to-end percentiles to p99.9. Two of the five stages
come back below the clock's resolution and are refused rather than printed,
which is the rule the benchmark table already followed and the reason the table
is worth reading.

The item asked for the coordinated-omission correction on the end-to-end number.
It is there, and building it produced a result the item did not anticipate: on
this workload **the correction recovers almost none of the tail it exists to
recover.** Measuring from each tick's own due time gives a p99 two orders of
magnitude above the handler-only figure -- 78.9 us against 1.10 us -- and the
correction closes under 2% of that gap while synthesising 8,741 samples. The
reason is structural. Tene's correction infers omission from a service time longer than
the expected interval, and here no single call is slow -- the queue builds from
arrivals bunching. The correction cannot see that, and it reports success
either way.

So the deliverable is the correction plus the evidence for when to distrust it.
A saturation sweep drives the tape from 125x to 64000x and shows the two metrics
moving in opposite directions: the handler-only p99 FALLS from 1844 ns to 621 ns
while the real tail climbs to 2.17 ms. A host-jitter control runs the same
schedule with no pipeline attached, so a tail that does not clear the machine's
own lateness is reported as measuring nothing.

Three things this left open, none of which block P2:

- The book stage is the largest at 40.4% and the table locates it without
  explaining it. `L2OrderBook::update_level` calls `now_ns()` unconditionally
  and the stage calls it twice; that is cheap to settle and has not been.
- `service` and `response` disagree by 72x, and nothing prevents a future reader
  from quoting the smaller one as tick-to-trade.
- The sweep brackets the saturation point rather than finding the knee, because
  the low end costs wall time proportional to 1/speed.

The original text follows, unedited.

---

### What the measurement says

Every latency number in this repository is a **per-operation** cost:
`try_push` 1.21 ns, `update_level` 24.03 ns. There is no number anywhere for
*how long it takes a trade to become an order*. The one composite figure is the
lane stage's 254 ns mean against a 500 ns budget, which is a mean and one stage.

### What industry does

- [Databento](https://databento.com/microstructure/tick-to-trade) defines
  tick-to-trade as last-bit-in to last-bit-out and measures it with **passive or
  regenerative network taps**, so the measurement never touches the critical
  path. Their named pitfall is the one this repo would walk into: "tick-to-trade
  latency is usually measured without any strategy or model logic. Hence, it can
  be misleading since some low latency trading strategies compute a very large
  number of features."
- [From NIC to P99](https://deepengineering.net/p/from-nic-to-p99-engineering-low-latency):
  "measuring p50 and p99 histograms at every stage of the pipeline is the only
  reliable way to know where your latency is coming from", with a p95
  wire-to-wire target under 50 µs, and a warning that TSC skew introduces false
  measurements.
- [Coordinated omission](https://www.scylladb.com/2021/04/22/on-coordinated-omission/)
  (Gil Tene): a closed-loop benchmark reports p99 10 ms for a service that froze
  for 500 ms; open-loop scheduling or HdrHistogram's correction reports 460 ms.

### Where this repo already stands

The replay pacing in `titans_lanes` is **already open-loop** — it schedules
against absolute due times and records lag when it falls behind, rather than
starting the next clock after the previous finish. That is the right shape and
it is why the lag distribution is trustworthy. What is missing:

- The benchmark harness is amortized-batch, which is correct for per-op cost and
  structurally **cannot** see a stall. There is no end-to-end number to omit
  from, which is a worse problem than omitting from one.
- The lag percentiles are computed from a sorted vector, not a histogram with a
  correction, so a stall's downstream cost is visible but not attributed.

### The change

Per-stage HdrHistogram-equivalent (ingest → book → signal → risk → order), fixed
bucket layout so runs are comparable, p50/p90/p99/p99.9 reported per stage and
end to end, with an explicit expected-interval correction on the end-to-end
number. Keep the existing rule that a percentile below the measured noise floor
is refused rather than printed.

### Done when

- One command prints a per-stage and end-to-end latency table with p99.9.
- A test injects a deliberate stall and asserts the corrected end-to-end p99
  moves while the uncorrected one does not. That is the property, and it is
  invisible without the test.

---

## P2 — Continuous benchmarking with change-point detection

### What the measurement says

CI runs `titans_benchmark` and checks that it self-calibrates. It does not
compare against yesterday. A 20% regression in `update_level` would pass CI
silently.

### What industry does

- [Bencher](https://bencher.dev/docs/explanation/continuous-benchmarking/):
  continuous benchmarking is to performance what CI is to correctness — catch
  the regression before it ships.
- [FOSDEM 2026, Continuous Performance Engineering](https://fosdem.org/2026/schedule/event/YNB7KR-continuous-perf-engineering/):
  automate the harness in CI, tune the infra for repeatability, and use **change
  point detection** to alert with minimal false positives.
- The load-bearing caveat: **GitHub Action runners see >30% variance between
  runs.** A naive threshold alert on a shared runner is a false-positive
  generator.

That caveat is why this item is P2 and not P1, and it is also why this
repository is unusually well placed to do it right: the harness already measures
its own noise floor and already refuses to report what it cannot resolve. The
regression gate should be built on the same rule — alert only when the change
exceeds the run's own measured noise, not a fixed percentage.

### Done when

- Benchmark JSON is retained per commit and a change-point test runs against the
  series.
- The gate fires on an injected regression and stays quiet across a no-op commit
  run ten times on CI hardware. Both directions are asserted; a gate that never
  fires and a gate that always fires are equally useless and look identical on a
  green build.

---

## P3 — Close the statistical gaps the walk-forward left open

### What the measurement says

The 28-day walk-forward is sound on its own terms and has two known holes.

**No embargo.** Training ends on day *t−1* and testing starts on day *t*. The
label looks 1000 ms into the future, so the last trades of the training day and
the first of the test day are adjacent in time. At day granularity the overlap
is seconds against days and almost certainly immaterial — but "almost certainly
immaterial" is a claim, and it is not currently measured.

**No multiple-testing correction.** A drift-window sweep at 5 settings and a TTL
sweep at 4 have already been run. Nothing was selected on those sweeps, and all
points were reported, which is why this is a gap rather than a defect. The moment
anything *is* selected, the reported interval is optimistic.

### What industry does

Purged and embargoed cross-validation, Combinatorial Purged CV, and the
**Deflated Sharpe Ratio** — a correction to the reported statistic for the number
of trials that produced it. The consensus in that literature is that walk-forward
is the right *realism* protocol and comparatively weak at false-discovery
prevention, which is exactly the trade this repo has taken.

### The change

An embargo parameter with the sensitivity actually run and reported; a trials
counter carried in the result JSON; and a deflated interval printed beside the
raw one whenever the trial count exceeds one.

### Done when

- The result file records how many configurations were evaluated to produce it.
- Sweeping a parameter and reporting the best point without a correction is
  impossible without the tool saying so.

---

## P4 — Context length as a freshness cost

### What the measurement says

Nothing yet — this is the cheapest genuinely new measurement available here.

### Why it is interesting

The research framework studies context strategies; P0 establishes that advice
ages against a horizon. Those two connect: **prefill cost scales with context
length, so a richer context directly buys staleness.** "More context is better"
and "fresher advice is better" are in direct tension, and the tension is
measurable on the hardware already here.

Modern serving stacks make the mechanism explicit —
[vLLM](https://vllm.ai/blog/2025-09-05-anatomy-of-vllm) separates prefill from
decode, and chunked prefill and prefix caching exist precisely because prefill
cost is the thing that moves with context length. Disaggregated prefill/decode
is a production feature specifically to stabilise decode latency.

### The change

Sweep context length in `titans_llm_experiment`, record time-to-first-token and
total latency per arm, and plot accuracy against advisory age rather than against
context size. If the curve has an interior optimum, that is a real result and it
is one this repository is unusually well set up to measure, because it is the
only place with both the context strategies and a lane that reports age.

### Done when

- An accuracy-versus-age curve exists on real model latencies.
- The degenerate-classifier gate still governs it: a model that answers the same
  thing regardless of context produces a flat line and must be reported as
  degenerate, not as "context does not matter".

---

## P5 — QUBO subset selection on the slow lane

Deliberately below P0. The design is worked out (see the ICML-workshop
formulation this was drawn from: monitoring as combinatorial subset selection,
λ-controlled risk–diversity, greedy/MMR/*k*-DPP as points on one Pareto
frontier), and warm-starting it across rolling windows answers an open question
a confidence-5 reviewer asked directly.

It is P5 because **a smarter slow lane is worth nothing until the advice
arrives in time.** The current lane loses a +0.19 signal in delivery. Making the
signal better while it still cannot be delivered would produce a more
sophisticated version of the same zero — and would look, in a results table, like
the QUBO had failed.

Two things must be true first: P0 lands, and the lane closes a measurable
fraction of the delivery gap. Then the QUBO's own selling point becomes testable
here, because this repo scores against **external** forward toxic-flow labels
rather than against the diversity term the QUBO itself optimises — which is the
tautology its own reviewers flagged.

---

## Explicitly not doing

- **Kernel bypass (DPDK, ef_vi, Solarflare).** Worth 20–45 µs on a real NIC path.
  This system has no live feed and replays from disk, so there is no wire to
  bypass. It would add a large dependency and improve a number that does not
  exist here. Revisit only if TLS and a live feed land.
- **NUMA pinning across sockets.** The host is single-socket. Pinning is already
  done and the fingerprint already records what was not controlled.
- **Quantum backends.** The source paper reports them as a compatibility check
  rather than an advantage claim, at $9.83 of QPU time. In an engineering
  project it reads as decoration.
- **A fourth prompt revision for the degenerate LLM.** Three changed the
  reasoning text on 45 of 60 trials and not one classification. A fourth, tuned
  until the number comes out right, is the failure mode this repository exists
  to prevent.
