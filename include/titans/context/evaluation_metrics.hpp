/**
 * @file evaluation_metrics.hpp
 * @brief Evaluation Metrics for Context Contamination Research
 *
 * Defines metrics for measuring contamination impact and mitigation effectiveness.
 * These are the core metrics that differentiate this work from hallucination studies.
 */

#pragma once

#include "versioned_entity.hpp"
#include "contamination_injector.hpp"
#include <vector>
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace titans {
namespace context {

// ============================================================================
// Model Output Structure (for evaluation)
// ============================================================================

struct ModelOutput {
    std::string task_id;
    Timestamp output_time;

    // Classification outputs
    std::string predicted_class;
    std::string ground_truth_class;

    // Entity references
    std::vector<std::string> referenced_entities;
    std::vector<StateVersion> referenced_versions;

    // Confidence and evidence
    double confidence;
    std::vector<std::string> cited_evidence;

    // Contamination tracking
    bool used_stale_information;
    std::vector<ContaminationType> contamination_types_present;

    // Decision quality
    bool is_correct;
    bool should_have_abstained;
    bool did_abstain;
};

// ============================================================================
// Core Evaluation Metrics
// ============================================================================

/**
 * @brief Contamination Impact Metrics
 *
 * Measures how contamination affects model decisions.
 */
struct ContaminationImpactMetrics {
    // Basic accuracy
    double accuracy;
    double accuracy_clean;       // Accuracy on clean context
    double accuracy_contaminated;// Accuracy on contaminated context
    double accuracy_delta;       // Degradation due to contamination

    // Stale information usage
    double stale_reference_rate; // % of outputs that reference stale states
    double stale_decision_rate;  // % of decisions based on stale info

    // Entity binding errors
    double entity_confusion_rate;// % of outputs with wrong entity bindings

    // Inference persistence
    double inference_persistence_rate;// % of model inferences treated as facts

    // Contamination propagation
    double cross_round_propagation_rate;// Errors that persist across rounds
    double cross_agent_propagation_rate;// Errors that spread between agents

    // Decision quality under contamination
    double false_positive_rate;
    double false_negative_rate;
    double inappropriate_action_rate;

    // Abstention behavior
    double appropriate_abstention_rate;
    double missed_abstention_rate;
};

/**
 * @brief Contamination Persistence Metrics
 *
 * Measures how long contamination affects the system.
 */
struct ContaminationPersistenceMetrics {
    // Duration metrics
    Duration mean_persistence;    // Average time contamination affects decisions
    Duration max_persistence;     // Longest contamination duration
    Duration p50_persistence;
    Duration p90_persistence;
    Duration p99_persistence;

    // Recovery metrics
    Duration mean_recovery_time;  // Time to recover correct state after contamination
    double recovery_rate;         // % of contaminations eventually corrected

    // Propagation metrics
    int mean_affected_rounds;     // Average rounds affected by single contamination
    int max_affected_rounds;
    int mean_affected_entities;   // Entities affected by contamination spread
};

/**
 * @brief Mitigation Effectiveness Metrics
 *
 * Measures how well mitigation methods work.
 */
struct MitigationEffectivenessMetrics {
    // Accuracy improvement
    double accuracy_improvement;  // (mitigated - baseline) accuracy
    double relative_improvement;  // % improvement over baseline

    // Contamination reduction
    double contamination_detection_rate;
    double contamination_prevention_rate;
    double false_alarm_rate;      // Clean context incorrectly flagged

    // Per-contamination-type effectiveness
    std::unordered_map<ContaminationType, double> detection_rate_by_type;
    std::unordered_map<ContaminationType, double> prevention_rate_by_type;

    // Persistence reduction
    double persistence_reduction;  // % reduction in mean persistence

    // Cost metrics
    double context_length_overhead;// Additional context tokens needed
    double latency_overhead_ms;    // Additional processing time
    double computation_overhead;   // Additional compute (relative)
};

// ============================================================================
// Metric Calculator
// ============================================================================

class MetricCalculator {
public:
    /**
     * @brief Calculate contamination impact metrics
     */
    ContaminationImpactMetrics calculate_impact_metrics(
        const std::vector<ModelOutput>& outputs
    ) {
        ContaminationImpactMetrics metrics{};

        if (outputs.empty()) return metrics;

        int correct = 0, total = 0;
        int correct_clean = 0, total_clean = 0;
        int correct_contaminated = 0, total_contaminated = 0;
        int stale_refs = 0, stale_decisions = 0;
        int entity_confusion = 0;
        int inference_persistence = 0;
        int false_positives = 0, false_negatives = 0;
        int should_abstain = 0, did_abstain = 0, appropriate_abstain = 0;

        for (const auto& output : outputs) {
            ++total;
            if (output.is_correct) ++correct;

            bool is_contaminated = !output.contamination_types_present.empty();

            if (is_contaminated) {
                ++total_contaminated;
                if (output.is_correct) ++correct_contaminated;
            } else {
                ++total_clean;
                if (output.is_correct) ++correct_clean;
            }

            if (output.used_stale_information) {
                ++stale_refs;
                if (!output.is_correct) ++stale_decisions;
            }

            for (auto ct : output.contamination_types_present) {
                if (ct == ContaminationType::EntityBinding) ++entity_confusion;
                if (ct == ContaminationType::InferencePersistence) ++inference_persistence;
            }

            if (output.should_have_abstained) {
                ++should_abstain;
                if (output.did_abstain) ++appropriate_abstain;
            }
            if (output.did_abstain) ++did_abstain;

            // Simplified FP/FN calculation (assuming binary classification task)
            if (output.predicted_class != output.ground_truth_class) {
                if (output.predicted_class == "anomaly") ++false_positives;
                else ++false_negatives;
            }
        }

        metrics.accuracy = static_cast<double>(correct) / total;
        metrics.accuracy_clean = total_clean > 0 ?
            static_cast<double>(correct_clean) / total_clean : 0;
        metrics.accuracy_contaminated = total_contaminated > 0 ?
            static_cast<double>(correct_contaminated) / total_contaminated : 0;
        metrics.accuracy_delta = metrics.accuracy_clean - metrics.accuracy_contaminated;

        metrics.stale_reference_rate = static_cast<double>(stale_refs) / total;
        metrics.stale_decision_rate = stale_refs > 0 ?
            static_cast<double>(stale_decisions) / stale_refs : 0;

        metrics.entity_confusion_rate = static_cast<double>(entity_confusion) / total;
        metrics.inference_persistence_rate = static_cast<double>(inference_persistence) / total;

        metrics.false_positive_rate = static_cast<double>(false_positives) / total;
        metrics.false_negative_rate = static_cast<double>(false_negatives) / total;

        metrics.appropriate_abstention_rate = should_abstain > 0 ?
            static_cast<double>(appropriate_abstain) / should_abstain : 0;
        metrics.missed_abstention_rate = should_abstain > 0 ?
            1.0 - metrics.appropriate_abstention_rate : 0;

        return metrics;
    }

    /**
     * @brief Calculate persistence metrics from contamination events
     */
    ContaminationPersistenceMetrics calculate_persistence_metrics(
        const std::vector<ContaminationEvent>& events,
        const std::vector<ModelOutput>& outputs
    ) {
        ContaminationPersistenceMetrics metrics{};

        if (events.empty()) return metrics;

        std::vector<Duration> persistence_durations;
        std::vector<int> affected_rounds;

        // Track each contamination's lifetime
        for (const auto& event : events) {
            Duration persistence = 0;
            int rounds_affected = 0;

            // Find outputs affected by this contamination
            for (const auto& output : outputs) {
                if (output.output_time >= event.detected_at) {
                    for (auto ct : output.contamination_types_present) {
                        if (ct == event.type) {
                            ++rounds_affected;
                            persistence = output.output_time - event.detected_at;
                            break;
                        }
                    }
                }
            }

            persistence_durations.push_back(persistence);
            affected_rounds.push_back(rounds_affected);
        }

        // Calculate statistics
        if (!persistence_durations.empty()) {
            std::sort(persistence_durations.begin(), persistence_durations.end());

            metrics.mean_persistence = std::accumulate(
                persistence_durations.begin(), persistence_durations.end(), 0LL
            ) / persistence_durations.size();

            metrics.max_persistence = persistence_durations.back();
            metrics.p50_persistence = persistence_durations[persistence_durations.size() / 2];
            metrics.p90_persistence = persistence_durations[persistence_durations.size() * 90 / 100];
            metrics.p99_persistence = persistence_durations[persistence_durations.size() * 99 / 100];
        }

        if (!affected_rounds.empty()) {
            metrics.mean_affected_rounds = std::accumulate(
                affected_rounds.begin(), affected_rounds.end(), 0
            ) / affected_rounds.size();
            metrics.max_affected_rounds = *std::max_element(
                affected_rounds.begin(), affected_rounds.end()
            );
        }

        return metrics;
    }

    /**
     * @brief Calculate mitigation effectiveness by comparing baseline vs mitigated
     */
    MitigationEffectivenessMetrics calculate_mitigation_effectiveness(
        const ContaminationImpactMetrics& baseline_metrics,
        const ContaminationImpactMetrics& mitigated_metrics,
        const ContaminationPersistenceMetrics& baseline_persistence,
        const ContaminationPersistenceMetrics& mitigated_persistence,
        double context_overhead = 0,
        double latency_overhead = 0
    ) {
        MitigationEffectivenessMetrics metrics{};

        // Accuracy improvement
        metrics.accuracy_improvement = mitigated_metrics.accuracy - baseline_metrics.accuracy;
        metrics.relative_improvement = baseline_metrics.accuracy > 0 ?
            metrics.accuracy_improvement / (1.0 - baseline_metrics.accuracy) : 0;

        // Contamination reduction
        metrics.contamination_detection_rate = 1.0 - (
            mitigated_metrics.stale_reference_rate / std::max(0.001, baseline_metrics.stale_reference_rate)
        );
        metrics.contamination_prevention_rate = 1.0 - (
            mitigated_metrics.stale_decision_rate / std::max(0.001, baseline_metrics.stale_decision_rate)
        );

        // Persistence reduction
        if (baseline_persistence.mean_persistence > 0) {
            metrics.persistence_reduction = 1.0 - (
                static_cast<double>(mitigated_persistence.mean_persistence) /
                baseline_persistence.mean_persistence
            );
        }

        // Cost metrics
        metrics.context_length_overhead = context_overhead;
        metrics.latency_overhead_ms = latency_overhead;

        return metrics;
    }
};

// ============================================================================
// Experiment Result Structure
// ============================================================================

struct ExperimentResult {
    std::string experiment_id;
    std::string method_name;
    std::string model_name;
    Timestamp start_time;
    Timestamp end_time;

    // Configuration
    size_t num_events;
    size_t num_entities;
    double contamination_rate;

    // Results
    ContaminationImpactMetrics impact_metrics;
    ContaminationPersistenceMetrics persistence_metrics;
    MitigationEffectivenessMetrics mitigation_metrics;  // Only if comparing to baseline

    // System metrics
    double avg_latency_ms;
    double p99_latency_ms;
    size_t avg_context_tokens;
    double gpu_utilization;
    double cpu_utilization;

    void print_summary() const {
        printf("\n=== Experiment: %s ===\n", experiment_id.c_str());
        printf("Method: %s, Model: %s\n", method_name.c_str(), model_name.c_str());
        printf("Events: %zu, Entities: %zu, Contamination Rate: %.2f%%\n",
               num_events, num_entities, contamination_rate * 100);

        printf("\n--- Impact Metrics ---\n");
        printf("Accuracy: %.2f%% (clean: %.2f%%, contaminated: %.2f%%)\n",
               impact_metrics.accuracy * 100,
               impact_metrics.accuracy_clean * 100,
               impact_metrics.accuracy_contaminated * 100);
        printf("Accuracy Delta: %.2f%%\n", impact_metrics.accuracy_delta * 100);
        printf("Stale Reference Rate: %.2f%%\n", impact_metrics.stale_reference_rate * 100);
        printf("Entity Confusion Rate: %.2f%%\n", impact_metrics.entity_confusion_rate * 100);

        printf("\n--- Persistence Metrics ---\n");
        printf("Mean Persistence: %.2f sec\n", persistence_metrics.mean_persistence / 1e9);
        printf("P99 Persistence: %.2f sec\n", persistence_metrics.p99_persistence / 1e9);
        printf("Mean Affected Rounds: %d\n", persistence_metrics.mean_affected_rounds);

        printf("\n--- System Metrics ---\n");
        printf("Avg Latency: %.2f ms, P99: %.2f ms\n", avg_latency_ms, p99_latency_ms);
        printf("Avg Context Tokens: %zu\n", avg_context_tokens);
    }
};

// ============================================================================
// Baseline Methods for Comparison
// ============================================================================

enum class ContextMethod {
    NoHistory,              // Only current event
    FullHistory,            // Keep everything
    FixedWindow,            // Last N tokens
    RollingSummary,         // Compress old context
    VectorRetrieval,        // Semantic retrieval
    TimeFilter,             // Drop old by time
    VersionedContext,       // Our method
    FullVersionedIntegrity  // Our full method with all features
};

std::string method_name(ContextMethod m) {
    switch (m) {
        case ContextMethod::NoHistory: return "NoHistory";
        case ContextMethod::FullHistory: return "FullHistory";
        case ContextMethod::FixedWindow: return "FixedWindow";
        case ContextMethod::RollingSummary: return "RollingSummary";
        case ContextMethod::VectorRetrieval: return "VectorRetrieval";
        case ContextMethod::TimeFilter: return "TimeFilter";
        case ContextMethod::VersionedContext: return "VersionedContext";
        case ContextMethod::FullVersionedIntegrity: return "VersionedIntegrity";
        default: return "Unknown";
    }
}

}  // namespace context
}  // namespace titans
