# Where this sits

A positioning document, not a literature review. It answers one question: what
in this repository is actually unusual, and what is a re-implementation of
something the field already does?

It is written down because the honest answer is mixed, and a project that
claims novelty it does not have is exactly the failure mode the rest of this
repository is built to prevent.

## How this was checked

A discovery scan (`scholar_scout`, 30-month window, 6 sources) returned 602
papers, 411 after deduplication. **The scan alone was not sufficient**, and its
own numbers say why:

- `strategies_used = [keyword_search, author_tracking, ecosystem_search]` — the
  deep query-extraction and citation-graph passes did not run.
- `api_usage: 58 calls, rate-limited: 22` — 38% of calls were throttled.
- None of the field's canonical priors appeared anywhere in the 60 ranked
  results — not TradingAgents, FinMem, AgentPoison, HFTBench, or the 2026
  agentic-trading survey. The scan's own top-ranked on-topic hit names two of
  them in its opening sentences.

So every claim below rests on directly reading the paper named, not on a
search-result snippet. Where only an abstract was read, it says so.

## The nearest neighbours

### On the trading side

**QuantAgent / QuantHarness** ([arXiv 2509.09995](https://arxiv.org/abs/2509.09995))
calls itself "the first multi-agent LLM framework explicitly designed for
high-frequency algorithmic trading." Reading the abstract: it evaluates at
**1-hour and 4-hour trading intervals**. That is not a criticism of the work —
it is a well-motivated system that correctly identifies TradingAgents and FinMem
as ill-suited to short-horizon signals. But its "high frequency" and this
repository's are six orders of magnitude apart, and it reports no wall-clock
latency for the LLM against the trading loop. *(Abstract read; full text not
read.)*

**Win Fast or Lose Slow** ([arXiv 2505.19481](https://arxiv.org/abs/2505.19481))
is the closest work on the axis this repository actually cares about. It makes
latency a first-class evaluation variable for LLM agents, introduces HFTBench
(an HFT simulation) and StreetFighter, and proposes FPX, which selects model
size and quantization against a real-time budget. Anyone reading this repo's
measurement sections should read that paper. The difference in kind: HFTBench
evaluates *the model* under a latency constraint; this repository measures
*the system* — an actual pinned C++ fast path — and asks whether the slow lane
can perturb it. *(Abstract read; the arXiv landing page did not state whether
its latency figures are wall-clock or simulated, so that question is open here.)*

**Agentic Trading: When LLM Agents Meet Financial Markets**
([arXiv 2605.19337](https://arxiv.org/abs/2605.19337)) surveys 77 LLM-trading
studies and finds only 19 meet minimum evaluation criteria. Of those 19: 2
report an extractable time-consistent split protocol, 1 reports an explicit
transaction-cost model, 1 documents survivorship handling. That finding is the
strongest argument for this repository's posture. The bar the field is failing
is not sophistication; it is stating how the number was produced.

### On the contamination side

The study of corrupted LLM memory is a crowded and fast-moving area, and it is
overwhelmingly framed as **security**:

| Work | Framing |
|---|---|
| [AgentPoison](https://arxiv.org/abs/2407.12784) (NeurIPS 2024) | backdoor poisoning of RAG memory/knowledge bases |
| [Hidden in Memory](https://arxiv.org/abs/2605.15338) | sleeper memory poisoning, delayed activation |
| [FARMA / SENTINEL](https://arxiv.org/abs/2607.05029) | forging the agent's *reasoning* history, and a detector |
| [MemSecBench](https://arxiv.org/abs/2607.27080) | lifecycle benchmark: persistence → consequence → repair |
| [State Contamination in Memory-Augmented LLM Agents](https://arxiv.org/abs/2605.16746) | "memory laundering": toxic context survives summarization below detector thresholds |
| [Isolation as a First-Class Principle](https://arxiv.org/abs/2607.12406) | boundary-centric taxonomy of agent-system isolation |

That last pair is the closest prior art to `include/titans/context/`, and the
last one is close to this project's own architectural argument. **The
contamination taxonomy here is not novel.** What differs is the threat model:
those papers assume an adversary, and the defense is detection. This repository
assumes no adversary at all — just a fast-moving market where a fact that was
true 800 ms ago is now wrong — and the mechanism is expiry (`valid_until` +
`context_generation`) rather than detection. Staleness is not an attack, and it
does not need a classifier; it needs a clock and a generation counter.

## The honest verdict

**Not novel:**

- Running an LLM off the critical path of a low-latency system. This is standard
  industry architecture and appears in the surveys.
- The contamination taxonomy — stale state, entity binding, prior-inference
  injection, retrieval pollution. All of it is described in the security
  literature above, usually in more depth.
- LLM-for-trading as an idea. Multiple 2025–2026 systems, at least one claiming
  priority.

**Unusual, and defensible:**

1. **The isolation claim is executable rather than asserted.** Papers state
   that the LLM does not interfere with execution. `tests/test_lane_isolation.cpp`
   hangs the slow lane at 200 ms per item and shows the fast lane's p99 is
   unchanged (40 ns) while the bridge sheds 99.9% of observations — with the
   drop rate reported as a metric rather than swallowed. No paper found in this
   search measures that interference.

2. **The measurement harness refuses.** It declines to print a p99 for
   operations below its own measured 20 ns timing floor, and exits non-zero if
   its own calibration fails. Set against the survey finding that 2 of 19
   studies report a time-consistent protocol, "the tool will not emit an
   unresolvable number" is a real position.

3. **Contamination as hygiene, not attack.** Expiry and generation counting are
   an architectural answer, and they cost nothing at runtime — unlike a detector,
   which is itself an inference.

4. **The negative results shipped.** The lane policy's `NOT RESOLVED` verdict,
   the `INERT` ablation, the degenerate-classifier gate that exits 3 rather than
   reporting deltas from a model that answers the same thing every time, and a
   drift-correction sweep that tested a plausible fix at four window sizes and
   reports that none of them worked. Publication incentives select against all
   of these; the first one is also what eventually located a real defect,
   because a null result that is kept stays available to be explained later.

**Bottom line.** The research contribution is small. The engineering
contribution is that every claim in this repository can be falsified by running
one command, and several of them, when run, refuse to produce the answer that
would have looked better. That is the thing worth defending in an interview,
and it is not what the surveyed field is currently rewarding.

## What would change this verdict

- A non-degenerate live-model result on the contamination task (needs a model
  larger than the hardware here could host — see the Known Limitations).
- ~~A slow-lane policy that measurably reduces adverse selection.~~ **Done, and
  it changed the verdict.** A 28-day walk-forward puts the order-flow policy at
  +0.1676 informedness out of sample, 95% CI [+0.1385, +0.1966], on days its
  threshold never saw. The earlier "no measurable effect" was a statement about
  the delivery path, not the rule: the advisory the fast lane acts on has a
  median age of 82 ms and a p99 of 1507 ms against a 1000 ms prediction horizon.
  That makes the interesting open question a systems question -- what publish
  cadence a slow lane needs relative to the horizon it is predicting over --
  rather than a modelling one.
- Running HFTBench's task through this system's lanes, which would connect the
  model-level latency-quality curve to a system-level isolation measurement.
  That is the most interesting unbuilt thing here.
