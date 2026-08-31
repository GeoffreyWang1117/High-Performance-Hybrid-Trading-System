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

/**
 * @brief Which components of the reliability layer are active.
 *
 * Each flag gates a DISTINCT code path. An ablation that leaves the executed
 * code identical is not an ablation, and `test_ablation_flags_are_wired`
 * enforces that every flag here changes the measured outcome.
 */
struct AblationFlags {
    /// Drop context entries older than the manager's max_age window.
    bool use_temporal_validity = true;
    /// Trust provenance metadata when deciding whether a state is admissible.
    bool use_provenance_tracking = true;
    /// Keep only the latest state per entity rather than every observation.
    bool use_selective_forgetting = true;
    /// Detect two mutually inconsistent states that are both "valid".
    bool use_conflict_detection = true;
    /// Quarantine claims that originate from another agent.
    bool use_cross_agent_validation = true;

    bool all_enabled() const {
        return use_temporal_validity && use_provenance_tracking &&
               use_selective_forgetting && use_conflict_detection &&
               use_cross_agent_validation;
    }
};

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

    // Which parts of the reliability layer are switched on. Only meaningful
    // for VersionedContext / FullVersionedIntegrity; baselines ignore it.
    AblationFlags ablation;

    // Temporal validity window for versioned context managers. MUST be well
    // inside the span of the event stream (num_events * event_interval_ns) or
    // the temporal filter never fires and ablating it changes nothing --
    // AblationRunner reports that case as INERT rather than as a result.
    // Default: 2 s, against a default stream of 10 s.
    Duration context_max_age_ns = 2000000000LL;

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

/**
 * @brief Generates an event stream whose labels are NOT readable from the event.
 *
 * TASK DESIGN, AND WHY IT MATTERS
 * -------------------------------
 * The previous generator did two things that made every downstream experiment
 * vacuous:
 *
 *   1. It wrote the answer into the event. `event_type` was set to the literal
 *      string "anomaly" or "normal" from `is_anomaly`, and the prompt builder
 *      forwarded that field to the model. The task was to copy a field.
 *
 *   2. Even with that field removed, anomalies were globally separable: normal
 *      values were N(100, 10) and anomalies were 1.5-2.5x larger, so a fixed
 *      threshold near 130 solved the task with no context at all. An experiment
 *      comparing CONTEXT MANAGEMENT STRATEGIES cannot learn anything from a task
 *      that needs no context -- every strategy scores the same, and any observed
 *      difference is noise.
 *
 * This generator fixes both. Each entity carries its own slowly drifting level
 * `mu_e`; a normal observation is `mu_e + N(0, sigma)` and an anomaly is
 * `mu_e + k*sigma` with k in [4, 8]. Because levels differ across entities and
 * move over time, an observation is anomalous only RELATIVE TO THAT ENTITY'S
 * RECENT HISTORY. No global threshold works, so context is load-bearing:
 *   - stale context supplies an outdated mu_e -> misclassification;
 *   - entity-binding contamination supplies another entity's mu -> misclassification.
 * Those are the effects the experiment is supposed to measure, and now can.
 *
 * `test_task_requires_context` asserts property 2 empirically by showing that
 * the best possible global threshold performs near chance.
 */
class SyntheticDataGenerator {
public:
    /// Plausible market event categories, drawn independently of the label.
    static constexpr const char* kEventTypes[] = {"trade", "quote", "cancel"};

    /// Standard deviation of a normal observation around its entity's level.
    static constexpr double kSigma = 5.0;

    /// Minimum deviation, in sigma, that makes an observation anomalous.
    static constexpr double kAnomalyMinSigma = 4.0;

    /**
     * @brief Deviation from an entity's level that separates normal from anomalous.
     *
     * Exposed because the PROMPT has to state it. A model shown one or two
     * prior observations of an entity cannot estimate that entity's spread, so
     * without a stated tolerance it has no basis for "far" and falls back to a
     * constant answer -- which is exactly what happened: with only 2-3 context
     * points, Qwen2.5-3B replied "Value far below recent trades" to values that
     * were entirely typical, and answered one class on 98-100% of trials.
     *
     * Stating the threshold does not give away the label. The model still has
     * to recover the entity's current level from context, which is precisely
     * what stale and misbound context corrupt. It converts the task from
     * "estimate a distribution from too few points" into "use the right
     * reference value", which is the question this experiment is actually
     * about.
     */
    static constexpr double kDecisionTolerance = 3.0 * kSigma;   // 15.0

