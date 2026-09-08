# Subset selection, scored against something it did not optimise

ROADMAP P5. The roadmap put this item last and expected a null. It got a result
instead — and not the one the formulation is about.

Reproduce with:

```
./build/titans_subset data/raw/BTCUSDT-aggTrades-*.csv \
    --max-rows 300000 --points 20000 \
    --json results/subset/subset_28days.json
```

Full output in
[`results/subset/subset_28days.txt`](../results/subset/subset_28days.txt).


## The formulation, unchanged

Taken as published, monitoring as combinatorial subset selection:

$$\min_z \; -\sum_i r_i z_i \;+\; \lambda \sum_{i<j} S_{ij} z_i z_j \;+\; P\Big(\sum_i z_i - K\Big)^2$$

Mapped onto the slow lane: the items are the last *n* = 64 trades, the budget
*K* = 16 is how many the lane may keep, relevance is `|signed size|` normalised
by the window's largest, and similarity is a dense RBF over (position in the
window, signed size). λ traces the frontier: at 0 it is top-*K* by relevance; as
it grows the selection spreads out.

The penalty is kept because it is what makes this a QUBO — an unconstrained
binary quadratic form an annealer or a QPU could accept unchanged. It never
binds, because the annealer moves by **swapping** one selected item for one
unselected one, which preserves |z| = K exactly. That avoids the usual failure
where a penalty weight is tuned until the answer is feasible and the tuning is
then reported as a result; a test asserts that every feasible assignment carries
the same penalty and so it cannot rank anything.


## How this document was nearly wrong

The first version of this file said selection does not work. It was written from
**one day**, 2024-01-09, where recency wins and no interval excludes zero. It
would have been a clean, confident, well-argued null.

It was caught by an assertion in `scripts/reproduce.sh` that ran the same tool on
a *different* day and failed:

```
AssertionError: qubo/sa l=0.10 now beats recency; SUBSET.md says nothing does
```

Eight days later it was clear that 2024-01-09 is one of only two days in eight
where selection loses. The item was then re-run the way every other outcome
claim in this repository is made — across all 28 days, with a day as one
observation. That is the number below.

The lesson is not subtle and it is worth the space: a single day of tape is an
anecdote, this repository has a walk-forward protocol precisely because of that,
and P5 did not use it until an assertion forced the issue.


## Does selection beat recency

28 days, ~20,000 decision points each, ~16,000 scored per day after
calibration. Within a day, every method sees the same points. Across days, a day
is one observation, so the interval and the sign test are over days.

| method | mean diff vs recency | 95% CI over days | days better |
|---|---|---|---|
| random-16 | −0.0009 | [−0.0230, +0.0213] | 12 / 28 |
| **greedy λ=0** | **+0.0576** | **[+0.0323, +0.0827]** | **21 / 28** |
| greedy λ=0.1 | +0.0546 | [+0.0295, +0.0806] | 21 / 28 |
| greedy λ=0.5 | +0.0408 | [+0.0145, +0.0665] | 20 / 28 |
| greedy λ=2 | +0.0414 | [+0.0162, +0.0661] | 20 / 28 |
| qubo/SA λ=0 | +0.0553 | [+0.0302, +0.0812] | 21 / 28 |
| qubo/SA λ=0.1 | +0.0527 | [+0.0270, +0.0783] | 21 / 28 |
| qubo/SA λ=2 | +0.0425 | [+0.0167, +0.0674] | 20 / 28 |

Every selection method's interval excludes zero. **Selection is worth about
+0.05 informedness over taking the most recent sixteen trades.**

Picking the best of eleven methods is selection, and P3 built the machinery for
exactly that, so it is applied here rather than left to the reader:

| | |
|---|---|
| method | greedy λ=0 |
| methods compared | 11 |
| mean difference | +0.0576 |
| sign test over days | p = 0.00045 |
| bar for the best of eleven under the null | +0.0209 |
| observed minus that bar | +0.0367, z = +2.85 |
| **survives the correction** | **yes** |


## The QUBO is not the part that works

Read the λ column and the result changes character.

**λ = 0 wins.** At λ = 0 the redundancy term is switched off entirely and the
objective is `max Σ r_i z_i` subject to |z| = K — which is a sort. Greedy at
λ = 0 is "take the sixteen largest trades by absolute size", and it is the best
method in the table.

**Raising λ makes it worse**, monotonically: +0.0576 at λ = 0 down to +0.0414 at
λ = 2. The redundancy term — the thing the source formulation is *about*, the
thing whose gain over greedy is its headline — costs 28% of the effect.

