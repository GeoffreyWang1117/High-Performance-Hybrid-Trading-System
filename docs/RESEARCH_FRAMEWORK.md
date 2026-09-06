# Titans Context Contamination Research Framework

> **Positioning first.** Before reading this as a research contribution, read
> [RELATED_WORK.md](RELATED_WORK.md). The contamination taxonomy below is not
> novel — it is covered, in more depth, by the 2024-2026 agent-memory security
> literature. What differs here is the threat model (staleness, not an attacker)
> and the mechanism (expiry, not detection).

## Overview

This framework supports systematic research on **LLM Context Contamination Detection and Mitigation** in event-driven streaming systems. It enables reproducible experiments comparing our versioned context approach against baseline methods.

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    Research Experiment Pipeline                         │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌──────────────┐    ┌──────────────────┐    ┌────────────────────┐    │
│  │ Data Sources │───▶│ Contamination    │───▶│ Context Method     │    │
│  │              │    │ Injector         │    │ Under Test         │    │
│  │ • LOBSTER    │    │                  │    │                    │    │
│  │ • Binance    │    │ 6 contamination  │    │ • NoHistory        │    │
│  │ • Synthetic  │    │ types:           │    │ • FullHistory      │    │
│  │ • Titans Log │    │ 1. Stale State   │    │ • FixedWindow      │    │
│  └──────────────┘    │ 2. Entity Bind   │    │ • TimeFilter       │    │
│                      │ 3. Inference     │    │ • VersionedContext │    │
│                      │ 4. Summary       │    │   (Our Method)     │    │
│                      │ 5. Retrieval     │    └─────────┬──────────┘    │
│                      │ 6. Cross-Agent   │              │               │
│                      └──────────────────┘              │               │
│                                                        ▼               │
│  ┌──────────────┐    ┌──────────────────┐    ┌────────────────────┐    │
│  │ LLM Backend  │◀───│ Prompt Builder   │◀───│ Model Inference    │    │
│  │              │    │                  │    │                    │    │
│  │ • Ollama     │    │ System prompt    │    │ Simulated or       │    │
│  │ • vLLM       │    │ Versioned ctx    │    │ Real LLM           │    │
│  │ • API        │    │ Event format     │    │                    │    │
│  └──────────────┘    └──────────────────┘    └─────────┬──────────┘    │
│                                                        │               │
│                                                        ▼               │
│  ┌──────────────┐    ┌──────────────────┐    ┌────────────────────┐    │
│  │ Persistence  │◀───│ Statistical      │◀───│ Metric Calculator  │    │
│  │              │    │ Analyzer         │    │                    │    │
│  │ • JSON       │    │                  │    │ • Impact Metrics   │    │
│  │ • CSV        │    │ • t-test         │    │ • Persistence      │    │
│  │ • LaTeX      │    │ • Bootstrap CI   │    │ • Mitigation Eff.  │    │
│  └──────────────┘    │ • Cross-val      │    └────────────────────┘    │
│                      └──────────────────┘                              │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

## Core Components

### 1. Versioned Entity State (`versioned_entity.hpp`)
- **VersionedState<T>**: State with version, temporal validity, and provenance
- **VersionedEntity<T>**: Entity with full version history
- **ContextReliabilityChecker**: Validates state before use
- **SelectiveForgetter**: Intelligent context pruning

### 2. Contamination Injection (`contamination_injector.hpp`)
Systematically injects 6 contamination types:

| Type | Description | Injection Method |
|------|-------------|------------------|
| Stale State | Outdated state used as current | `inject_stale_state()` |
| Entity Binding | Wrong entity's state used | `inject_entity_binding_error()` |
| Inference Persistence | Model speculation as fact | `inject_inference_as_fact()` |
| Summary Contamination | Stale info in summaries | `inject_summary_contamination()` |
| Retrieval Contamination | Stale RAG results | `inject_retrieval_contamination()` |
| Cross-Agent | Error propagation | `inject_cross_agent_contamination()` |

### 3. Evaluation Metrics (`evaluation_metrics.hpp`)
- **ContaminationImpactMetrics**: Accuracy, stale reference rate, entity confusion
- **ContaminationPersistenceMetrics**: Duration, recovery time, affected rounds
- **MitigationEffectivenessMetrics**: Improvement, detection/prevention rates

### 4. Experiment Harness (`experiment_harness.hpp`)
- **SyntheticDataGenerator**: Reproducible event streams whose labels are NOT
  recoverable from the event (guarded by `tests/test_task_design.cpp`)
