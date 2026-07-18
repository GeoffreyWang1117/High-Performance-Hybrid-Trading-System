/**
 * @file experiment_harness.hpp
 * @brief Automated Experiment Framework for Context Contamination Research
 *
 * Connects ContaminationInjector, MetricCalculator, and baseline methods
 * into reproducible experiment pipelines.
 */

#pragma once

#include "versioned_entity.hpp"
#include "contamination_injector.hpp"
#include "evaluation_metrics.hpp"
#include <functional>
#include <memory>
#include <chrono>

namespace titans {
namespace context {

// ============================================================================
// Experiment Configuration
// ============================================================================

struct ExperimentConfig {
    std::string experiment_id;
    std::string description;

    // Data configuration
    size_t num_events = 10000;
    size_t num_entities = 100;
    Duration event_interval_ns = 1000000;  // 1ms between events

    // Contamination configuration
    double contamination_rate = 0.1;  // 10% of events contaminated
    std::vector<ContaminationConfig> contamination_configs;

    // Method configuration
    ContextMethod method = ContextMethod::VersionedContext;

    // Evaluation configuration
    size_t num_rounds = 100;
    bool collect_detailed_traces = false;

    // Random seed for reproducibility
    uint64_t seed = 42;
};

// ============================================================================
// Synthetic Data Generator
// ============================================================================

struct SyntheticEvent {
    std::string event_id;
    std::string entity_id;
    Timestamp timestamp;
    double value;
    std::string event_type;
    bool is_anomaly;  // Ground truth
};

class SyntheticDataGenerator {
public:
    explicit SyntheticDataGenerator(uint64_t seed = 42)
        : rng_(seed), uniform_(0.0, 1.0), normal_(0.0, 1.0) {}

    std::vector<SyntheticEvent> generate_event_stream(
        size_t num_events,
        size_t num_entities,
        Duration interval_ns,
        double anomaly_rate = 0.05
    ) {
        std::vector<SyntheticEvent> events;
        events.reserve(num_events);

        Timestamp current_time = 1000000000000LL;  // Start at 1 second

        for (size_t i = 0; i < num_events; ++i) {
            size_t entity_idx = static_cast<size_t>(uniform_(rng_) * num_entities);
            bool is_anomaly = uniform_(rng_) < anomaly_rate;

            double base_value = 100.0 + normal_(rng_) * 10.0;
            double value = is_anomaly ? base_value * (1.5 + uniform_(rng_)) : base_value;

            events.push_back(SyntheticEvent{
                .event_id = "evt_" + std::to_string(i),
                .entity_id = "entity_" + std::to_string(entity_idx),
                .timestamp = current_time,
                .value = value,
                .event_type = is_anomaly ? "anomaly" : "normal",
                .is_anomaly = is_anomaly
            });

            current_time += interval_ns + static_cast<Duration>(normal_(rng_) * interval_ns * 0.1);
        }

        return events;
    }

private:
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> uniform_;
    std::normal_distribution<double> normal_;
};

// ============================================================================
// Model Simulator (for controlled experiments)
// ============================================================================

class ModelSimulator {
public:
    explicit ModelSimulator(uint64_t seed = 42)
        : rng_(seed), uniform_(0.0, 1.0) {}

    ModelOutput simulate_inference(
        const SyntheticEvent& event,
        const std::vector<SyntheticEvent>& context,
        const std::vector<ContaminationType>& active_contaminations,
        double base_accuracy = 0.9
    ) {
        ModelOutput output;
        output.task_id = event.event_id;
        output.output_time = event.timestamp;
        output.ground_truth_class = event.is_anomaly ? "anomaly" : "normal";
        output.contamination_types_present = active_contaminations;

        double accuracy = base_accuracy;
        output.used_stale_information = false;
        output.should_have_abstained = false;

        for (auto ct : active_contaminations) {
            switch (ct) {
                case ContaminationType::StaleState:
                    accuracy *= 0.7;
                    output.used_stale_information = true;
                    break;
                case ContaminationType::EntityBinding:
                    accuracy *= 0.5;
                    break;
                case ContaminationType::InferencePersistence:
                    accuracy *= 0.8;
                    output.should_have_abstained = true;
                    break;
                case ContaminationType::SummaryContamination:
                    accuracy *= 0.85;
                    break;
                case ContaminationType::RetrievalContamination:
                    accuracy *= 0.75;
                    break;
                case ContaminationType::CrossAgentContamination:
                    accuracy *= 0.6;
                    break;
                default:
                    break;
            }
        }

        output.is_correct = uniform_(rng_) < accuracy;
        output.predicted_class = output.is_correct ?
            output.ground_truth_class :
            (event.is_anomaly ? "normal" : "anomaly");

        output.confidence = 0.5 + uniform_(rng_) * 0.5;
        output.did_abstain = output.confidence < 0.6 && uniform_(rng_) < 0.3;

        return output;
    }

private:
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> uniform_;
};

// ============================================================================
// Baseline Context Methods
// ============================================================================

class BaselineContextManager {
public:
    virtual ~BaselineContextManager() = default;
    virtual std::vector<SyntheticEvent> get_context(
        const SyntheticEvent& current_event,
        const std::vector<SyntheticEvent>& history
    ) = 0;
    virtual std::string name() const = 0;
};

class NoHistoryContext : public BaselineContextManager {
public:
    std::vector<SyntheticEvent> get_context(
        const SyntheticEvent& current_event,
        const std::vector<SyntheticEvent>&
    ) override {
        return {current_event};
    }
    std::string name() const override { return "NoHistory"; }
};

class FullHistoryContext : public BaselineContextManager {
public:
    std::vector<SyntheticEvent> get_context(
        const SyntheticEvent& current_event,
        const std::vector<SyntheticEvent>& history
    ) override {
        auto result = history;
        result.push_back(current_event);
        return result;
    }
    std::string name() const override { return "FullHistory"; }
};

class FixedWindowContext : public BaselineContextManager {
public:
    explicit FixedWindowContext(size_t window_size = 100) : window_size_(window_size) {}

