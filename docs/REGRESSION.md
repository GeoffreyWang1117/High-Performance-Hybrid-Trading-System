# The benchmark regression gate

ROADMAP P2. What it asked for, what the hardware turned out to be doing, and
the two bugs that made the first version of the gate silently useless.

Reproduce with:

```
scripts/bench_series.sh results/benchmark_history/local 40
./build/titans_regression results/benchmark_history/local \
    --reference results/benchmark_GW-X570-Taichi_20260830.json \
    --reference results/benchmark_GW-X570-Taichi_20260906.json
```

Full output in [`results/regression/`](../results/regression/).


## The gap this filled

CI ran `titans_benchmark` and checked that it self-calibrates. It never compared
today's number against yesterday's, so — the roadmap's own example — a 20%
regression in `update_level` passed silently. Nothing in this repository had
ever compared two runs.

The obvious fix, failing when a metric moves more than X%, is a false-positive
generator on shared hardware. The continuous-performance literature is
unanimous on the alternative: change point detection. This uses **E-Divisive**
(Matteson & James 2014), which is what MongoDB deployed for exactly this
problem — non-parametric, finds a level shift rather than an outlier, and takes
its significance from a permutation test on the series itself.


## The item's own prescription was wrong

P2 said the gate should "alert only when the change exceeds the run's own
measured noise". The harness already reports that number, `rep_spread_pct`, the
spread across repetitions inside one process. It looked like the answer and it
is not the answer.

Repetitions inside a process share a cache state, a page mapping, a clock domain
and a thermal state. Everything that perturbs the process perturbs all fifteen
of them together, so the number they agree on is not evidence about the next
process. Concretely, from the two benchmark runs committed in this repository,
same binary, same host, seven days apart:

| metric | 30 Aug | 6 Sep | change | within-run spread claimed |
|---|---|---|---|---|
| `operator new/delete` | 11.31 ns | 16.34 ns | **+45%** | 1.4% |
| `EventBus::publish` (auto) | 56.82 ns | 67.28 ns | +18% | 0.3% |
| `ObjectPool::deallocate` | 1.58 ns | 1.30 ns | −18% | 5.5% |

A run reported itself internally consistent to 1.4% while sitting 45% away from
the same binary's median. That is not a lower bound being conservative; it is a
quantity that carries no information about the one the gate needs.

So the dispersion has to come from the **series**. `titans_regression` computes
both and prints the ratio, so the gap is reported rather than assumed.


## Then the hardware turned out to be bimodal

Building a 40-run baseline to estimate between-run dispersion produced something
the item did not anticipate. `SPSCQueue::try_push` does not have a level with
noise on it. It has **two levels**:

```
  1.4832  1.4883  1.4947  1.1345  1.0199  1.0183  1.0540  1.4737  1.4908 ...
```

One of two values, ~1.02 ns or ~1.48 ns, **46% apart**, chosen once at process
start and stable for that entire run. Six of the nine benchmarked metrics do
this:

| metric | fast | slow | gap | separation | share in slow mode |
|---|---|---|---|---|---|
| `SPSCQueue::try_push` | 1.017 | 1.483 | 45.8% | 19.9 sd | 48% |
| `SPSCQueue::try_pop` | 0.768 | 0.939 | 22.3% | 19.6 sd | 38% |
| `ObjectPool::deallocate` | 1.059 | 1.267 | 19.7% | 12.5 sd | 28% |
| `operator new/delete` | 11.451 | 13.433 | 17.3% | 12.9 sd | 22% |
| `EventBus::publish` (preset) | 38.093 | 42.107 | 10.5% | 8.5 sd | 25% |
| `EventBus::publish` (auto) | 58.625 | 63.570 | 8.4% | 9.1 sd | 20% |

Separations of 8.5 to 19.9 robust standard deviations, so these are modes and
not a threshold being generous. The usual suspects — code and data alignment
under ASLR, page colouring, which physical core the pin lands on relative to the
CCX — are all fixed for a process and vary between them.

Two consequences, and they point in opposite directions.

**The level test survives it.** The modes alternate at random, so no segment is
systematically different and E-Divisive correctly finds nothing. A percentage
threshold would have fired on nearly every run.

**The effect-size bar does not.** A dispersion computed across a bimodal series
describes neither mode, so a run of luck in the mode mix would read as a large
shift. The gate detects bimodality, reports the numbers, and excludes those
metrics from the verdict rather than judging them on a statistic that does not
mean what it says.

This also puts a floor under what the README's per-operation table can claim.
It already said that what should reproduce elsewhere is "the ordering and the
order of magnitude, not the third significant digit". On this host the second
digit is not reproducible either, and now that is measured rather than hedged.


## The bug that made the gate useless

The first version of the modality check sorted the series and clustered it. That
is the natural way to look for two modes, and it silently destroys the gate,
because **a regression is also two clusters** — fast before, slow after. Handed
a planted +20% step in `update_level`, it reported "two modes 20.6% apart,
cannot gate" and passed the build. A gate that never fires and a gate that
always fires look identical on a green build, and this one had quietly become
the first.

What separates the two cases is not spread, it is arrangement. Two modes
interleave; a step is one block then the other. So the cluster labels are tested
in their original order with Wald–Wolfowitz runs: under interleaved modes the
label sequence looks like a coin, under a step it is two runs against an
expectation of about n/2. A z-score at or below −2 means the split is explained
by time, and the series is a shift rather than a mode mix.

