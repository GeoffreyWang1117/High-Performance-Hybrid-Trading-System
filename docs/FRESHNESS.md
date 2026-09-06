# Advisory age as a contract

How a wrong knob was found, what replaced it, and what the replacement cost.

## The knob that was wrong

`Advisory::valid_until` is a wall-clock expiry, and the original design treated
it as the load-bearing defence against acting on stale model output. It is not.

Measured over 100 000 BTCUSDT trades with a 60-second TTL — wide enough that
almost nothing expired — the age of the advisory when the fast lane looked at it
was **p50 82 ms, p99 1543 ms, max 4418 ms**, against a **1000 ms** prediction
horizon. One read in a hundred acted on advice older than the entire horizon it
predicted over, while sitting comfortably inside its declared lifetime.

Raising the TTL from 250 ms to 60 s cut the rejection rate from 34.4% to 10.9%
and moved the outcome not at all. The knob controlled something real and
irrelevant.

## Four hypotheses

The same policy scores +0.16 informedness evaluated without the lane
(`titans_walkforward`) and ~0 through it. Four explanations were tested:

| Hypothesis | How it was tested | Outcome |
|---|---|---|
| Advisories expire before use | `--ttl-ms` 250 → 60 000 | rejected — rejection rate moved, outcome did not |
| The bridge sheds observations | `LaneBridge::dropped()` | rejected — 0 of 99 972 |
| The advice is too old, so gate it | `--max-age-frac` 1.0, 0.5, 0.25, 0.10, 0.05 | rejected — acted-on p99 age tracked the gate exactly (897 → 478 → 240 → 98 → 50 ms) and informedness stayed unresolved at every setting |
| The advice is *misaligned* rather than stale | compare the delivered decision against the ideal one, trade by trade | **confirmed** |

The third row is the interesting failure. The gate worked perfectly and bought
nothing, which is only possible if age was not the mechanism.

## What the instrument showed

`titans_lanes` now computes the reference decisions offline — the same
`FlowPolicy`, over the same trades, with no bridge, thread, or advisory — and
compares them to what was actually delivered, trade by trade:

```
  ideal sizes down 3320, lane sizes down 3211
  both size down          1332  (40.1% of ideal's actions survived delivery)
  ideal only, lane no     1988  the lane missed these
  lane only, ideal no     1879  the lane acted where it should not
```

Nearly the right *number* of actions, on the wrong *trades*.

A threshold crossing is a moment, not a state. The median advisory here is three
trades old; three trades is enough to move the action onto its neighbours. That
is why no expiry rule and no age gate helped: both can suppress an action,
neither can move it back to where it belonged.

## The fix

Ship the parameter, not the decision.

The slow lane already computes a calibrated warn threshold — a 5000-sample
quantile of |net flow|. That quantity moves slowly, so it survives a trip
through a lane. The decision derived from it moves at every trade, so it does
not. `Advisory::parameter` carries the threshold, and in
`--advisory parameter` mode the fast lane evaluates the same `FlowPolicy`
against its own current window.

Same rule, same trades, same threshold to four significant figures, same lane,
same advisory ages:

| `--advisory` | agreement with ideal | informedness | run-to-run sd | verdict |
|---|---|---|---|---|
| `decision` | 40.1% | median −0.0102 | 0.0255 | NOT RESOLVED |
| `parameter` | 89.8% | median +0.2426 | 0.0028 | resolved |

The standard deviation matters as much as the median. Shipping a decision makes
the outcome depend on when the slow lane happened to wake, a 9× run-to-run
spread; shipping a parameter makes it nearly deterministic.

## The contract that came out of it

An advisory declares `signal_horizon_ns`, the horizon of **what it carries** —
and the two payloads do not share one:

- a **decision** is a threshold crossing, so it inherits the signal's horizon:
  1000 ms.
- a **parameter** is a distributional summary, so its horizon is the stretch of
  market it was estimated over: 789 s here, measured from the calibration
  sample's own timestamps rather than configured.

A consumer declares what fraction of that horizon it will tolerate
(`FreshnessPolicy::max_age_fraction`) and what p99 it holds the producer to
(`slo_p99_fraction`, default 1.0 — advice should not be older than the horizon
it predicts over). Under identical delivery at an identical p99 age of ~1530 ms:

```
--advisory decision    declared horizon 1000 ms      SLO BREACHED   exit 5
--advisory parameter   declared horizon 789282 ms    SLO MET        exit 0
```

Both are asserted in `scripts/reproduce.sh`.

### Two distributions, kept apart

`OFFERED` is the age of the newest advice available when the fast lane looked.
It is the producer's property and the SLO is declared on it. `ACTED-ON` is what
the gate let through, and is bounded by the gate by construction.

The first version recorded offered age *after* the expiry check. With a TTL
tighter than the horizon that truncates the distribution at the TTL, so the
contract reported MET by discarding exactly the samples that would have failed
it. `tests/test_freshness.cpp` pins this: seven reads past a 200 ms TTL must all
be counted, and the SLO must then report BREACHED.

### Cadence is reported, not enforced

`CadenceBudget` measures the market-time gap between publishes and counts those
over budget. It is reported next to a control — the tape's own inter-trade gaps
— and does not gate the exit code:

```
cadence           p50 135 ms, p99 1593 ms   (budget 250 ms, 33.2% over)
tape's own gaps   p50  27 ms, p99 1291 ms
```

At the tail the two nearly coincide. A slow lane cannot publish more often than
the market gives it something to say, and failing a run for not publishing
during a silent market would be failing it for physics. Where cadence exceeds
the control — the p50, 135 ms against 27 ms — the lane is genuinely behind, and
that part is engineering.

This is also why "publish faster" was never the fix. Most of the tail staleness
is the market's own event sparsity, and the answer to inherent staleness is to
carry something that tolerates it.

## The general form

Put the slow-moving quantity on the slow lane, and evaluate the fast-moving one
where the state is fresh.

Online feature stores reach the same rule from the other direction: each feature
carries its own staleness budget rather than inheriting one global freshness
target, because a lifetime average and a five-minute count are not stale on the
same timescale. A lane that ships one advisory type with one expiry is making
the mistake that framing exists to prevent.

## What is still open

- The lane recovers 89.8% of the reference's actions, not 100%. The residual is
  the remaining ~3-trade delay on the parameter's *first* availability and the
  window the fast lane rebuilds from scratch.
- The lane acts more often than the reference (5254 vs 3320) because the two
  calibrate their thresholds on different subsamples — per-publish against
  per-trade. The agreement rate and the decision-vs-parameter comparison are
  unaffected, but the informedness *levels* of lane and reference are not
  directly comparable, and the tool says so in its own output.