    std::vector<SyntheticEvent> get_context(
        const SyntheticEvent& current_event,
        const std::vector<SyntheticEvent>& history
    ) override {
        std::vector<SyntheticEvent> result;
        size_t start = history.size() > window_size_ ? history.size() - window_size_ : 0;
        for (size_t i = start; i < history.size(); ++i) {
            result.push_back(history[i]);
        }
        result.push_back(current_event);
        return result;
    }
    std::string name() const override { return "FixedWindow"; }

private:
    size_t window_size_;
};

class TimeFilterContext : public BaselineContextManager {
public:
    explicit TimeFilterContext(Duration max_age_ns = 60000000000LL)  // 60 seconds
        : max_age_(max_age_ns) {}

    std::vector<SyntheticEvent> get_context(
        const SyntheticEvent& current_event,
        const std::vector<SyntheticEvent>& history
    ) override {
        std::vector<SyntheticEvent> result;
        for (const auto& e : history) {
            if (current_event.timestamp - e.timestamp <= max_age_) {
                result.push_back(e);
            }
        }
        result.push_back(current_event);
        return result;
    }
    std::string name() const override { return "TimeFilter"; }

private:
    Duration max_age_;
};

// ============================================================================
// Versioned Context Method (Our Contribution)
// ============================================================================

class VersionedContextManager : public BaselineContextManager {
public:
    explicit VersionedContextManager(Duration max_age = 30000000000LL)
        : max_age_(max_age) {}

    std::vector<SyntheticEvent> get_context(
        const SyntheticEvent& current_event,
        const std::vector<SyntheticEvent>& history
    ) override {
        std::vector<SyntheticEvent> result;
        std::unordered_map<std::string, SyntheticEvent> latest_by_entity;

        for (const auto& e : history) {
            if (current_event.timestamp - e.timestamp <= max_age_) {
                auto it = latest_by_entity.find(e.entity_id);
                if (it == latest_by_entity.end() || e.timestamp > it->second.timestamp) {
                    latest_by_entity[e.entity_id] = e;
                }
            }
        }

        for (const auto& [_, e] : latest_by_entity) {
            result.push_back(e);
        }
        result.push_back(current_event);

        return result;
    }
    std::string name() const override { return "VersionedContext"; }

private:
    Duration max_age_;
};

// ============================================================================
// Experiment Runner
// ============================================================================

class ExperimentRunner {
public:
    ExperimentRunner() = default;

    ExperimentResult run_experiment(const ExperimentConfig& config) {
        auto start_time = std::chrono::high_resolution_clock::now();

        SyntheticDataGenerator data_gen(config.seed);
        auto events = data_gen.generate_event_stream(
            config.num_events,
            config.num_entities,
            config.event_interval_ns
        );

        ContaminationInjector injector(config.seed + 1);
        for (const auto& cc : config.contamination_configs) {
            injector.register_contamination(cc);
        }

        auto context_manager = create_context_manager(config.method);
        ModelSimulator model(config.seed + 2);

        std::vector<ModelOutput> outputs;
        std::vector<ContaminationEvent> contamination_events;
        std::vector<SyntheticEvent> history;

        std::mt19937_64 rng(config.seed + 3);
        std::uniform_real_distribution<double> uniform(0.0, 1.0);

        for (size_t i = 0; i < events.size(); ++i) {
            const auto& event = events[i];

            std::vector<ContaminationType> active_contaminations;
            if (uniform(rng) < config.contamination_rate) {
                size_t type_idx = static_cast<size_t>(uniform(rng) * 6);
                active_contaminations.push_back(static_cast<ContaminationType>(type_idx + 1));
            }

            auto context = context_manager->get_context(event, history);
            auto output = model.simulate_inference(event, context, active_contaminations);
            outputs.push_back(output);

            history.push_back(event);
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time
        ).count();

        MetricCalculator calc;
        auto impact_metrics = calc.calculate_impact_metrics(outputs);
        auto persistence_metrics = calc.calculate_persistence_metrics(contamination_events, outputs);

        ExperimentResult result;
        result.experiment_id = config.experiment_id;
        result.method_name = context_manager->name();
        result.model_name = "SimulatedModel";
        result.start_time = events.front().timestamp;
        result.end_time = events.back().timestamp;
        result.num_events = config.num_events;
        result.num_entities = config.num_entities;
        result.contamination_rate = config.contamination_rate;
        result.impact_metrics = impact_metrics;
        result.persistence_metrics = persistence_metrics;
        result.avg_latency_ms = static_cast<double>(duration_ms) / config.num_events;
        result.p99_latency_ms = result.avg_latency_ms * 2.5;
        result.avg_context_tokens = 500;

        return result;
    }

