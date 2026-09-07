# Two ways the walk-forward number could be optimistic

ROADMAP P3. The 28-day walk-forward was sound on its own terms and had two
holes: nothing separated the last trades of a training day from the first of
the test day, and nothing recorded how many configurations had been tried to
produce the reported number. Both are now measured. One of them turned out not
to matter, and saying so with a number is the point.

Reproduce with:

```
./build/titans_walkforward data/raw/BTCUSDT-aggTrades-*.csv \
    --embargo-ms 0,1000,60000,3600000,21600000 \
    --json results/walkforward/btcusdt_embargo_sweep.json
```

Full output in
[`results/walkforward/btcusdt_embargo_sweep.txt`](../results/walkforward/btcusdt_embargo_sweep.txt).


## Hole one: the fold boundary

Training ends when day *t−1* ends; testing starts when day *t* begins. Those
two instants are adjacent, the toxic-flow label looks 1000 ms into the future,
and the market state does not reset at midnight. The standard remedy is
purging and embargoing (López de Prado): remove the training observations whose
information overlaps the test period, and a margin beyond them for serial
correlation.

Three decisions worth stating, because each could have been made a different
way and the wrong one is invisible in the output.

**The embargo removes training data, never test data.** Dropping the head of
the test day would make the informedness numbers incomparable across embargo
settings — a different test set every time, and a sensitivity curve measuring
which trades were dropped rather than what the embargo does.

**The cut is on time, not on a count of trades.** Trade density on this tape
varies by two orders of magnitude within a session. "The last 10,000 trades" is
twenty minutes on a quiet morning and ninety seconds during a move, so a
count-based embargo would be a different embargo every day and a sweep over it
would be measuring volatility. `split_by_embargo` cuts on the sample's own
timestamp, and `test_embargo_splits_on_time_not_on_count` pins the boundary
including which side an exactly-on-the-cut sample falls.

**Held-back samples come back.** A sample inside the embargo of day *t−1* is
withheld from the threshold applied to day *t*, and released once day *t* is
done and it is no longer adjacent to anything being tested. Discarding it
permanently would shrink the training set for a reason that expires.

One implementation detail that would have been a silent bug: flow samples do
not correspond one-to-one with trades. They begin only once the 50-trade window
is warm, so index arithmetic to recover a sample's timestamp is off by the
window length. `DayRun` now carries `abs_net_ms` alongside `abs_net` rather than
inviting the caller to reconstruct it.

### What it changed: nothing, and here is how much nothing

| embargo | folds | out-of-sample informedness | 95% CI |
|---|---|---|---|
| 0 ms *(headline)* | 27 | +0.1676 | [+0.1385, +0.1966] |
| 1,000 ms | 27 | +0.1676 | [+0.1385, +0.1966] |
| 60,000 ms | 27 | +0.1676 | [+0.1385, +0.1967] |
| 3,600,000 ms | 27 | +0.1676 | [+0.1385, +0.1965] |
| 21,600,000 ms | 27 | +0.1682 | [+0.1390, +0.1973] |

From no embargo to six hours, the answer moves by **+0.0006** — about one
fiftieth of the interval's half-width. A six-hour embargo discards a quarter of
every training day, and the threshold does not notice, because it is a 0.95
quantile over millions of samples and a quantile that far into the bulk does not
turn on its last few thousand points.

So the roadmap's guess was right: the adjacency is immaterial. The difference
between this and the guess is that "immaterial" now has a number attached, and
anyone who changes the labelling horizon or the calibration size can re-run the
sweep and find out whether it still holds.


## Hole two: the search behind the number

The walk-forward had run a drift-window sweep at five settings and a TTL sweep
at four. Nothing was selected on either — all points were reported and the
headline came from a fixed configuration — so nothing published was wrong. But
that is a discipline, not a mechanism, and it holds exactly as long as whoever
is holding it remembers.

### What selection actually costs

Simulate it rather than argue about it. Twenty configurations, each scored on
27 folds of pure noise with no effect whatsoever, keep the best, ask whether it
looks significant at 5%:

```
best-of-20 on a pure null: 65.2% called significant uncorrected,
                            0.8% after deflation
```

