# Tick to trade

ROADMAP P1. What it asked for, what came out, and the one place where the
standard technique does not work.

Reproduce with:

```
./build/titans_ticktotrade --data data/raw/BTCUSDT-aggTrades-2024-01-15.csv \
    --rows 400000 --speed 2000 --core 2 --slow-core 4 --sweep \
    --json results/latency/ticktotrade_2024-01-15.json
```

Full output in [`results/latency/ticktotrade_2024-01-15.txt`](../results/latency/ticktotrade_2024-01-15.txt).


## The gap this filled

Every latency figure this repository had published was a **per-operation** cost
from amortized batch timing: `try_push` 1.21 ns, `update_level` 24.03 ns. All
correct, and none of them a system. There was no number anywhere for how long a
trade takes to become an order, and batch timing cannot produce one — it divides
total time by a repetition count, so a stall is invisible to it by construction.

`titans_ticktotrade` runs the real path over the real tape: ingest, book,
signal, risk, order. The strategy logic is inside the measured region, which is
deliberate. Databento names the opposite as the standard pitfall — "tick-to-trade
latency is usually measured without any strategy or model logic. Hence, it can
be misleading since some low latency trading strategies compute a very large
number of features."


## Why a stamp chain and not five scopes

The repository already had an instrument for this shape: `BudgetScope` in
`lanes/latency_budget.hpp`, an RAII pair that times its own lifetime. Nesting
five of them is the obvious implementation and it is wrong twice.

**It costs too much.** A scope reads the TSC twice, so five scopes read it ten
times. Measured here at **23.3 ns per boundary**, ten reads is 233 ns of probe on
a path whose real cost is 242 ns. The measurement would have been half the
measurement. A chain of boundary stamps needs six reads for five stages.

**It loses time.** Scopes measure disjoint intervals and the space between one
scope's end and the next scope's start belongs to no stage. The stages then sum
to less than the whole and nothing in the output says where the difference went.
A chain partitions exactly: stage *i* is `stamps[i+1] - stamps[i]`, and the
stages telescope to `stamps[N] - stamps[0]`.

`tests/test_pipeline_latency.cpp` runs both instruments over the same work and
requires the chain's sum to be exact while the scope version's is strictly
short. The same test requires every stamp to advance, because a boundary that
was never marked still telescopes and the sum alone would not notice.


## What the probe costs

A hardware tap keeps the instrument off the critical path. Without one the
honest substitute is to measure what the probe costs rather than assume it is
small. `StageTrace` is templated on whether it stamps at all, so a probed and an
unprobed fast path are both compiled with no branch in either, and one unpaced
burst of each gives the difference directly.

| | ns/tick |
|---|---|
| without stamps | 242.3 |
| with stamps | 382.0 |
| **probe** | **139.7** over 6 boundaries, 23.3 each |

Every per-stage figure below carries roughly one boundary read. That is 58% on
top of the real path, which is why the probe is a printed number in the header
of every run rather than a footnote.


## Per stage

400,000 trades, 658.9 minutes of tape at 2000×, Ryzen 9 5950X, TSC 3.40 GHz,
single-shot floor 20 ns.

| stage | mean | p50 | p99 | p99.9 | share |
|---|---|---|---|---|---|
| ingest | under floor | under floor | 110.3 ns | 210.3 ns | 6.6% |
| book | 170.0 ns | 120.3 ns | 780.9 ns | 1609.2 ns | 40.4% |
| signal | 104.6 ns | 100.3 ns | 210.3 ns | 580.9 ns | 24.9% |
| risk | 90.8 ns | 90.3 ns | 130.3 ns | 290.3 ns | 21.6% |
| order | under floor | under floor | under floor | under floor | 6.5% |
| **sum** | **420.3 ns** | | | | |

The stage means sum to 420.3 ns and the end-to-end service mean is 420.3 ns.
That equality is the chain doing its job, and it is asserted in the tests.

**"under floor" is a refusal, not a small number.** Two of the five stages cost
less than three times the clock read used to measure them, so a distribution of
them would be a distribution of the probe. The README's benchmark table refuses
per-operation p99s for exactly this reason; the same rule applies per cell here,
which is why `ingest` shows no p50 but does show a p99 — it is normally free and
occasionally pays a cache miss.

**The percentile columns do not add.** Only the means do. The tick holding the
book stage's p99 is rarely the tick holding the risk stage's, so reading the
percentile columns as a decomposition would overstate the tail by summing
independent worst cases. This is the standard way a per-stage table lies and the
report says so in place.

### The book stage, and what the table does not settle

At 40.4% the book is the largest stage, and attribution stops at the stage
boundary — it says *where*, not *why*. Two candidates, in order of how easy they
are to check:

