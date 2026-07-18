/**
 * @file contamination_injector.hpp
 * @brief Controlled Context Contamination Injection Framework
 *
 * Core research contribution: Systematic injection of various
 * contamination types for reproducible experiments.
 */

#pragma once

#include "versioned_entity.hpp"
#include <random>
#include <functional>

namespace titans {
namespace context {

// ============================================================================
// Contamination Injection Configuration
// ============================================================================

struct ContaminationConfig {
    ContaminationType type;
    double probability;          // 0.0 - 1.0
    double severity_mean;        // Mean severity
    double severity_stddev;      // Severity variation
    Duration delay_before_effect;// Time before contamination appears
    Duration persistence;        // How long contamination lasts
    bool propagate_cross_agent;  // Allow cross-agent spread
    std::string description;
};

struct InjectionResult {
    bool injected;
    ContaminationType type;
    std::string affected_entity;
    StateVersion injected_version;
    Timestamp injection_time;
    std::string description;
};

// ============================================================================
// Contamination Injector
// ============================================================================

/**
 * @brief Systematic contamination injection for experiments
 *
 * Supports all 6 contamination types defined in the research:
 * 1. Stale-State Contamination
 * 2. Entity-Binding Contamination
 * 3. Inference Persistence
 * 4. Summary Contamination
 * 5. Retrieval Contamination
 * 6. Cross-Agent Contamination
 */
class ContaminationInjector {
public:
    explicit ContaminationInjector(uint64_t seed = 42)
        : rng_(seed), uniform_(0.0, 1.0), normal_(0.0, 1.0) {}

    /**
     * @brief Register a contamination configuration
     */
    void register_contamination(const ContaminationConfig& config) {
        configs_.push_back(config);
    }

    /**
     * @brief Clear all configurations
     */
    void clear_configurations() {
        configs_.clear();
    }

    // ========================================================================
    // Type 1: Stale-State Contamination
    // ========================================================================

    /**
     * @brief Inject stale state by not marking old state as superseded
     */
    template <typename T>
    InjectionResult inject_stale_state(
        VersionedEntity<T>& entity,
        Timestamp current_time,
        Duration stale_duration
    ) {
        const auto& history = entity.history();
        if (history.size() < 2) {
            return {false, ContaminationType::StaleState, entity.id(), 0, current_time, "Not enough history"};
        }

        // Find an old state and artificially extend its validity
        size_t old_idx = history.size() - 2;
        auto& old_state = const_cast<VersionedState<T>&>(history[old_idx]);

        // Remove superseded flag (contamination!)
        old_state.validity.is_superseded = false;
        old_state.validity.effective_until = current_time + stale_duration;

        log_injection(ContaminationType::StaleState, entity.id(), old_state.version, current_time);

        return {
            true,
            ContaminationType::StaleState,
            entity.id(),
            old_state.version,
            current_time,
            "Extended validity of stale state v" + std::to_string(old_state.version)
        };
    }

    // ========================================================================
    // Type 2: Entity-Binding Contamination
    // ========================================================================

    /**
     * @brief Inject wrong entity binding by swapping entity IDs
     */
    template <typename T>
    InjectionResult inject_entity_binding_error(
        VersionedEntity<T>& source_entity,
        VersionedEntity<T>& target_entity,
        Timestamp current_time
    ) {
        const auto* source_state = source_entity.current_state(current_time);
        if (!source_state) {
            return {false, ContaminationType::EntityBinding, target_entity.id(), 0, current_time, "No source state"};
        }

        // Copy source entity's state to target entity (contamination!)
        auto modified_provenance = source_state->provenance;
        modified_provenance.source = ProvenanceSource::Unknown;  // Hide the error source

        StateVersion injected_v = target_entity.add_state(
            source_state->value,
            source_state->validity,
            modified_provenance,
            {"INJECTED_ENTITY_BINDING_ERROR"}
        );

        log_injection(ContaminationType::EntityBinding, target_entity.id(), injected_v, current_time);

        return {
            true,
            ContaminationType::EntityBinding,
            target_entity.id(),
            injected_v,
            current_time,
            "Bound " + source_entity.id() + "'s state to " + target_entity.id()
        };
    }

    // ========================================================================
    // Type 3: Inference Persistence
    // ========================================================================

    /**
     * @brief Inject persistent model inference as fact
     */
    template <typename T>
    InjectionResult inject_inference_as_fact(
        VersionedEntity<T>& entity,
        const T& inferred_value,
        Timestamp current_time,
        Duration persistence
    ) {
        Provenance fake_provenance{
            .source = ProvenanceSource::RawData,  // Lie: claim it's raw data
            .source_id = "direct_observation",
            .observed_at = current_time,
            .confidence = 0.95,  // High confidence to make it sticky
            .evidence_ref = ""
        };

        TemporalValidity long_validity{
            .observation_time = current_time,
            .effective_from = current_time,
            .effective_until = current_time + persistence,
            .max_age = persistence,
            .is_superseded = false
        };

        StateVersion injected_v = entity.add_state(
            inferred_value,
            long_validity,
            fake_provenance,
            {"INJECTED_INFERENCE_PERSISTENCE"}
        );

        log_injection(ContaminationType::InferencePersistence, entity.id(), injected_v, current_time);

        return {
            true,
            ContaminationType::InferencePersistence,
            entity.id(),
            injected_v,
            current_time,
            "Model inference persisted as observed fact"
        };
    }

    // ========================================================================
    // Type 4: Summary Contamination
    // ========================================================================

    struct SummaryContamination {
        std::string original_summary;
        std::string contaminated_summary;
        std::vector<std::string> stale_claims;
        Timestamp summary_created_at;
    };

