# Context length, priced in staleness

ROADMAP P4. Prefill cost scales with context length, so a richer prompt buys
delay. "More context is better" and "fresher advice is better" are in direct
opposition and neither survives being put on the same axis as the other.

Reproduce with:

```
# the reference curve, no model needed
./build/titans_context_cost --data data/raw/BTCUSDT-aggTrades-2024-01-09.csv \
    --max-rows 300000 --skip-model

# the full measurement, against a live model
./build/titans_context_cost --data data/raw/BTCUSDT-aggTrades-2024-01-09.csv \
    --max-rows 300000 --model llama3.1:8b --contexts 8,32,128,512,1024 \
    --points 400 --json results/context/context_cost_llama31_8b.json
```

Full output in
[`results/context/context_cost_llama31_8b.txt`](../results/context/context_cost_llama31_8b.txt).


## Part A: what a delay costs on its own

The flow heuristic needs no context at all, so scoring it at a range of delivery
delays isolates the price of arriving late with the value of context set to
zero. 300,000 real trades, every decision scored against whichever trade is
current when the answer would have landed.

| delay | informedness | action rate |
|---|---|---|
| 0 ms | +0.0759 | 4.32% |
| 25 ms | +0.0568 | 3.55% |
| 50 ms | +0.0416 | 3.27% |
| 100 ms | +0.0409 | 3.02% |
| **250 ms** | **+0.0195** | 2.57% |
| 500 ms | +0.0140 | 2.44% |
| 1000 ms | +0.0086 | 2.50% |
| 4000 ms | +0.0009 | 2.36% |

**Half the value is gone by 250 ms**, against a signal horizon of 1000 ms.
By four seconds there is nothing left to deliver.

This is P0's finding with a number on it. P0 established that a threshold
crossing is a moment and that a stale decision misses it; this says how fast.

One implementation note that matters more than it looks. `run_policy_delivered`
is a second evaluator for the same policy, and two evaluators that disagree by a
little would produce a "cost of delay" that is really the difference between two
pieces of code. At zero delay it reproduces `run_policy_over_day` counter for
counter, and a test asserts it.


## Part B: what context costs, on a real model

llama3.1:8b on an RTX 3090, 400 decision points, the same points in every arm.
The task is deliberately the one the heuristic already solves — judge whether
recent order flow is unusually one-sided, and which way — so the two are
comparable rather than merely both present.

| context | tokens | prefill | total | J at context (95% CI) | J delivered (95% CI) |
|---|---|---|---|---|---|
| 8 trades | 241 | 30 ms | 413 ms | +0.0650 [−0.0265, +0.1581] | **+0.2361** [+0.1104, +0.3604] |
| 32 | 361 | 83 ms | 516 ms | +0.1350 [+0.0515, +0.2158] | +0.1202 [−0.0151, +0.2597] |
| 128 | 841 | 250 ms | 708 ms | +0.1200 [+0.0262, +0.2137] | +0.0497 [−0.0856, +0.1863] |
| 512 | 2761 | 963 ms | 1608 ms | −0.0250 [−0.1238, +0.0677] | −0.0609 [−0.2380, +0.1087] |
| 1024 | 5321 | 1988 ms | 2887 ms | +0.1100 [+0.0116, +0.2065] | +0.0559 [−0.1627, +0.2644] |

Prompt size fits **201 tokens of fixed overhead plus 5.00 tokens per trade**,
and the marginal prefill cost is **0.385 ms per token — about 1.9 ms for every
trade of context added**.

### The paired test is the result

Comparing two of those intervals by eye is the wrong test: every arm answered
the *same* 400 points, so resampling them jointly removes the variance the
points themselves contribute. Differences against the smallest arm:

| context | at context, difference | delivered, difference |
|---|---|---|
| 32 | +0.0700 [−0.0354, +0.1719] | −0.1160 [−0.2520, +0.0191] |
| 128 | +0.0550 [−0.0732, +0.1791] | **−0.1864 [−0.3419, −0.0302]** |
| 512 | −0.0900 [−0.2198, +0.0350] | **−0.2970 [−0.5044, −0.0964]** |
| 1024 | +0.0450 [−0.0918, +0.1721] | −0.1802 [−0.4316, +0.0684] |

Read the two columns apart, because they say different things and only together
do they say the interesting one.

**At context, nothing is resolved.** Every interval contains zero. Across 128×
more context the model's judgement did not measurably improve — or worsen. More
context bought nothing.

**Delivered, the middle arms are resolved and negative.** Going from 8 to 512
trades of context costs 0.30 informedness and the interval excludes zero; at
128 it costs 0.19, also resolved.

So the decomposition is unambiguous on this hardware: **more context did not
make the model better, and it did make the answer later, and the delay did all
the damage.**

### Four runs, and what is stable across them

The model runs at temperature 0, so given the same decision points its answers
are identical — the at-context column is reproduced exactly between runs that
share a seed and a sample size. **The only thing that varies run to run is the
measured latency**, which moves where the answer lands and therefore what it is
scored against. That makes the delivered column the noisy one by construction,
and it is worth seeing how noisy:

| paired vs 8 trades, delivered | 200 pts | 200 pts | 400 pts | 400 pts *(artifact)* |
|---|---|---|---|---|
| 32 | −0.0511 | −0.0040 | −0.0786 | −0.1160 |
| 128 | −0.1098 | −0.0795 | −0.1166 | **−0.1864 \*** |
| **512** | **−0.4405 \*** | **−0.3331 \*** | **−0.3271 \*** | **−0.2970 \*** |
| 1024 | **−0.4929 \*** | −0.2397 | **−0.2829 \*** | −0.1802 |

*\* excludes zero.*

**The 512-trade arm is resolved negative in all four runs**, and that is the
claim this document makes. The 1024 arm resolves in two of four: it is the
noisiest because its answer lands furthest away, so the trade it is scored
against is least related to the one its context ended on. No at-context
difference is resolved in any run, at any context.

The larger sample was run because the intervals at 200 points were marginal, not
because of what they said, and all four runs are tabulated here rather than the
one that reads best. That is the discipline `docs/SELECTION.md` built machinery
for one item earlier.

### The fixed cost already exceeds the half-life

The sharpest number here is not in the sweep. At the smallest context the model
spends 30 ms on prefill and 383 ms on everything else — sampling, scheduling,
the HTTP round trip. **That floor alone is more than the 250 ms it takes for
half the signal's value to disappear**, before context has bought anything at
all.

For an 8B model on this hardware, the context-length trade-off is not a curve
with an interior optimum to find. The optimum is at the left edge, and even
there the model is spending more than half the signal to answer.


## Two ways this could have lied

**Silent truncation.** A serving stack asked for more context than it holds does
not complain — it truncates, answers normally, and the sweep then measures the
truncation. Prefill plateaus, the answer stops changing, and the curve reads
"more context does not help": a fact about a config file wearing the costume of
a finding. `check_context_growth` fits the fixed prompt overhead from the two
smallest arms and refuses any arm whose prefilled tokens fall short of the line.
It also states what it will miss — a tenth of a prompt lost is inside the
tolerance, so it finds where truncation becomes large, not where it begins.

The check itself shipped broken and was caught by its own test: the first
version compared raw token counts, which grow 4× while content grows 16×,
because a 201-token system prompt is most of the smallest arm. It called a
perfectly healthy sweep truncated.

**A degenerate model.** A model that answers the same thing regardless of input
draws a flat line at exactly chance, and reading that as "context does not
matter" would be completely wrong — the failure `titans_llm_experiment` already
documented. Here the guard was wrong in a subtler way first: it measured the
share of `one_sided` answers alone, saw 99–100%, and called four of five arms
degenerate. But the decision rule is `one_sided && direction == the side of the
trade being judged`, so a model that always says `one_sided: true` and varies
`direction` produces decisions that vary. Degeneracy has to be judged on the
model's whole answer, and on that measure the most common answer takes 56–66%
of the arm and nothing is degenerate.

That correction cuts both ways, and the wrong version was dangerous in the other
direction too. A model answering `{true, "buy"}` to *everything* would produce
decisions that still vary — because the rule reads the target trade's own side —
and would score a non-zero informedness built entirely on the aggressor-side
leak the label audit already documents. Judging degeneracy on the score rather
than on the answer would have credited the model for it.


## Threats to these numbers

- **One model, one host, one day.** llama3.1:8b on an RTX 3090 over 300,000
  trades of 2024-01-09. The mechanism generalises; the specific 390 ms floor is
  this stack's.
- **The label threshold is 2 bps, not the repository's usual 5.** At 5 bps this
  tape is 1.5% toxic, so a few hundred model calls would contain a handful of
  positives and the true-positive rate would be noise. At 2 bps it is 11%. Both
  parts of the program use the same threshold, which is what makes them
  comparable to each other.
- **Points are stratified on the at-context label**, which is legitimate for
  informedness because TPR and FPR are class-conditional. It does mean the
  *delivered* sample is enriched in a way a full-tape number is not, so the
  absolute levels in Part B are not comparable with Part A. Only the trend
  across arms is, and that is the claim being made.
- **400 points is not many.** The individual arm intervals are wide, which is
  exactly why the headline rests on the paired comparison instead, and why the
  four-run table above is there rather than a single set of numbers.
- **Ollama, not a production serving stack.** Chunked prefill, prefix caching
  and disaggregated prefill/decode all exist to attack precisely this cost, and
  none of them are in play here. A stack that used them would move the floor;
  it would not remove it.


## What is still open

1. **Prefix caching is the obvious next measurement.** Consecutive decision
   points share almost all of their context, so a cache should collapse the
   prefill term and leave the fixed floor. That would separate "context is
   expensive" from "this serving stack recomputes it".
2. **The floor is unattributed.** 390 ms of non-prefill cost on a 5 GB model is
   a lot, and nothing here says how much is sampling, scheduling, or HTTP.
3. **One model.** Whether a more capable model's at-context accuracy would rise
   with context — making the trade a real trade rather than a one-sided loss —
   is exactly the question this instrument now exists to answer, and it needs a
   model this hardware cannot host.