- **AssumedDegradationModel**: a stand-in whose response to contamination is
  ASSUMED, not measured. Formerly named `ModelSimulator`, which read like a
  model. See "What the stand-in can and cannot support" below.
- **BaselineContextManager**: 5 baseline method implementations
- **ExperimentRunner**: Automated comparison pipeline
- **AblationRunner**: Component-wise ablation studies

### 5. LLM Interface (`llm_interface.hpp`)
- **LLMBackend**: Abstract interface for LLM providers
- **OllamaBackendImpl** / **VLLMBackendImpl** (`ollama_backend.hpp`): the real
  HTTP clients. Stub classes named `OllamaBackend` and `VLLMBackend` used to sit
  in `llm_interface.hpp` returning a fixed `{"classification": "normal"}` with
  `success = true`; they were deleted rather than fixed, because two classes
  with one responsibility and one silently fake is a trap.
- **LLMBackendFactory**: health-checks a real endpoint before returning a
  backend
- **PromptBuilder**: Task-specific prompt templates
- **LLMExperimentRunner**: Real LLM experiment orchestration

### 5b. Context Contamination (`context_contaminator.hpp`)
- **ContextContaminator**: mutates the context that is serialized into the
  prompt. Before this existed, the LLM experiment constructed a
  `ContaminationInjector`, never called it, and attached the contamination
  vector to the output as metadata -- the independent variable was never
  applied to the model's input.
- Each contamination type must be MISLEADING IF TRUSTED but DETECTABLE IN
  PRINCIPLE; an undetectable one would flatten every strategy comparison just as
  surely as no contamination at all.

### 5c. Real Market Data (`binance_dataset.hpp`)
- **BinanceToxicFlowDataset**: Binance aggTrades labelled by forward adverse
  selection -- toxic if the price moves `threshold_bps` in the aggressor's
  favour within `horizon_ms`. The label comes from the future, so no field of
  the event can encode it.
- **`titans_dataset`**: labels a file and audits it for leakage, exiting
  non-zero if any single event field predicts the label or if context features
  do not.

### 6. Data Adapters (`data_adapters.hpp`)
- **LOBSTERAdapter**: Nasdaq L3 order book data
- **BinanceTradeAdapter**: Binance historical trades
- **TitansBinaryAdapter**: Our binary log format
- **DatasetCatalog**: Dataset discovery and management

### 7. Persistence & Analysis (`experiment_persistence.hpp`)
- **ExperimentStore**: JSON/CSV result storage
- **StatisticalAnalyzer**: t-test, bootstrap CI, cross-validation
- **VisualizationExporter**: Paper figure data export
- **LaTeX table generation**

## Quick Start

### Run Simulated Experiments
```cpp
#include "titans/context/experiment_harness.hpp"

ExperimentConfig config;
config.experiment_id = "baseline_comparison";
config.num_events = 10000;
config.contamination_rate = 0.15;

ExperimentRunner runner;
auto results = runner.run_comparison(config, {
    ContextMethod::NoHistory,
    ContextMethod::FullHistory,
    ContextMethod::FixedWindow,
    ContextMethod::TimeFilter,
    ContextMethod::VersionedContext
});

runner.print_comparison(results);
```

### Run Real LLM Experiments
```cpp
#include "titans/context/llm_interface.hpp"

OllamaConfig cfg;
cfg.host = "localhost";
cfg.port = 11434;
cfg.default_model = "llama3.1:8b";

auto backend = LLMBackendFactory::create_ollama(cfg);
if (!backend->is_available()) {
    // Do NOT substitute a stand-in here. A number produced without querying a
    // model is not evidence about a model.
    return 1;
}
```

In practice, drive it from the command line, which runs both arms of the paired
design and writes every raw response to the results file:

```bash
./build/titans_llm_experiment --backend vllm --port 8000 \
    --model Qwen/Qwen2.5-7B-Instruct --events 1000 --seed 42 --out results/llm
```

### Generate Paper Figures
```bash
python python/research/generate_figures.py --results results
```
Fails with exit 2 when the results directory is empty. There is deliberately no
synthetic fallback; the previous version drew `np.random` data around
hand-picked accuracies and rendered publication-ready figures from it.

## What the stand-in can and cannot support