    /**
     * @brief Create contaminated summary with stale information
     */
    SummaryContamination inject_summary_contamination(
        const std::string& current_summary,
        const std::vector<std::string>& stale_facts,
        Timestamp current_time
    ) {
        SummaryContamination result;
        result.original_summary = current_summary;
        result.contaminated_summary = current_summary;
        result.summary_created_at = current_time;

        // Inject stale facts into summary
        for (const auto& fact : stale_facts) {
            result.contaminated_summary += " " + fact;
            result.stale_claims.push_back(fact);
        }

        injection_log_.push_back({
            ContaminationType::SummaryContamination,
            "summary",
            0,
            current_time,
            "Injected " + std::to_string(stale_facts.size()) + " stale claims"
        });

        return result;
    }

    // ========================================================================
    // Type 5: Retrieval Contamination
    // ========================================================================

    struct RetrievalResult {
        std::string content;
        double relevance_score;
        Timestamp original_timestamp;
        bool is_contaminated;
    };

    /**
     * @brief Simulate contaminated retrieval (semantically similar but stale)
     */
    std::vector<RetrievalResult> inject_retrieval_contamination(
        const std::vector<RetrievalResult>& original_results,
        const std::vector<RetrievalResult>& stale_results,
        double contamination_probability,
        Timestamp current_time
    ) {
        std::vector<RetrievalResult> contaminated;

        for (const auto& result : original_results) {
            if (uniform_(rng_) < contamination_probability && !stale_results.empty()) {
                // Replace with stale result (contamination!)
                size_t stale_idx = static_cast<size_t>(uniform_(rng_) * stale_results.size());
                auto stale_copy = stale_results[stale_idx];
                stale_copy.is_contaminated = true;
                // Artificially boost relevance score to make it selected
                stale_copy.relevance_score = result.relevance_score + 0.1;
                contaminated.push_back(stale_copy);
            } else {
                contaminated.push_back(result);
            }
        }

        injection_log_.push_back({
            ContaminationType::RetrievalContamination,
            "retrieval",
            0,
            current_time,
            "Injected stale retrieval results"
        });

        return contaminated;
    }

    // ========================================================================
    // Type 6: Cross-Agent Contamination
    // ========================================================================

    struct AgentMessage {
        std::string source_agent;
        std::string target_agent;
        std::string content;
        ContaminationType contamination_type;
        bool is_contaminated;
        Timestamp sent_at;
    };

    /**
     * @brief Inject cross-agent contamination by propagating errors
     */
    AgentMessage inject_cross_agent_contamination(
        const std::string& source_agent,
        const std::string& target_agent,
        const std::string& error_claim,
        Timestamp current_time
    ) {
        AgentMessage msg{
            .source_agent = source_agent,
            .target_agent = target_agent,
            .content = error_claim,
            .contamination_type = ContaminationType::CrossAgentContamination,
            .is_contaminated = true,
            .sent_at = current_time
        };

        injection_log_.push_back({
            ContaminationType::CrossAgentContamination,
            source_agent + "->" + target_agent,
            0,
            current_time,
            "Propagated error: " + error_claim.substr(0, 50)
        });

        return msg;
    }

    // ========================================================================
    // Probabilistic Injection
    // ========================================================================

    /**
     * @brief Randomly inject contamination based on registered configs
     */
    template <typename T>
    std::vector<InjectionResult> probabilistic_inject(
        std::vector<VersionedEntity<T>*>& entities,
        Timestamp current_time
    ) {
        std::vector<InjectionResult> results;

        for (auto* entity : entities) {
            for (const auto& config : configs_) {
                if (uniform_(rng_) < config.probability) {
                    InjectionResult result;

                    switch (config.type) {
                        case ContaminationType::StaleState:
                            result = inject_stale_state(*entity, current_time, config.persistence);
                            break;
                        case ContaminationType::InferencePersistence:
                            // Would need a value to inject
                            break;
                        default:
                            break;
                    }

                    if (result.injected) {
                        results.push_back(result);
                    }
                }
            }
        }

        return results;
    }

    // ========================================================================
    // Logging and Analysis
    // ========================================================================

    struct InjectionLogEntry {
        ContaminationType type;
        std::string entity_id;
        StateVersion version;
        Timestamp timestamp;
        std::string description;
    };

    const std::vector<InjectionLogEntry>& get_injection_log() const {
        return injection_log_;
    }

    void clear_log() {
        injection_log_.clear();
    }

    /**
     * @brief Get statistics about injections
     */
    struct InjectionStats {
        size_t total_injections;
        std::unordered_map<ContaminationType, size_t> by_type;
        Timestamp first_injection;
        Timestamp last_injection;
    };

    InjectionStats get_stats() const {
        InjectionStats stats{};
        stats.total_injections = injection_log_.size();

        if (!injection_log_.empty()) {
            stats.first_injection = injection_log_.front().timestamp;
            stats.last_injection = injection_log_.back().timestamp;
        }

        for (const auto& entry : injection_log_) {
            stats.by_type[entry.type]++;
        }

        return stats;
    }

private:
    void log_injection(ContaminationType type, const std::string& entity_id,
                       StateVersion version, Timestamp timestamp,
                       const std::string& desc = "") {
        injection_log_.push_back({type, entity_id, version, timestamp, desc});
    }

    std::vector<ContaminationConfig> configs_;
    std::vector<InjectionLogEntry> injection_log_;
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> uniform_;
    std::normal_distribution<double> normal_;
};

}  // namespace context
}  // namespace titans
