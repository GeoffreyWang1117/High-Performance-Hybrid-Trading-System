# Titans Context Contamination Research Framework

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
- **SyntheticDataGenerator**: Reproducible event streams
- **ModelSimulator**: Controlled inference experiments
- **BaselineContextManager**: 5 baseline method implementations
- **ExperimentRunner**: Automated comparison pipeline
- **AblationRunner**: Component-wise ablation studies

### 5. LLM Interface (`llm_interface.hpp`)
- **LLMBackend**: Abstract interface for LLM providers
- **OllamaBackend**: Local Ollama integration
- **VLLMBackend**: High-throughput local inference
- **PromptBuilder**: Task-specific prompt templates
- **LLMExperimentRunner**: Real LLM experiment orchestration

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

auto ollama = std::make_shared<OllamaBackend>(OllamaConfig{
    .host = "localhost",
    .port = 11434,
    .default_model = "llama3.1:8b"
});

LLMExperimentConfig config;
config.experiment_id = "llama3_versioned";
config.model_name = "llama3.1:8b";
config.backend = ollama;
config.context_method = ContextMethod::VersionedContext;
config.num_events = 500;

LLMExperimentRunner runner;
auto result = runner.run(config);
result.print_summary();
```

### Generate Paper Figures
```bash
cd python/research
python generate_figures.py
```

## Known Limitations of the Simulator

Honest caveats to carry into the paper:

1. **NoHistory is a strong baseline by construction.** The `ModelSimulator`
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
   300 items) are simulator parameters. Sensitivity to them should be reported
   in an appendix; the method *ranking* is what the simulation establishes.

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
- GPU: NVIDIA RTX 4090 / A100 (24GB+ VRAM)
- Storage: 100GB SSD

### Models Tested
| Model | VRAM Required | Recommended Backend |
|-------|---------------|---------------------|
| Llama 3.1 8B | 8GB | Ollama |
| Llama 3.1 70B | 40GB | vLLM |
| Qwen 2.5 32B | 20GB | vLLM |
| Mistral 7B | 6GB | Ollama |

## Citation

```bibtex
@inproceedings{titans2026contamination,
  title={Reliable Event-Driven LLM Analytics: A Framework for 
         Detecting and Mitigating Context Contamination},
  author={...},
  booktitle={Proceedings of ...},
  year={2026}
}
```