1. `L2OrderBook::update_level` calls `now_ns()` unconditionally, and the book
   stage calls it twice (one level in, one trimmed out to keep depth bounded).
2. Map churn: unlike the microbenchmark, which updates a handful of hot keys,
   the real tape walks the price grid and inserts and erases across ~128 live
   levels.

Arithmetic on the table favours the first being material: the risk stage makes
one such clock call plus several map lookups and float comparisons, and comes to
about 67 ns net of probe. That is an inference from two rows, not a controlled
measurement, and it is written here as the next thing to check rather than as a
result. It is the same shape as the `EventBus::publish` finding already in the
README, which *was* measured both ways.


## End to end, and where coordinated omission stops working

| | mean | p50 | p99 | p99.9 |
|---|---|---|---|---|
| service | 420.4 ns | 371.5 ns | 1100.9 ns | 2305.6 ns |
| corrected | 441.7 ns | 371.5 ns | 1364.4 ns | 4348.0 ns |
| response | 9152.1 ns | 809.1 ns | **78909.6 ns** | 378285.5 ns |
| queueing | 8731.7 ns | 333.8 ns | 78909.6 ns | 378285.5 ns |
| *host floor (control)* | *603.6 ns* | *under floor* | *3595.1 ns* | *16489.5 ns* |

- **service** is handler work: last stamp minus first stamp. A closed-loop
  benchmark reports this, and a stall cannot move it, because a frozen system
  takes no samples and the samples it fails to take are the slow ones.
- **response** measures from the moment each tick was *due*. Exact, because a
  replay knows its own schedule.
- **corrected** is `service` through `Histogram::record_corrected`, Gil Tene's
  correction, against the median inter-arrival interval.
- **host floor** is the control: the same schedule with no pipeline attached, so
  it is the machine's own lateness. Response figures at or below it measure
  nothing about this system. Here the tail clears it by 21.9×, so the queueing is
  real.

The headline is the gap between rows two and three. **The correction recovers
1,364 ns against an exact 78,910 ns — it closes under 2% of the gap while
synthesising 8,741 samples.**

The reason is structural, and it is the most useful thing this item produced.
Tene's correction infers omission from a *service* time longer than the expected
interval: it assumes the handler blocking is what suppressed sampling. Here no
single call is slow. The handler runs in 420 ns against a 500 ns median
interval, and the queue builds because arrivals bunch — 43% of these trades
share a millisecond with the one before them. A correction that looks only at
service times cannot see a tail caused by burst arrivals, and it reports success
either way.

So: **the correction is not a cheaper version of knowing your arrival times. It
is a substitute, and on a bursty feed it is a poor one.** Where the exact answer
is computable the substitute should be scored against it rather than trusted,
which is what `test_correction_approximates_the_exact_response_time` does — on
uniform arrivals with an injected stall the correction lands within 5%, and the
run above shows what happens to that agreement when arrivals stop being uniform.


## The millisecond grid

Binance records `transact_time` to the millisecond and trades cluster inside
one. Replayed against the raw stamps, every trade after the first in a cluster
is due at the same instant and is late before the handler starts, so the
response tail would be measuring the exchange's timestamp resolution. In this
file that is **43.0% of trades**.

Each millisecond's trades are spread evenly across it. The recorded order is
preserved, no information the feed does not carry is invented, and among the
arrival times consistent with the stamp it is the maximum-entropy choice. It is
also the *favourable* one: real arrivals inside a busy millisecond are burstier
than uniform, so the queueing reported here is a lower bound.

`titans_lanes` met the same problem and answered it differently — it reports a
lag distribution and refuses a late/on-time count — because there the quantity
of interest was the outcome, not the schedule.


## Saturation

Accelerating the tape by *S* asks the pipeline to handle a market *S* times
busier with the same burst structure.

| speed | interval | service p99 | corrected p99 | response p99 | late | synthesised |
|---|---|---|---|---|---|---|
| 125× | 8000 ns | 1844 ns | 1854 ns | 36593 ns | 15.9% | 4 |
| 250× | 4000 ns | 1666 ns | 1666 ns | 43370 ns | 25.9% | 7 |
| 500× | 2000 ns | 1496 ns | 1534 ns | 51201 ns | 35.5% | 382 |
| 1000× | 1000 ns | 1308 ns | 1430 ns | 60839 ns | 46.1% | 1673 |
| 2000× | 500 ns | 1162 ns | 1487 ns | 78307 ns | 54.4% | 10319 |
| 8000× | 125 ns | 913 ns | 1553 ns | 142760 ns | 64.1% | 630510 |
| 32000× | 31 ns | 682 ns | 828 ns | 1190275 ns | 73.3% | 4164455 |
| 64000× | 16 ns | 621 ns | 677 ns | 2168517 ns | 80.3% | 8868796 |