`test_a_step_is_not_mistaken_for_two_modes` and
`test_interleaved_modes_are_not_gated` pin both directions. Only one of them
would have caught this.


## Two bars, and why the second one exists

A change has to clear both:

1. **Significant.** The level shift survives a permutation test on the series.
2. **Larger than the noise.** The shift exceeds 3× the dispersion the series
   shows with its own steps removed.

The second bar is not decoration. Injecting a step of known size into the real
40-run baseline, 12 runs affected:

| injected | shift seen | p | effect | gate |
|---|---|---|---|---|
| 1% | — | — | — | clean |
| 2% | +2.8% | 0.0050 | 1.3 sd | clean |
| 3% | +3.9% | 0.0010 | 1.8 sd | clean |
| 4% | +4.9% | 0.0010 | 2.3 sd | clean |
| 5% | +5.9% | 0.0010 | 2.8 sd | clean |
| **6%** | **+6.9%** | **0.0010** | **3.2 sd** | **REGRESSION** |
| 10% | +10.9% | 0.0010 | 5.1 sd | REGRESSION |
| 20% | +21.0% | 0.0010 | 9.8 sd | REGRESSION |

**A 2% injection is already statistically significant.** A gate built on
significance alone would fire there — three times more sensitive than the noise
justifies, on a host where the same metric's own runs vary by more than that.
That gate gets switched off by its owners inside a week.

The sensitivity floor on this host is 6%, and the roadmap's motivating case, a
20% regression in `update_level`, is caught at 9.8 sd.


## Noise by timescale

The dispersion a benchmark shows depends on the span it is measured over, and
every level here is a real measurement from this repository:

| span | what varies | measured |
|---|---|---|
| within run | almost nothing | 1.40% |
| within session, 40 back-to-back runs | alignment, mode mix | 1.82% |
| between sessions, 7 days apart | everything | 10 of 18 metrics land outside the 40-run range; worst 48.1% |

A gate has to be calibrated at the span it runs at. For CI that is across
commits and days — the widest one — and the narrower numbers are not
conservative approximations of it, they are different quantities.


## What CI can and cannot assert

The `regression-gate` job **cannot** tell you a commit is slower than its
parent. That needs a series whose runs differ only by commit, and a shared
runner does not provide one; GitHub runners are reported at over 30% variance
between runs, which is five times the sensitivity floor measured above.

What it does instead is build a 20-run series inside a single job and assert the
gate still works in all three directions:

- quiet on that no-op series, while printing the runner's own noise;
- fires on a planted 25% regression, exit 6;
- refuses a series too short to judge, exit 2, rather than passing it.

That third one matters as much as the others. A silent pass and a refusal are
the same exit code unless someone makes them different.


## Threats to these numbers

- **One host.** Bimodality is a property of this machine and this build. The
  mechanism generalises; the specific 46% gap does not.
- **No `isolcpus`/`nohz_full`, SMT and turbo on.** Recorded in the fingerprint
  of every run. The bimodality is not explained by these — modes are stable
  within a process and switch between them, which is the wrong shape for
  scheduler interference.
- **Back-to-back runs understate the span CI faces.** Stated in the tool's own
  output and quantified by the between-session row above.
- **The 3 sd bar is a choice.** It is reported, configurable with `--sigmas`,
  and the sensitivity table shows exactly what it buys.


## A false positive worth keeping

The first end-to-end run of `scripts/reproduce.sh` after this landed fired the
gate on a series built moments earlier with no code change:

```
EventBus::publish (timestamp preset)  37.209 ns  series 0.86%  +4.9%  p 0.025  5.6 sd  REGRESSION
```

The gate was right and the assertion around it was wrong. Twenty back-to-back
runs taken immediately after eight other reproduce stages are not a stationary
baseline, and the detector found a real level shift in the timing. But it is
the host that shifted, not the code.

It also exposes a limit of a purely sigma-based bar. That metric's within-series
dispersion was 0.86%, so 3 sd is about 2.6% and a 4.9% move clears it — while
the between-session table above measures deviations up to 48% on this same
machine. **The gate was claiming a sensitivity the widest timescale cannot
support.** A bar in standard deviations is scale-free, which is what makes it
portable and also what lets it become arbitrarily fine on a metric that happens
to be quiet within one session.

`scripts/reproduce.sh` now makes its pass/fail assertions against the committed
baseline, which is data rather than a measurement of whatever the host is doing,
and runs a fresh series purely as a report on the machine.


## What is still open

0. **The effect bar has no floor in absolute terms.** The false positive above
   is the argument for one: a shift should have to clear both K sigma AND the
   dispersion of the widest timescale there is evidence about. When reference
   runs from other sessions are supplied the tool already measures that
   timescale; it does not yet feed it back into the bar.
1. **The bimodality is not explained, only measured.** The candidates are
   alignment, page colouring and core placement, and separating them needs
   `perf` counters and a controlled allocator, not another statistic. Until
   then six of nine metrics are ungated.
2. **A real cross-commit series does not exist yet.** CI uploads one run per
   commit as an artifact; nothing accumulates them. That is a storage decision,
   not a statistics one.
3. **Bimodal metrics could be gated mode-conditionally** — detect the mode, then
   compare within it. That needs the modes to be identifiable from something
   other than the timing itself, which is item 1 again.