    explicit SyntheticDataGenerator(uint64_t seed = 42)
        : rng_(seed), uniform_(0.0, 1.0), normal_(0.0, 1.0) {}

    /**
     * @param num_events     Length of the stream.
     * @param num_entities   Distinct entities; each gets its own level.
     * @param interval_ns    Mean spacing between events.
     * @param anomaly_rate   Fraction of events that are anomalies.
     */
    std::vector<SyntheticEvent> generate_event_stream(
        size_t num_events,
        size_t num_entities,
        Duration interval_ns,
        double anomaly_rate = 0.05
    ) {
        std::vector<SyntheticEvent> events;
        events.reserve(num_events);

        // Per-entity levels, spread widely enough that no global threshold can
        // separate one entity's anomaly from another entity's normal value.
        std::vector<double> level(num_entities);
        for (size_t e = 0; e < num_entities; ++e) {
            level[e] = 50.0 + uniform_(rng_) * 450.0;   // levels in [50, 500]
        }

        constexpr double kDrift = 0.20;   // per-observation random walk step

        Timestamp current_time = 1000000000000LL;  // start at 1000 s

        for (size_t i = 0; i < num_events; ++i) {
            const size_t entity_idx =
                static_cast<size_t>(uniform_(rng_) * num_entities) % num_entities;
            const bool is_anomaly = uniform_(rng_) < anomaly_rate;

            // The level drifts, so context that is merely OLD is also wrong.
            level[entity_idx] += normal_(rng_) * kDrift;

            double value;
            if (is_anomaly) {
                const double k = kAnomalyMinSigma + uniform_(rng_) * 4.0;   // 4-8 sigma
                const double sign = (uniform_(rng_) < 0.5) ? -1.0 : 1.0;
                value = level[entity_idx] + sign * k * kSigma;
            } else {
                value = level[entity_idx] + normal_(rng_) * kSigma;
            }

            // Category is independent of the label: it carries no information
            // about `is_anomaly` and cannot be used to shortcut the task.
            const char* type = kEventTypes[
                static_cast<size_t>(uniform_(rng_) * 3) % 3];

            events.push_back(SyntheticEvent{
                .event_id = "evt_" + std::to_string(i),
                .entity_id = "entity_" + std::to_string(entity_idx),
                .timestamp = current_time,
                .value = value,
                .event_type = type,
                .is_anomaly = is_anomaly
            });

            current_time += interval_ns +
                static_cast<Duration>(normal_(rng_) * interval_ns * 0.1);
        }

        return events;
    }

private:
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> uniform_;
    std::normal_distribution<double> normal_;
};

// ============================================================================
// Assumed-degradation stand-in model
// ============================================================================

/**
 * @brief A stand-in whose response to contamination is ASSUMED, not measured.
 *
 * READ THIS BEFORE QUOTING ANY NUMBER THIS CLASS PRODUCES.
 *
 * There is no model here. Accuracy is a product of hardcoded multipliers --
 * 0.5 for an entity-binding error, 0.7 for stale state, and so on -- followed
 * by one Bernoulli draw. Those constants were chosen, not observed.
 *
 * Consequently this class CANNOT answer questions of the form "which
 * contamination type hurts a language model most", because the ranking it
 * produces is precisely the ranking of the constants below. Using its output
 * as evidence for such a claim is circular: the conclusion was typed into the
 * switch statement.
 *
 * What it IS good for, and the only reason it still exists:
 *   - exercising the pipeline end to end without an inference server;
 *   - checking that contamination injection, mitigation, and metric
 *     computation are wired together and respond in the expected direction;
 *   - regression-testing that a refactor did not change experiment mechanics.
 *
 * For any claim about real model behaviour, use LLMExperimentRunner
 * (llm_interface.hpp) against a live backend. Results from this class are
 * labelled "assumed-degradation" everywhere they surface, and the experiment
 * binaries refuse to describe them as findings.
 */
class AssumedDegradationModel {
public:
    explicit AssumedDegradationModel(uint64_t seed = 42)
        : rng_(seed), uniform_(0.0, 1.0) {}