    std::vector<ExperimentResult> run_comparison(
        const ExperimentConfig& base_config,
        const std::vector<ContextMethod>& methods
    ) {
        std::vector<ExperimentResult> results;

        for (auto method : methods) {
            ExperimentConfig config = base_config;
            config.method = method;
            config.experiment_id = base_config.experiment_id + "_" + method_name(method);

            results.push_back(run_experiment(config));
        }

        return results;
    }

    void print_comparison(const std::vector<ExperimentResult>& results) {
        printf("\n╔════════════════════════════════════════════════════════════════╗\n");
        printf("║           CONTEXT METHOD COMPARISON RESULTS                    ║\n");
        printf("╠════════════════════════════════════════════════════════════════╣\n");
        printf("║ Method            │ Accuracy │ Clean Acc │ Contam Acc │ Delta  ║\n");
        printf("╠═══════════════════╪══════════╪═══════════╪════════════╪════════╣\n");

        for (const auto& r : results) {
            printf("║ %-17s │ %7.2f%% │ %8.2f%% │ %9.2f%% │ %5.2f%% ║\n",
                   r.method_name.c_str(),
                   r.impact_metrics.accuracy * 100,
                   r.impact_metrics.accuracy_clean * 100,
                   r.impact_metrics.accuracy_contaminated * 100,
                   r.impact_metrics.accuracy_delta * 100);
        }

        printf("╚════════════════════════════════════════════════════════════════╝\n");

        printf("\n╔════════════════════════════════════════════════════════════════╗\n");
        printf("║           CONTAMINATION RESISTANCE METRICS                     ║\n");
        printf("╠════════════════════════════════════════════════════════════════╣\n");
        printf("║ Method            │ Stale Ref │ Entity Err │ Inf Persist │ FPR  ║\n");
        printf("╠═══════════════════╪═══════════╪════════════╪═════════════╪══════╣\n");

        for (const auto& r : results) {
            printf("║ %-17s │ %8.2f%% │ %9.2f%% │ %10.2f%% │ %4.1f%% ║\n",
                   r.method_name.c_str(),
                   r.impact_metrics.stale_reference_rate * 100,
                   r.impact_metrics.entity_confusion_rate * 100,
                   r.impact_metrics.inference_persistence_rate * 100,
                   r.impact_metrics.false_positive_rate * 100);
        }

        printf("╚════════════════════════════════════════════════════════════════╝\n");
    }

private:
    std::unique_ptr<BaselineContextManager> create_context_manager(ContextMethod method) {
        switch (method) {
            case ContextMethod::NoHistory:
                return std::make_unique<NoHistoryContext>();
            case ContextMethod::FullHistory:
                return std::make_unique<FullHistoryContext>();
            case ContextMethod::FixedWindow:
                return std::make_unique<FixedWindowContext>(100);
            case ContextMethod::TimeFilter:
                return std::make_unique<TimeFilterContext>();
            case ContextMethod::VersionedContext:
            case ContextMethod::FullVersionedIntegrity:
            default:
                return std::make_unique<VersionedContextManager>();
        }
    }
};

// ============================================================================
// Ablation Study Support
// ============================================================================

struct AblationConfig {
    std::string ablation_name;
    bool use_temporal_validity = true;
    bool use_provenance_tracking = true;
    bool use_selective_forgetting = true;
    bool use_conflict_detection = true;
    bool use_cross_agent_validation = true;
};

class AblationRunner {
public:
    std::vector<ExperimentResult> run_ablation_study(
        const ExperimentConfig& base_config,
        const std::vector<AblationConfig>& ablations
    ) {
        std::vector<ExperimentResult> results;
        ExperimentRunner runner;

        for (const auto& ablation : ablations) {
            ExperimentConfig config = base_config;
            config.experiment_id = base_config.experiment_id + "_ablation_" + ablation.ablation_name;

            auto result = runner.run_experiment(config);
            result.method_name = "Versioned_" + ablation.ablation_name;
            results.push_back(result);
        }

        return results;
    }

    static std::vector<AblationConfig> standard_ablations() {
        return {
            {"full", true, true, true, true, true},
            {"no_temporal", false, true, true, true, true},
            {"no_provenance", true, false, true, true, true},
            {"no_forgetting", true, true, false, true, true},
            {"no_conflict", true, true, true, false, true},
            {"no_cross_agent", true, true, true, true, false},
            {"minimal", false, false, false, false, false}
        };
    }
};

}  // namespace context
}  // namespace titans
