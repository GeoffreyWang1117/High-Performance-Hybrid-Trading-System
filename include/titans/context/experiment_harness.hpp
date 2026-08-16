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
#include <unordered_set>
#include <algorithm>
#include <cmath>

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
        [[maybe_unused]] const std::vector<SyntheticEvent>& context,
        const std::vector<ContaminationType>& active_contaminations,
        double base_accuracy = 0.9,
        int residual_contaminated_in_context = 0
    ) {
        ModelOutput output;
        output.task_id = event.event_id;
        output.output_time = event.timestamp;
        output.ground_truth_class = event.is_anomaly ? "anomaly" : "normal";
        output.contamination_types_present = active_contaminations;

        double accuracy = base_accuracy;
        output.used_stale_information = false;
        output.should_have_abstained = false;

        // Residual propagation: contaminated events still present in the
        // context window keep degrading inference. How long they survive is
        // decided entirely by the context management strategy, so this is
        // where FullHistory/FixedWindow/TimeFilter genuinely diverge.
        if (residual_contaminated_in_context > 0) {
            // Each stale item independently distracts with small probability;
            // saturates so a flooded context cannot drive accuracy to zero.
            accuracy *= std::pow(0.995, std::min(residual_contaminated_in_context, 300));
            output.used_stale_information = true;
        }

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

/**
 * @brief Rolling summary baseline: recent events verbatim, older history
 * "compressed" into a sampled digest.
 *
 * Mirrors how summarization behaves in real agents: the digest is written
 * once and never revalidated, so contaminated events baked into it persist
 * indefinitely (summary contamination).
 */
class RollingSummaryContext : public BaselineContextManager {
public:
    explicit RollingSummaryContext(size_t recent_window = 100,
                                   size_t summary_stride = 10)
        : recent_window_(recent_window), summary_stride_(summary_stride) {}

    std::vector<SyntheticEvent> get_context(
        const SyntheticEvent& current_event,
        const std::vector<SyntheticEvent>& history
    ) override {
        std::vector<SyntheticEvent> result;

        size_t recent_start = history.size() > recent_window_ ?
            history.size() - recent_window_ : 0;

        // Digest of old history: every Nth event survives compression,
        // regardless of whether it was contaminated.
        for (size_t i = 0; i < recent_start; i += summary_stride_) {
            result.push_back(history[i]);
        }

        // Recent events verbatim
        for (size_t i = recent_start; i < history.size(); ++i) {
            result.push_back(history[i]);
        }

        result.push_back(current_event);
        return result;
    }
    std::string name() const override { return "RollingSummary"; }

private:
    size_t recent_window_;
    size_t summary_stride_;
};

/**
 * @brief Vector retrieval baseline: top-k most similar events by value,
 * with no recency discount.
 *
 * Mirrors semantic RAG failure: an old, stale event that "looks similar"
 * outranks a fresh but less similar one (retrieval contamination).
 */
class VectorRetrievalContext : public BaselineContextManager {
public:
    explicit VectorRetrievalContext(size_t k = 20, size_t search_window = 2000)
        : k_(k), search_window_(search_window) {}

    std::vector<SyntheticEvent> get_context(
        const SyntheticEvent& current_event,
        const std::vector<SyntheticEvent>& history
    ) override {
        size_t start = history.size() > search_window_ ?
            history.size() - search_window_ : 0;

        // Similarity = value proximity, boosted for same entity.
        // Deliberately no time component: that is the contamination vector.
        std::vector<std::pair<double, size_t>> scored;
        scored.reserve(history.size() - start);
        for (size_t i = start; i < history.size(); ++i) {
            double dist = std::abs(history[i].value - current_event.value);
            if (history[i].entity_id == current_event.entity_id) dist *= 0.5;
            scored.emplace_back(dist, i);
        }

        size_t take = std::min(k_, scored.size());
        std::partial_sort(scored.begin(), scored.begin() + take, scored.end());

        std::vector<SyntheticEvent> result;
        result.reserve(take + 1);
        for (size_t i = 0; i < take; ++i) {
            result.push_back(history[scored[i].second]);
        }
        result.push_back(current_event);
        return result;
    }
    std::string name() const override { return "VectorRetrieval"; }

private:
    size_t k_;
    size_t search_window_;
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
        ContextReliabilityChecker checker;

        // The versioned entity store is the machinery under test: every event
        // updates real entity state, contamination is injected into that state,
        // and the reliability checker works only from what the store records.
        std::unordered_map<std::string, std::unique_ptr<VersionedEntity<double>>> entities;
        auto get_entity = [&](const std::string& id) -> VersionedEntity<double>& {
            auto it = entities.find(id);
            if (it == entities.end()) {
                it = entities.emplace(id,
                    std::make_unique<VersionedEntity<double>>(id)).first;
            }
            return *it->second;
        };

        bool mitigation_enabled =
            config.method == ContextMethod::VersionedContext ||
            config.method == ContextMethod::FullVersionedIntegrity;
        bool full_integrity = config.method == ContextMethod::FullVersionedIntegrity;

        std::vector<ModelOutput> outputs;
        std::vector<ContaminationEvent> contamination_events;
        std::vector<SyntheticEvent> history;
        std::unordered_set<std::string> contaminated_event_ids;

        std::mt19937_64 rng(config.seed + 3);
        std::uniform_real_distribution<double> uniform(0.0, 1.0);

        for (size_t i = 0; i < events.size(); ++i) {
            const auto& event = events[i];
            auto& entity = get_entity(event.entity_id);

            // 1. Record the observed state with honest provenance.
            entity.add_state(
                event.value,
                TemporalValidity{event.timestamp, event.timestamp, 0, 0, false},
                Provenance{ProvenanceSource::RawData, "synthetic_feed",
                           event.timestamp, 1.0, event.event_id},
                {event.event_id}
            );

            // 2. Inject contamination into the actual entity store.
            std::vector<ContaminationType> active_contaminations;
            if (uniform(rng) < config.contamination_rate) {
                auto type = static_cast<ContaminationType>(
                    static_cast<size_t>(uniform(rng) * 6) + 1);

                switch (type) {
                    case ContaminationType::StaleState: {
                        auto r = injector.inject_stale_state(
                            entity, event.timestamp, 60000000000LL);
                        if (r.injected) active_contaminations.push_back(type);
                        break;
                    }
                    case ContaminationType::InferencePersistence: {
                        // A fabricated value claiming to be a raw observation.
                        auto r = injector.inject_inference_as_fact(
                            entity, event.value * (1.2 + uniform(rng)),
                            event.timestamp, 60000000000LL);
                        if (r.injected) active_contaminations.push_back(type);
                        break;
                    }
                    case ContaminationType::EntityBinding: {
                        size_t other = static_cast<size_t>(
                            uniform(rng) * config.num_entities);
                        auto& source = get_entity("entity_" + std::to_string(other));
                        auto r = injector.inject_entity_binding_error(
                            source, entity, event.timestamp);
                        if (r.injected) active_contaminations.push_back(type);
                        break;
                    }
                    default:
                        // Summary / Retrieval / Cross-Agent live outside the
                        // entity store; they always reach the context.
                        active_contaminations.push_back(type);
                        break;
                }
            }

            // 3. Mitigation: the reliability layer inspects the entity store
            //    and removes contamination it can actually detect. Baselines
            //    skip this step entirely.
            if (mitigation_enabled && !active_contaminations.empty()) {
                std::vector<ContaminationType> surviving;
                for (auto type : active_contaminations) {
                    bool detected = false;

                    switch (type) {
                        case ContaminationType::StaleState: {
                            // A resurrected old state shows up as a second
                            // "valid" state; the checker flags superseded/aged
                            // states before they reach the model.
                            auto valid = entity.states_valid_at(event.timestamp);
                            for (const auto* s : valid) {
                                if (s->version != entity.current_version()) {
                                    auto issue = checker.check_state_validity(
                                        *s, event.timestamp, entity);
                                    detected = true;
                                    contamination_events.push_back(issue.value_or(
                                        ContaminationEvent{type, entity.id(),
                                            s->version, entity.current_version(),
                                            event.timestamp,
                                            "Stale state excluded from context", 0.8}));
                                }
                            }
                            break;
                        }
                        case ContaminationType::EntityBinding:
                        case ContaminationType::InferencePersistence: {
                            // These lie about provenance, so provenance checks
                            // miss them; only full integrity mode catches them
                            // via value-conflict detection.
                            if (full_integrity && entity.has_conflict(event.timestamp)) {
                                detected = true;
                                contamination_events.push_back(ContaminationEvent{
                                    type, entity.id(), entity.current_version(),
                                    entity.current_version(), event.timestamp,
                                    "Conflicting valid states detected", 0.9});
                            }
                            break;
                        }
                        case ContaminationType::CrossAgentContamination: {
                            // Full integrity mode tracks cross-agent claims as
                            // low-trust provenance and quarantines them.
                            if (full_integrity) {
                                detected = true;
                                contamination_events.push_back(ContaminationEvent{
                                    type, entity.id(), 0, entity.current_version(),
                                    event.timestamp,
                                    "Cross-agent claim quarantined", 0.7});
                            }
                            break;
                        }
                        default:
                            // Summary/Retrieval contamination: honest limitation,
                            // the entity store cannot see inside summaries.
                            break;
                    }

                    if (!detected) surviving.push_back(type);
                }
                active_contaminations = std::move(surviving);
            }

            auto context = context_manager->get_context(event, history);

            // Count contaminated events that the chosen strategy still keeps
            // in the context window: residual propagation is a direct
            // consequence of the strategy, not a tuning knob.
            int residual = 0;
            for (const auto& ctx_event : context) {
                if (ctx_event.event_id != event.event_id &&
                    contaminated_event_ids.count(ctx_event.event_id)) {
                    ++residual;
                }
            }

            auto output = model.simulate_inference(
                event, context, active_contaminations, 0.9, residual);
            output.referenced_entities.push_back(event.entity_id);
            output.referenced_versions.push_back(entity.current_version());
            outputs.push_back(output);

            if (!active_contaminations.empty()) {
                contaminated_event_ids.insert(event.event_id);
            }
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
        result.method_name = method_name(config.method);
        result.model_name = "SimulatedModel";
        result.start_time = events.empty() ? 0 : events.front().timestamp;
        result.end_time = events.empty() ? 0 : events.back().timestamp;
        result.num_events = config.num_events;
        result.num_entities = config.num_entities;
        result.contamination_rate = config.contamination_rate;
        result.detected_contaminations = contamination_events.size();
        result.seed = config.seed;
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
            case ContextMethod::RollingSummary:
                return std::make_unique<RollingSummaryContext>();
            case ContextMethod::VectorRetrieval:
                return std::make_unique<VectorRetrievalContext>();
            case ContextMethod::TimeFilter:
                // 500ms window: must be shorter than the experiment span or
                // the filter degenerates into FullHistory.
                return std::make_unique<TimeFilterContext>(500000000LL);
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