**Random selection does nothing.** −0.0009, better on 12 of 28 days, an interval
straddling zero. That is the control that makes the rest interpretable: keeping
a subset is not what helps, keeping *the big trades* is.

So the honest summary is that a sophisticated method beat the baseline and the
sophisticated part is not why. The measured effect is entirely explained by a
one-line function that needs no annealer, no similarity kernel, no penalty
weight and no solver:

```cpp
// The whole of the measured gain.
std::partial_sort(idx.begin(), idx.begin() + K, idx.end(),
                  [&](auto a, auto b) { return std::fabs(q[a]) > std::fabs(q[b]); });
```

Greedy at λ = 0 takes 2.0 µs. The annealer takes 119 µs to reach the same
answer, and the exhaustive solver takes 23 ms.


## Do the solvers agree

Establishing that separately matters, because a claim about λ from a broken
solver is not a claim about λ. n = 20, K = 8, 200 real windows, where exhaustive
search is affordable.

| λ | exact | greedy | annealing | annealing matched exact |
|---|---|---|---|---|
| 0.0 | −258.61 | −258.61 | −258.61 | 151 / 200 |
| 0.5 | −255.27 | −255.24 | −255.27 | 188 / 200 |
| 1.0 | −252.24 | −252.15 | −252.24 | 197 / 200 |
| 2.0 | −246.23 | −246.05 | −246.23 | 196 / 200 |

Median solve: exhaustive 22.9 ms, annealing 43 µs, greedy 2.2 µs. Annealing
reaches the optimum more often as λ rises — at λ = 0 the objective is flat
across many near-tied top-K sets and the search has little to grip, which is
also why greedy and exact coincide exactly there.


## Does warm starting help

The window slides one trade at a time, so the previous solution is a
near-feasible start. The source formulation leaves this open; a reviewer asked
about it directly.

**No.** Cold and warm energies differ in the seventh significant figure, solve
time is unchanged, and **the sign of the difference is not stable between
runs** — one run had warm ahead, the next had it behind. There is nothing for a
good start to save, because the annealer already reaches the optimum from a
random one at this size. The tool now reports the difference against its own
scale rather than announcing a direction for it.


## What the solve costs

The annealer's median solve is 119 µs per decision. This tape stamps to the
millisecond, so that is **eight times below the resolution the data can
express** and its cost cannot be measured here. The greedy solver, which
produces the best result, is another order down at 2 µs.

The exhaustive solver is the one whose cost is measurable: **23 ms** on a much
smaller instance (n = 20 against the n = 64 used above), and the same rule
delivered 23 ms late loses about a fifth of its value on this tape. Solving the
objective exactly, at a size where the annealer is only an approximation, would
spend real freshness — but nothing here needs it to, because the winning
configuration is a sort.


## Threats to these numbers

- **One relevance, one similarity, one budget.** `|signed size|` and an RBF on
  (position, size), n = 64, K = 16. The source reports its diversity gain
  growing with n; the external outcome here was measured at one n.
- **The downstream rule is a sum.** Subsetting a sum can only lose information
  unless what is dropped is noise — which turns out to be exactly what happens:
  dropping the small trades keeps the sum's informative part. That also means
  this result may say more about the flow rule than about selection.
- **λ = 0 winning is a boundary result.** The best point sits at the edge of the
  swept range, so "the redundancy term hurts" is measured over [0, 2] and
  nothing here rules out a negative λ, which would be a different claim.
- **Per-day intervals are wide.** The within-day comparisons resolve on only
  some days; the effect is established over days, not within them.
- **No quantum backend.** The roadmap lists it under explicitly-not-doing. The
  matrix here would be accepted by one unchanged if that ever became
  interesting, which is why the penalty term is still in it.


## What is still open

1. **Why do the big trades carry it?** The measured gain is entirely from
   keeping large `|signed size|`, and this document does not explain the
   mechanism. Large trades being more informative about imminent adverse
   selection is plausible and unmeasured here.
2. **A consumer that is not a sum.** P4 established that a model's context is
   expensive and that the most recent K is what it gets by default. Choosing
   *which* K trades go into a prompt at a fixed token budget is the same problem
   with a downstream consumer where redundancy might actually matter — and it is
   the one place the λ term has a reason to earn its keep.
3. **Larger n.** The source's diversity gain grows with n. Whether λ > 0 stops
   hurting somewhere above n = 64 is unmeasured, and it is where the solve cost
   would finally become measurable too.