Read down the columns.

**Service p99 falls as load rises**, from 1844 ns to 621 ns across a 512×
increase in arrival rate — monotonically, at every step. Not a fluke: at low
speed the handler spins waiting for the next due time and pays cold-ish caches
and a mispredicted loop exit; at high speed it runs back to back. **The naive
number improves as the system degrades.** That is the closed-loop lie in its
purest form, and a report containing only that column would show a system with
no saturation point at all.

**Response p99 rises monotonically**, 36.6 µs to 2.17 ms, a 59× spread over the
same range. Both columns are monotone over eight operating points and they point
in opposite directions.

**The correction tracks neither.** At 64000× it synthesises 8.9 million samples
and still reports 677 ns against a real 2.17 ms — off by a factor of 3,200. Its
value is set by whether any single service sample happened to cross the
interval, not by how deep the queue got.

The pipeline is not saturated on this tape in real time. At 420 ns of handler
per trade, a millisecond would have to contain about **2,400 trades** before the
handler alone filled it, and the busiest millisecond in these 400,000 trades
holds 569. The sweep exists to find where that stops being true, and to show
what the two candidate metrics do on the way there.


## Run to run

The same command, four times, on the same host. The first two ran while other
work was on the machine; the last two were the only thing running.

| | run 1 | run 2 (contended) | run 3 | run 4 (published) |
|---|---|---|---|---|
| book stage mean | 180.7 ns | 176.0 ns | 169.1 ns | 170.0 ns |
| service p99 | 1143.3 ns | 1152.7 ns | 1020.9 ns | 1100.9 ns |
| response p99 | 80.1 µs | 81.9 µs | 78.9 µs | 78.9 µs |
| corrected p99 | 1355 ns | 1581 ns | 1251 ns | 1364 ns |
| **125× sweep row, response p99** | **43.1 µs** | **669.8 µs** | **50.0 µs** | **36.6 µs** |

The headline figures move by under 13% across all four, contention included, and
the response p99 by under 4%. The 125× sweep row moves by **18×**, and it is the
only row that does.

That is not a coincidence and it is the argument for how a regression gate has
to be built. A lightly loaded pass takes the most wall time — 316 seconds at
125× against 5 at 8000× — and produces the fewest genuine queueing events, so a
single scheduler interruption owns its tail outright. A fixed-percentage
threshold would fire on that row constantly and never fire on the rows that
matter. ROADMAP P2 gates on the harness's own measured noise for exactly this
reason; the table above is the first direct evidence in this repository that the
noise is not uniform across operating points.

The numbers reported everywhere in this document are from run 4, which is the
committed artifact.


## Threats to these numbers

- **No `isolcpus`/`nohz_full`.** The kernel schedules other work on the measured
  core. The host-floor control quantifies it — p99 3.6 µs, p99.9 16.5 µs — and
  the response tail clears the p99 floor by 21.9×, so the headline stands. The
  p99.9 figures are upper bounds, and the run-to-run table above shows what
  happens to a lightly loaded pass when the host is not quiet.
- **SMT and turbo are on.** Both are reported by the machine fingerprint in
  every run and in the JSON.
- **The book is synthetic.** aggTrades carries no depth, so the book stage is
  fed levels derived from trades and trimmed to 64 per side. The timing is real
  — real structure, real level counts, real cache behaviour — the contents are
  not a real book, and nothing here depends on them being one.
- **Risk limits were raised.** `check_order` returns at the first failure, so a
  tripped limit turns the risk stage into a measurement of its reject path. The
  rate limiter counts against the wall clock and would reject nearly everything
  at 2000×; the price-deviation bound would trip once the tape moved past it.
  Both were raised so every branch runs, and the run says so in its own output.
- **One host, one day of tape.** Comparing across commits is P2's job, and P2
  is explicitly gated on building the regression detector against the harness's
  own measured noise rather than a fixed percentage.


## What is still open

1. Explain the book stage rather than only locate it. The two candidates are
   named above and the first is cheap to settle.
2. `service` and `response` disagree by 72× on this workload. Only `response`
   should be quoted as tick-to-trade, and nothing currently prevents a future
   reader from quoting the smaller one.
3. The saturation point is bracketed, not measured. The sweep shows the response
   tail degrading monotonically from 125× upward; it does not find the knee,
   because the low end costs wall time proportional to `1/speed` — and that is
   also the regime where the host noise is worst, so pushing lower would need a
   quieter machine, not just more patience.