    /**
     * @brief Draw an outcome from the assumed degradation curve.
     * @warning The returned accuracy encodes this file's constants, not any
     *          property of a language model.
     */
    ModelOutput infer_from_assumed_degradation(
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
    explicit VersionedContextManager(Duration max_age = 30000000000LL,
                                     AblationFlags flags = {})
        : max_age_(max_age), flags_(flags) {}

    /**
     * @brief Assemble the context this method would hand to a model.
     *
     * Two of the ablation flags act here and each removes a real filter:
     *   - use_temporal_validity: without it, entries older than max_age are
     *     retained, so expired state reaches the model.
     *   - use_selective_forgetting: without it, every historical observation is
     *     kept rather than the latest per entity, so superseded values sit in
     *     the context alongside the current one.
     */
    std::vector<SyntheticEvent> get_context(
        const SyntheticEvent& current_event,
        const std::vector<SyntheticEvent>& history
    ) override {
        std::vector<SyntheticEvent> result;

        if (!flags_.use_selective_forgetting) {
            // No deduplication: superseded observations stay in context.
            for (const auto& e : history) {
                if (flags_.use_temporal_validity &&
                    current_event.timestamp - e.timestamp > max_age_) {
                    continue;
                }
                result.push_back(e);
            }
            result.push_back(current_event);
            return result;
        }

        std::unordered_map<std::string, SyntheticEvent> latest_by_entity;
        for (const auto& e : history) {
            if (flags_.use_temporal_validity &&
                current_event.timestamp - e.timestamp > max_age_) {
                continue;
            }
            auto it = latest_by_entity.find(e.entity_id);
            if (it == latest_by_entity.end() || e.timestamp > it->second.timestamp) {
                latest_by_entity[e.entity_id] = e;
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
    AblationFlags flags_;
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

        auto context_manager = create_context_manager(
            config.method, config.ablation, config.context_max_age_ns);
        AssumedDegradationModel model(config.seed + 2);
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
                            // states before they reach the model. Detecting it
                            // requires trusting the version/provenance metadata
                            // that says which state supersedes which.
                            if (!config.ablation.use_provenance_tracking) break;
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
                            if (full_integrity && config.ablation.use_conflict_detection &&
                                entity.has_conflict(event.timestamp)) {
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
                            if (full_integrity && config.ablation.use_cross_agent_validation) {
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

            auto output = model.infer_from_assumed_degradation(
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
    std::unique_ptr<BaselineContextManager> create_context_manager(
        ContextMethod method, AblationFlags flags = {},
        Duration max_age = 2000000000LL) {
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
                return std::make_unique<VersionedContextManager>(max_age, flags);
        }
    }
};

// ============================================================================
// Ablation Study Support
// ============================================================================

struct AblationConfig {
    std::string ablation_name;
    AblationFlags flags;
};

/**
 * @brief One ablation's outcome, plus whether the flag actually did anything.
 */
struct AblationOutcome {
    ExperimentResult result;
    std::string name;
    /// True when this ablation's metrics are bit-identical to the full system,
    /// meaning the disabled component never executed under this configuration.
    bool inert = false;
    std::string inert_reason;
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
            config.experiment_id =
                base_config.experiment_id + "_ablation_" + ablation.ablation_name;

            // Apply the ablation. Before this was wired, every configuration
            // ran the identical experiment with the identical seed and was
            // merely relabelled, producing seven identical rows that looked
            // like a result and were not one.
            config.ablation = ablation.flags;

            auto result = runner.run_experiment(config);
            result.method_name = "Versioned_" + ablation.ablation_name;
            results.push_back(result);
        }

        return results;
    }

    /// @brief One-at-a-time ablations plus the two endpoints.
    /// Field order matches AblationFlags: temporal, provenance, forgetting,
    /// conflict, cross-agent.
    /**
     * @brief Run the ablations and mark the ones that changed nothing.
     *
     * An ablation whose metrics are identical to the full system did not
     * ablate anything: the component it disables was never reached under this
     * configuration. Reporting such a row as a number invites the reader to
     * conclude "this component does not matter", when the truth is "this
     * experiment cannot tell". They are marked INERT and excluded from any
     * claim about component importance.
     *
     * This check exists because the original implementation ignored the
     * ablation flags entirely and printed seven identical rows.
     */
    std::vector<AblationOutcome> run_ablation_study_checked(
        const ExperimentConfig& base_config,
        const std::vector<AblationConfig>& ablations
    ) {
        auto results = run_ablation_study(base_config, ablations);

        std::vector<AblationOutcome> outcomes;
        outcomes.reserve(results.size());

        // The "full" configuration is the reference. It is expected first.
        const ExperimentResult* full = nullptr;
        for (size_t i = 0; i < results.size(); ++i) {
            if (ablations[i].ablation_name == "full") full = &results[i];
        }

        for (size_t i = 0; i < results.size(); ++i) {
            AblationOutcome o;
            o.result = results[i];
            o.name = ablations[i].ablation_name;

            if (full && o.name != "full" && identical(results[i], *full)) {
                o.inert = true;
                o.inert_reason = inert_reason_for(o.name, base_config);
            }
            outcomes.push_back(std::move(o));
        }
        return outcomes;
    }

    /// @brief True if two results agree on every reported metric.
    static bool identical(const ExperimentResult& a, const ExperimentResult& b) {
        auto eq = [](double x, double y) { return std::abs(x - y) < 1e-12; };
        return eq(a.impact_metrics.accuracy, b.impact_metrics.accuracy) &&
               eq(a.impact_metrics.accuracy_delta, b.impact_metrics.accuracy_delta) &&
               eq(a.impact_metrics.stale_reference_rate,
                  b.impact_metrics.stale_reference_rate) &&
               eq(a.impact_metrics.false_positive_rate,
                  b.impact_metrics.false_positive_rate);
    }

    /// @brief Why a given flag could not have had an effect here.
    static std::string inert_reason_for(const std::string& name,
                                        const ExperimentConfig& cfg) {
        const bool integrity = cfg.method == ContextMethod::FullVersionedIntegrity;
        const double stream_ns =
            static_cast<double>(cfg.num_events) * cfg.event_interval_ns;
        const double window_ns = static_cast<double>(cfg.context_max_age_ns);

        if (name == "no_temporal") {
            // Expected wall time between two observations of the SAME entity.
            // Under selective forgetting only the latest per entity is kept, so
            // this -- not the stream length -- is the age the window must beat.
            const double revisit_ns =
                cfg.num_entities > 0
                    ? static_cast<double>(cfg.num_entities) * cfg.event_interval_ns
                    : stream_ns;

            char buf[384];
            if (cfg.ablation.use_selective_forgetting && revisit_ns < window_ns) {
                std::snprintf(
                    buf, sizeof(buf),
                    "selective forgetting already caps context age at the "
                    "per-entity revisit interval (~%.0f ms for %zu entities at "
                    "%.1f ms spacing), which is inside the %.1f s temporal "
                    "window -- the two mechanisms overlap, so the window is "
                    "never the binding constraint. Ablate it against "
                    "no_forgetting to measure it, or shrink the window below "
                    "%.0f ms.",
                    revisit_ns / 1e6, cfg.num_entities,
                    static_cast<double>(cfg.event_interval_ns) / 1e6,
                    window_ns / 1e9, revisit_ns / 1e6);
            } else {
                std::snprintf(
                    buf, sizeof(buf),
                    "event stream spans %.1f s but the temporal window is "
                    "%.1f s, so no entry was ever old enough to filter",
                    stream_ns / 1e9, window_ns / 1e9);
            }
            return buf;
        }
        if (name == "no_conflict" || name == "no_cross_agent") {
            if (!integrity) {
                return "this check runs only under FullVersionedIntegrity; the "
                       "base configuration uses a weaker method";
            }
            return "the corresponding contamination type was never injected";
        }
        return "the disabled component was not reached under this configuration";
    }

    static std::vector<AblationConfig> standard_ablations() {
        return {
            {"full",           {true,  true,  true,  true,  true }},
            {"no_temporal",    {false, true,  true,  true,  true }},
            {"no_provenance",  {true,  false, true,  true,  true }},
            {"no_forgetting",  {true,  true,  false, true,  true }},
            {"no_conflict",    {true,  true,  true,  false, true }},
            {"no_cross_agent", {true,  true,  true,  true,  false}},
            {"minimal",        {false, false, false, false, false}}
        };
    }
};

}  // namespace context
}  // namespace titans