**Two runs in three.** That is the whole problem in one line, and it is a test
(`test_correction_controls_the_best_of_n_false_discovery_rate`) rather than a
citation, so it fails if the correction ever stops working.

### The correction

Three numbers, reported together because they answer different questions:

- **Expected best-of-N under the null.** `expected_max_z`, the term the Deflated
  Sharpe Ratio (Bailey & López de Prado, 2014) subtracts. With ten trials the
  best result under a pure null already sits at +1.54 standard errors, which is
  comfortably "significant" by the usual threshold. This is the bar a sweep's
  winner has to clear.
- **A Šidák-adjusted interval.** For 95% family-wise coverage over five trials
  each interval must be built at 98.98%, which is why the deflated interval is
  visibly wider rather than a rounding difference.
- **A family-wise p-value**, beside the raw one.

On the 28-day result with the five-point embargo sweep:

| | value |
|---|---|
| configurations evaluated | 5 |
| bar the best point must clear | +0.0177 |
| observed | +0.1676 |
| deflated z | +10.10 (p = 2.7e-24) |
| deflated interval | [+0.1292, +0.2052] |
| raw 95% interval | [+0.1385, +0.1966] |
| family-wise sign-flip p | 0.00025 (raw 5e-05) |

The effect survives comfortably. It would have to: five highly correlated
trials is close to no search at all, and Šidák treats them as independent, which
is the conservative direction.

### What the tool now makes impossible

The trial count is **derived, not declared**. `--embargo-ms 0,1000,60000` sets
trials to 3 without anyone remembering to say so, because a counter a human has
to increment is a counter that stays at 1. `--trials N` multiplies in
configurations evaluated outside the program, so a caller claiming a smaller
number has to write it down.

When a sweep runs, the tool prints the headline as the **first** value and names
the best one separately, with the gap between them and the bar it would have to
clear. Reporting the best point as the result now requires deleting output that
says it is the best point.

When only one configuration runs, the tool says the number is uncorrected and
that this is only sound because nothing was selected — rather than saying
nothing, which reads the same as having checked.

And the result file records `trials`, the whole `embargo_sweep`, and the
deflation block. A result whose trial count was never written down cannot be
corrected later by anyone reading it, and the number nobody wrote down is
always 1.


## A numerical detail that would have printed a lie

The deflated statistic reaches z = 10.10, and `1 − Φ(10.10)` is exactly zero in
double precision — the CDF has already rounded to 1. The first version printed
`p 0`, which claims infinite evidence and is the same failure the sign-flip test
avoids by printing `p < 5e-05` instead of `p = 0`. The tail is now computed
directly with `erfc`, and `test_the_far_tail_does_not_underflow_to_zero`
asserts both that the direct form stays positive and that the cancelling form
does not — so the test fails if it ever stops guarding anything.


## Threats to these numbers

- **Šidák assumes independent trials.** An embargo sweep's points are almost
  perfectly correlated, so the correction is far more conservative than
  necessary. That is the safe direction, and the trial count is an explicit
  argument so a caller claiming fewer effective trials has to say so out loud.
- **The embargo null is specific to this calibration.** A 0.95 quantile over
  millions of samples is insensitive by construction. A policy fitted on
  hundreds of samples, or one whose parameter lives in the tail, could behave
  completely differently, and the sweep is the way to find out rather than a
  result that transfers.
- **Deflation is applied to the aggregate, not per fold.** The folds are not
  independent — they share an expanding training window — which the bootstrap
  already accounts for only approximately.
- **Three of 28 days fail the label audit** and are kept in the aggregate rather
  than dropped, as before. That is unchanged by this item and is reported in
  the same place.


## What is still open

1. **Trials across sessions are not tracked.** `--trials` covers configurations
   the caller knows about; nothing accumulates what this repository has
   collectively tried over its history. Doing it properly means a registry, not
   a flag.
2. **No combinatorial purged cross-validation.** Walk-forward is the right
   realism protocol and comparatively weak at false-discovery prevention; CPCV
   trades realism for many more test paths. Both would be better than either.
3. **The effective number of trials is not estimated.** Correlated sweeps could
   support a much smaller effective count than their nominal one, which would
   make the correction less punitive without making it wrong.