`AssumedDegradationModel` computes accuracy as a product of hardcoded
multipliers -- 0.5 for an entity-binding error, 0.7 for stale state, and so on
-- followed by one Bernoulli draw. The constants were chosen, not observed.

**It cannot answer "which contamination type hurts a language model most",**
because the ranking it produces is exactly the ranking of those constants.
Quoting it for that purpose is circular: the conclusion was typed into the
switch statement. `titans_experiment` prints this in its own output.

It IS useful for exercising the pipeline end to end without an inference server,
for checking that injection, mitigation, and metrics respond in the expected
direction, and for regression-testing refactors.

Further caveats:

1. **NoHistory is a strong baseline by construction.** The stand-in
   gives context no upward benefit — it only carries contamination risk — so a
   method that discards everything scores well on clean accuracy. With real
   LLMs, context improves clean-task accuracy, which is exactly what the
   real-model experiments (`titans_llm_experiment`) must measure. Simulated
   results are therefore valid for *ranking contamination resistance*, not for
   absolute accuracy claims.
2. **Summary/Retrieval contamination is undetectable by the entity store.**
   `VersionedContext` cannot inspect inside a summary blob or a retrieved
   passage; only provenance-tagged state is checkable. This is an intentional
   scope boundary, not an oversight.
3. **Residual degradation constants** (0.995 per stale context item, floor at
   300 items) are stand-in parameters, not measurements.

4. **An ablation that changes nothing is reported as INERT, not as a null
   result.** `AblationRunner::run_ablation_study_checked()` flags any
   configuration whose metrics are identical to the full system and states why
   the disabled component never executed. An identical row means the experiment
   cannot tell whether the component matters -- not that it does not.

## Experiment Checklist

### Phase 1: Simulated Experiments
- [ ] Baseline method comparison (5 methods)
- [ ] Contamination rate sensitivity (5-50%)
- [ ] Ablation study (7 configurations)
- [ ] Cross-validation (k=5)

### Phase 2: Real LLM Experiments
- [ ] Ollama local models (Llama 3.1 8B, 70B)
- [ ] vLLM deployment (Qwen 2.5, Mistral)
- [ ] Multi-model comparison matrix

### Phase 3: Real Data Experiments
- [ ] LOBSTER dataset (Nasdaq L3)
- [ ] Binance historical trades
- [ ] Cross-dataset generalization

### Phase 4: Analysis
- [ ] Statistical significance testing
- [ ] Generate paper figures (6 figures)
- [ ] Generate paper tables (3 tables)

## Output Artifacts

```
experiments/
├── baseline_comparison/
│   ├── all_results.jsonl
│   └── summary.csv
├── sensitivity_analysis/
├── ablation_study/
└── index.csv

figures/
├── fig1_accuracy_comparison.pdf
├── fig2_sensitivity_curve.pdf
├── fig3_accuracy_delta_heatmap.pdf
├── fig4_contamination_breakdown.pdf
├── fig5_ablation_study.pdf
├── fig6_latency_comparison.pdf
├── table1_main_results.tex
└── *.png (raster versions)
```

## Hardware Requirements

### Minimum (Simulated Experiments)
- CPU: 4 cores
- RAM: 8GB
- Storage: 10GB

### Recommended (Local LLM Experiments)
- CPU: 16 cores
- RAM: 64GB
- GPU: 24GB+ VRAM for a 7-8B model at bf16
- Storage: 100GB SSD

No GPU available? `python/serving/cpu_shim.py` serves the same
OpenAI-compatible protocol on CPU. It is not a vLLM replacement -- no batching,
one request at a time, single-digit tokens/sec -- but it keeps the experiment
path exercisable in CI and on a workstation whose GPUs are committed elsewhere.

### Model compatibility
Any backend speaking the Ollama or OpenAI chat protocol. The table below lists
what the code supports, NOT what has been run; consult `results/` for the
models actually exercised, since each results file records its backend and
model.

| Size | Approx. VRAM (bf16) | Backend |
|------|---------------------|---------|
| 1.5B | 3GB  | any, or the CPU shim |
| 3B   | 6GB  | Ollama / vLLM |
| 7-8B | 16GB | vLLM |
| 32B  | 64GB, or ~20GB quantized | vLLM |

## Citation

```bibtex
@misc{titans_contamination,
  title  = {Titans: Context Contamination in Event-Driven LLM Analytics},
  note   = {Software framework. No results in this repository have been
            published; figures come only from measured runs recorded under
            results/.},
  year   = {2026}
}
```
