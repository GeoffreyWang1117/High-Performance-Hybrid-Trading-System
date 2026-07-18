# Titans API Reference

## Table of Contents

1. [Core Types](#1-core-types)
2. [Context Management](#2-context-management)
3. [Contamination Injection](#3-contamination-injection)
4. [Evaluation Metrics](#4-evaluation-metrics)
5. [Experiment Framework](#5-experiment-framework)
6. [LLM Integration](#6-llm-integration)
7. [Data Adapters](#7-data-adapters)
8. [Distributed Experiments](#8-distributed-experiments)
9. [GPU Monitoring](#9-gpu-monitoring)
10. [Utilities](#10-utilities)

---

## 1. Core Types

### `titans/core/types.hpp`

```cpp
namespace titans {

using Price = int64_t;      // Fixed-point: actual_price * 10^8
using Quantity = int64_t;
using OrderId = uint64_t;
using Timestamp = int64_t;  // Nanoseconds since epoch
using Duration = int64_t;   // Nanoseconds
using Symbol = std::array<char, 16>;

struct Order {
    OrderId id;
    Symbol symbol;
    Price price;
    Quantity quantity;
    Side side;          // Buy/Sell
    OrderType type;     // Limit/Market
    Timestamp timestamp;
};

struct Trade {
    TradeId id;
    OrderId maker_order_id;
    OrderId taker_order_id;
    Price price;
    Quantity quantity;
    Timestamp timestamp;
};

}
```

---

## 2. Context Management

### `titans/context/versioned_entity.hpp`

#### ProvenanceSource

```cpp
enum class ProvenanceSource : uint8_t {
    RawData,           // Direct observation
    RuleSystem,        // Deterministic rule
    StatisticalModel,  // Statistical detection
    GPUFeature,        // CUDA-computed
    LLMInference,      // Model inference
    LLMHypothesis,     // Model speculation
    HumanAnnotation,   // Human-provided
    ExternalTool,      // External system
    CrossAgentClaim,   // From another agent
    SummaryDerived,    // From summary
    Unknown
};
```

#### Provenance

```cpp
struct Provenance {
    ProvenanceSource source;
    std::string source_id;
    Timestamp observed_at;
    double confidence;        // 0.0 - 1.0
    std::string evidence_ref;

    bool is_direct_observation() const;
    bool is_model_derived() const;
};
```

#### TemporalValidity

```cpp
struct TemporalValidity {
    Timestamp observation_time;
    Timestamp effective_from;
    Timestamp effective_until;  // 0 = indefinite
    Duration max_age;
    bool is_superseded;

    bool is_valid_at(Timestamp t) const;
    Duration age_at(Timestamp t) const;
};
```

#### VersionedState<T>

```cpp
template <typename T>
struct VersionedState {
    StateVersion version;
    T value;
    TemporalValidity validity;
    Provenance provenance;
    std::vector<std::string> related_events;

    bool is_current(Timestamp now) const;
};
```

#### VersionedEntity<T>

```cpp
template <typename T>
class VersionedEntity {
public:
    explicit VersionedEntity(EntityId id);

    // Add new state version
    StateVersion add_state(
        const T& value,
        const TemporalValidity& validity,
        const Provenance& provenance,
        const std::vector<std::string>& events = {}
    );

    // Get current state (if valid)
    const VersionedState<T>* current_state(Timestamp now) const;

    // Get state at specific version
    const VersionedState<T>* state_at_version(StateVersion v) const;

    // Get all states valid at time
    std::vector<const VersionedState<T>*> states_valid_at(Timestamp t) const;

    // Get full history
    const std::vector<VersionedState<T>>& history() const;

    // Detect conflicts
    bool has_conflict(Timestamp now) const;

    const EntityId& id() const;
    StateVersion current_version() const;
};
```

#### ContaminationType

```cpp
enum class ContaminationType : uint8_t {
    None = 0,
    StaleState,              // Outdated state used
    EntityBinding,           // Wrong entity's state
    InferencePersistence,    // Speculation as fact
    SummaryContamination,    // Stale summary
    RetrievalContamination,  // Stale retrieval
    CrossAgentContamination, // Error from agent
    SourceAmbiguity          // Unknown provenance
};
```

#### ContextReliabilityChecker

```cpp
class ContextReliabilityChecker {
public:
    template <typename T>
    std::optional<ContaminationEvent> check_state_validity(
        const VersionedState<T>& state,
        Timestamp current_time,
        const VersionedEntity<T>& entity
    );

    template <typename T>
    std::vector<const VersionedState<T>*> build_safe_context(
        const std::vector<VersionedEntity<T>*>& entities,
        Timestamp current_time,
        std::vector<ContaminationEvent>& detected_issues
    );
};
```

---

## 3. Contamination Injection

### `titans/context/contamination_injector.hpp`

#### ContaminationConfig

```cpp
struct ContaminationConfig {
    ContaminationType type;
    double probability;         // 0.0 - 1.0
    double severity_mean;
    double severity_stddev;
    Duration delay_before_effect;
    Duration persistence;
    bool propagate_cross_agent;
    std::string description;
};
```

#### ContaminationInjector

```cpp
class ContaminationInjector {
public:
    explicit ContaminationInjector(uint64_t seed = 42);

    void register_contamination(const ContaminationConfig& config);
    void clear_configurations();

    // Type 1: Stale State
    template <typename T>
    InjectionResult inject_stale_state(
        VersionedEntity<T>& entity,
        Timestamp current_time,
        Duration stale_duration
    );

    // Type 2: Entity Binding Error
    template <typename T>
    InjectionResult inject_entity_binding_error(
        VersionedEntity<T>& source_entity,
        VersionedEntity<T>& target_entity,
        Timestamp current_time
    );

    // Type 3: Inference Persistence
    template <typename T>
    InjectionResult inject_inference_as_fact(
        VersionedEntity<T>& entity,
        const T& inferred_value,
        Timestamp current_time,
        Duration persistence
    );

    // Type 4: Summary Contamination
    SummaryContamination inject_summary_contamination(
        const std::string& current_summary,
        const std::vector<std::string>& stale_facts,
        Timestamp current_time
    );

    // Type 5: Retrieval Contamination
    std::vector<RetrievalResult> inject_retrieval_contamination(
        const std::vector<RetrievalResult>& original_results,
        const std::vector<RetrievalResult>& stale_results,
        double contamination_probability,
        Timestamp current_time
    );

    // Type 6: Cross-Agent Contamination
    AgentMessage inject_cross_agent_contamination(
        const std::string& source_agent,
        const std::string& target_agent,
        const std::string& error_claim,
        Timestamp current_time
    );

    // Probabilistic injection
    template <typename T>
    std::vector<InjectionResult> probabilistic_inject(
        std::vector<VersionedEntity<T>*>& entities,
        Timestamp current_time
    );

    // Logging
    const std::vector<InjectionLogEntry>& get_injection_log() const;
    InjectionStats get_stats() const;
};
```

---

## 4. Evaluation Metrics

### `titans/context/evaluation_metrics.hpp`

#### ContaminationImpactMetrics

```cpp
struct ContaminationImpactMetrics {
    double accuracy;
    double accuracy_clean;
    double accuracy_contaminated;
    double accuracy_delta;

    double stale_reference_rate;
    double stale_decision_rate;
    double entity_confusion_rate;
    double inference_persistence_rate;

    double cross_round_propagation_rate;
    double cross_agent_propagation_rate;

    double false_positive_rate;
    double false_negative_rate;
    double inappropriate_action_rate;

    double appropriate_abstention_rate;
    double missed_abstention_rate;
};
```

#### ContaminationPersistenceMetrics

```cpp
struct ContaminationPersistenceMetrics {
    Duration mean_persistence;
    Duration max_persistence;
    Duration p50_persistence;
    Duration p90_persistence;
    Duration p99_persistence;

    Duration mean_recovery_time;
    double recovery_rate;

    int mean_affected_rounds;
    int max_affected_rounds;
    int mean_affected_entities;
};
```

#### MitigationEffectivenessMetrics

```cpp
struct MitigationEffectivenessMetrics {
    double accuracy_improvement;
    double relative_improvement;

    double contamination_detection_rate;
    double contamination_prevention_rate;
    double false_alarm_rate;

    std::unordered_map<ContaminationType, double> detection_rate_by_type;
    std::unordered_map<ContaminationType, double> prevention_rate_by_type;

    double persistence_reduction;

    double context_length_overhead;
    double latency_overhead_ms;
    double computation_overhead;
};
```

#### MetricCalculator

```cpp
class MetricCalculator {
public:
    ContaminationImpactMetrics calculate_impact_metrics(
        const std::vector<ModelOutput>& outputs
    );

    ContaminationPersistenceMetrics calculate_persistence_metrics(
        const std::vector<ContaminationEvent>& events,
        const std::vector<ModelOutput>& outputs
    );

    MitigationEffectivenessMetrics calculate_mitigation_effectiveness(
        const ContaminationImpactMetrics& baseline_metrics,
        const ContaminationImpactMetrics& mitigated_metrics,
        const ContaminationPersistenceMetrics& baseline_persistence,
        const ContaminationPersistenceMetrics& mitigated_persistence,
        double context_overhead = 0,
        double latency_overhead = 0
    );
};
```

---

## 5. Experiment Framework

### `titans/context/experiment_harness.hpp`

#### ExperimentConfig

```cpp
struct ExperimentConfig {
    std::string experiment_id;
    std::string description;

    size_t num_events = 10000;
    size_t num_entities = 100;
    Duration event_interval_ns = 1000000;

    double contamination_rate = 0.1;
    std::vector<ContaminationConfig> contamination_configs;

    ContextMethod method = ContextMethod::VersionedContext;

    size_t num_rounds = 100;
    bool collect_detailed_traces = false;

    uint64_t seed = 42;
};
```

#### ContextMethod

```cpp
enum class ContextMethod {
    NoHistory,              // Only current event
    FullHistory,            // Keep everything
    FixedWindow,            // Last N tokens
    RollingSummary,         // Compress old context
    VectorRetrieval,        // Semantic retrieval
    TimeFilter,             // Drop old by time
    VersionedContext,       // Our method
    FullVersionedIntegrity  // Full method
};

std::string method_name(ContextMethod m);
```

#### ExperimentRunner

```cpp
class ExperimentRunner {
public:
    ExperimentResult run_experiment(const ExperimentConfig& config);

    std::vector<ExperimentResult> run_comparison(
        const ExperimentConfig& base_config,
        const std::vector<ContextMethod>& methods
    );

    void print_comparison(const std::vector<ExperimentResult>& results);
};
```

#### AblationRunner

```cpp
class AblationRunner {
public:
    std::vector<ExperimentResult> run_ablation_study(
        const ExperimentConfig& base_config,
        const std::vector<AblationConfig>& ablations
    );

    static std::vector<AblationConfig> standard_ablations();
};
```

---

## 6. LLM Integration

### `titans/context/llm_interface.hpp`

#### LLMRequest / LLMResponse

```cpp
struct LLMRequest {
    std::string model;
    std::vector<LLMMessage> messages;
    double temperature = 0.0;
    int max_tokens = 1024;
    bool stream = false;
    std::optional<std::string> response_format;

    std::string request_id;
    Timestamp context_cutoff_time;
    std::vector<std::string> entity_ids_in_context;
};

struct LLMResponse {
    std::string request_id;
    std::string content;
    std::string model;

    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;

    double latency_ms = 0;
    double time_to_first_token_ms = 0;

    bool success = true;
    std::string error_message;
};
```

#### LLMBackend

```cpp
class LLMBackend {
public:
    virtual std::string name() const = 0;
    virtual bool is_available() const = 0;
    virtual std::vector<std::string> available_models() const = 0;

    virtual LLMResponse complete(const LLMRequest& request) = 0;
    virtual std::future<LLMResponse> complete_async(const LLMRequest& request) = 0;

    virtual std::vector<LLMResponse> complete_batch(
        const std::vector<LLMRequest>& requests,
        int max_concurrent = 4
    );
};
```

### `titans/context/ollama_backend.hpp`

#### OllamaBackendImpl

```cpp
class OllamaBackendImpl : public LLMBackend {
public:
    explicit OllamaBackendImpl(const OllamaConfig& config = {});

    // LLMBackend interface
    std::string name() const override;
    bool is_available() const override;
    std::vector<std::string> available_models() const override;
    LLMResponse complete(const LLMRequest& request) override;
    std::future<LLMResponse> complete_async(const LLMRequest& request) override;

    // Additional methods
    bool pull_model(const std::string& model_name);
    json::Value get_model_info(const std::string& model_name);
};
```

#### LLMBackendFactory

```cpp
class LLMBackendFactory {
public:
    static std::shared_ptr<LLMBackend> create_ollama(const OllamaConfig& config = {});
    static std::shared_ptr<LLMBackend> create_vllm(const VLLMConfig& config = {});
    static std::shared_ptr<LLMBackend> auto_detect();
};
```

---

## 7. Data Adapters

### `titans/context/data_adapters.hpp`

#### DataSource

```cpp
class DataSource {
public:
    virtual bool open(const std::string& path) = 0;
    virtual bool has_next() const = 0;
    virtual SyntheticEvent next() = 0;
    virtual size_t total_events() const = 0;
    virtual void reset() = 0;
};
```

#### Available Adapters

```cpp
class LOBSTERAdapter : public DataSource;   // Nasdaq L3
class BinanceTradeAdapter : public DataSource;  // Binance
class TitansBinaryAdapter : public DataSource;  // Our format
```

#### DatasetCatalog

```cpp
class DatasetCatalog {
public:
    explicit DatasetCatalog(const std::string& catalog_path = "data/catalog.csv");

    std::vector<DatasetInfo> list_datasets() const;
    std::shared_ptr<DataSource> get_dataset(const std::string& name);
    void register_dataset(const DatasetInfo& info);
};
```

---

## 8. Distributed Experiments

### `titans/context/distributed_experiment.hpp`

#### LocalDistributedRunner

```cpp
class LocalDistributedRunner {
public:
    LocalDistributedRunner(int num_workers = 4);

    std::vector<ExperimentResult> run_distributed(
        const std::vector<ExperimentConfig>& configs
    );
};
```

#### ExperimentGridGenerator

```cpp
class ExperimentGridGenerator {
public:
    struct GridConfig {
        std::vector<ContextMethod> methods;
        std::vector<double> contamination_rates;
        std::vector<size_t> event_counts;
        int seeds_per_config = 5;
        size_t num_entities = 50;
    };

    static std::vector<ExperimentConfig> generate_grid(const GridConfig& grid);
    static GridConfig standard_grid();
    static GridConfig quick_grid();
};
```

#### ResultAggregator

```cpp
class ResultAggregator {
public:
    struct AggregatedResult {
        std::string method;
        double contamination_rate;
        size_t event_count;
        double accuracy_mean;
        double accuracy_std;
        int sample_count;
    };

    static std::vector<AggregatedResult> aggregate(
        const std::vector<ExperimentResult>& results
    );

    static void print_aggregated(const std::vector<AggregatedResult>& results);
};
```

---

## 9. GPU Monitoring

### `titans/core/gpu_monitor.hpp`

#### GPUMetrics

```cpp
struct GPUMetrics {
    int gpu_id;
    std::string name;

    double gpu_utilization;      // 0-100%
    double memory_utilization;

    size_t memory_used;          // bytes
    size_t memory_total;
    double memory_percent;

    double temperature_c;

    double power_draw_w;
    double power_limit_w;

    std::string pstate;          // P0-P12
    std::chrono::steady_clock::time_point timestamp;

    double memory_used_gb() const;
    double memory_total_gb() const;
};
```

#### GPUQuery

```cpp
class GPUQuery {
public:
    static bool is_nvidia_available();
    static int gpu_count();
    static GPUSnapshot query_all();
    static GPUMetrics query_gpu(int gpu_id);
};
```

#### GPUMonitor

```cpp
class GPUMonitor {
public:
    using Callback = std::function<void(const GPUSnapshot&)>;

    explicit GPUMonitor(int interval_ms = 1000);

    void start();
    void stop();

    void add_callback(Callback cb);
    GPUSnapshot current() const;
    std::vector<GPUSnapshot> history() const;

    struct Statistics {
        double avg_gpu_utilization;
        double max_gpu_utilization;
        double avg_memory_gb;
        double max_memory_gb;
        double avg_temperature;
        double max_temperature;
        size_t sample_count;
    };

    Statistics compute_statistics() const;
    void clear_history();
};
```

---

## 10. Utilities

### `titans/core/json.hpp`

```cpp
namespace titans::json {

class Value {
public:
    // Constructors
    Value();
    Value(bool b);
    Value(int n);
    Value(double n);
    Value(const std::string& s);
    Value(const Array& a);
    Value(const Object& o);

    // Type checks
    bool is_null() const;
    bool is_bool() const;
    bool is_number() const;
    bool is_string() const;
    bool is_array() const;
    bool is_object() const;

    // Getters
    bool as_bool(bool default_val = false) const;
    double as_number(double default_val = 0.0) const;
    int as_int(int default_val = 0) const;
    const std::string& as_string() const;
    const Array& as_array() const;
    const Object& as_object() const;

    // Access
    const Value& operator[](size_t index) const;
    const Value& operator[](const std::string& key) const;
    bool contains(const std::string& key) const;
    size_t size() const;
};

// Functions
Value parse(const std::string& json);
std::optional<Value> try_parse(const std::string& json);
std::string stringify(const Value& v, bool pretty = false);

}
```

### `titans/core/http_client.hpp`

```cpp
namespace titans::http {

class HttpClient {
public:
    Response get(const std::string& url, int timeout_ms = 30000);

    Response post(const std::string& url,
                  const std::string& body,
                  const std::string& content_type = "application/json",
                  int timeout_ms = 30000);

    std::future<Response> async_post(const std::string& url,
                                     const std::string& body,
                                     int timeout_ms = 30000);
};

}
```

### `titans/core/debug.hpp`

See [Debugging Guide](DEBUGGING_GUIDE.md) for detailed documentation.

---

## Usage Examples

### Basic Experiment

```cpp
#include "titans/context/experiment_harness.hpp"

using namespace titans::context;

int main() {
    ExperimentConfig config;
    config.experiment_id = "my_experiment";
    config.num_events = 5000;
    config.contamination_rate = 0.15;
    config.method = ContextMethod::VersionedContext;

    ExperimentRunner runner;
    auto result = runner.run_experiment(config);
    result.print_summary();

    return 0;
}
```

### LLM Integration

```cpp
#include "titans/context/ollama_backend.hpp"

using namespace titans::context;

int main() {
    auto backend = LLMBackendFactory::create_ollama();

    if (!backend->is_available()) {
        std::cerr << "Ollama not running!" << std::endl;
        return 1;
    }

    LLMRequest request;
    request.model = "llama3.1:8b";
    request.messages = {
        {LLMMessage::Role::System, "You are an anomaly detector."},
        {LLMMessage::Role::User, "Is this event anomalous? {...}"}
    };

    auto response = backend->complete(request);
    std::cout << response.content << std::endl;

    return 0;
}
```

### Distributed Grid Search

```cpp
#include "titans/context/distributed_experiment.hpp"

using namespace titans::context;

int main() {
    auto grid = ExperimentGridGenerator::standard_grid();
    auto configs = ExperimentGridGenerator::generate_grid(grid);

    LocalDistributedRunner runner(8);  // 8 workers
    auto results = runner.run_distributed(configs);

    auto aggregated = ResultAggregator::aggregate(results);
    ResultAggregator::print_aggregated(aggregated);

    return 0;
}
```
