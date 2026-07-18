/**
 * @file versioned_entity.hpp
 * @brief Versioned Entity State Management for Context Reliability
 *
 * Core research contribution: Entity versioning with temporal validity
 * and provenance tracking to detect and mitigate context contamination.
 */

#pragma once

#include "titans/core/types.hpp"
#include <unordered_map>
#include <vector>
#include <optional>
#include <variant>
#include <string>

namespace titans {
namespace context {

// ============================================================================
// Provenance Types - Where did this information come from?
// ============================================================================

enum class ProvenanceSource : uint8_t {
    RawData,           // Direct observation from data feed
    RuleSystem,        // Deterministic rule/threshold
    StatisticalModel,  // Statistical anomaly detection
    GPUFeature,        // CUDA-computed feature
    LLMInference,      // Model inference/conclusion
    LLMHypothesis,     // Model speculation (lower confidence)
    HumanAnnotation,   // Human-provided label
    ExternalTool,      // External system
    CrossAgentClaim,   // Received from another agent
    SummaryDerived,    // Extracted from a summary
    Unknown            // Source not tracked
};

struct Provenance {
    ProvenanceSource source;
    std::string      source_id;       // e.g., "binance_ws", "gpu_feature_v1", "agent_2"
    Timestamp        observed_at;
    double           confidence;      // 0.0 - 1.0
    std::string      evidence_ref;    // Reference to supporting evidence

    bool is_direct_observation() const {
        return source == ProvenanceSource::RawData ||
               source == ProvenanceSource::GPUFeature;
    }

    bool is_model_derived() const {
        return source == ProvenanceSource::LLMInference ||
               source == ProvenanceSource::LLMHypothesis;
    }
};

// ============================================================================
// Temporal Validity - When is this information valid?
// ============================================================================

struct TemporalValidity {
    Timestamp observation_time;   // When was this observed
    Timestamp effective_from;     // When does it become valid
    Timestamp effective_until;    // When does it expire (0 = indefinite)
    Duration  max_age;            // Maximum allowed age for use
    bool      is_superseded;      // Has a newer version replaced this

    bool is_valid_at(Timestamp t) const {
        if (is_superseded) return false;
        if (effective_until > 0 && t > effective_until) return false;
        if (max_age > 0 && (t - observation_time) > max_age) return false;
        return t >= effective_from;
    }

    Duration age_at(Timestamp t) const {
        return t - observation_time;
    }
};

// ============================================================================
// Versioned Entity State
// ============================================================================

using EntityId = std::string;
using StateVersion = uint64_t;

template <typename T>
struct VersionedState {
    StateVersion       version;
    T                  value;
    TemporalValidity   validity;
    Provenance         provenance;
    std::vector<std::string> related_events;  // Event IDs that contributed

    bool is_current(Timestamp now) const {
        return validity.is_valid_at(now) && !validity.is_superseded;
    }
};

/**
 * @brief Entity with full version history
 */
template <typename T>
class VersionedEntity {
public:
    explicit VersionedEntity(EntityId id) : id_(id), current_version_(0) {}

    /**
     * @brief Add a new state version
     */
    StateVersion add_state(const T& value,
                           const TemporalValidity& validity,
                           const Provenance& provenance,
                           const std::vector<std::string>& events = {}) {
        StateVersion new_version = ++current_version_;

        // Mark previous current state as superseded
        if (!states_.empty()) {
            states_.back().validity.is_superseded = true;
        }

        states_.push_back(VersionedState<T>{
            .version = new_version,
            .value = value,
            .validity = validity,
            .provenance = provenance,
            .related_events = events
        });

        return new_version;
    }

    /**
     * @brief Get current state (if valid)
     */
    const VersionedState<T>* current_state(Timestamp now) const {
        for (auto it = states_.rbegin(); it != states_.rend(); ++it) {
            if (it->is_current(now)) {
                return &(*it);
            }
        }
        return nullptr;
    }

    /**
     * @brief Get state at specific version
     */
    const VersionedState<T>* state_at_version(StateVersion v) const {
        for (const auto& s : states_) {
            if (s.version == v) return &s;
        }
        return nullptr;
    }

    /**
     * @brief Get all states valid at a given time (for conflict detection)
     */
    std::vector<const VersionedState<T>*> states_valid_at(Timestamp t) const {
        std::vector<const VersionedState<T>*> result;
        for (const auto& s : states_) {
            if (s.validity.is_valid_at(t)) {
                result.push_back(&s);
            }
        }
        return result;
    }

    /**
     * @brief Get state history (for debugging/analysis)
     */
    const std::vector<VersionedState<T>>& history() const { return states_; }

    /**
     * @brief Detect conflicts: multiple valid states from different sources
     */
    bool has_conflict(Timestamp now) const {
        auto valid = states_valid_at(now);
        if (valid.size() <= 1) return false;

        // Check if they agree
        for (size_t i = 1; i < valid.size(); ++i) {
            if (valid[i]->value != valid[0]->value) {
                return true;
            }
        }
        return false;
    }

    const EntityId& id() const { return id_; }
    StateVersion current_version() const { return current_version_; }

private:
    EntityId id_;
    StateVersion current_version_;
    std::vector<VersionedState<T>> states_;
};

// ============================================================================
// Context Contamination Types
// ============================================================================

enum class ContaminationType : uint8_t {
    None = 0,
    StaleState,            // Outdated state used as current
    EntityBinding,         // Wrong entity's state used
    InferencePersistence,  // Model speculation treated as fact
    SummaryContamination,  // Stale summary information
    RetrievalContamination,// Semantically similar but stale retrieval
    CrossAgentContamination,// Error propagated from another agent
    SourceAmbiguity        // Unknown provenance
};

struct ContaminationEvent {
    ContaminationType type;
    EntityId          affected_entity;
    StateVersion      contaminating_version;
    StateVersion      current_correct_version;
    Timestamp         detected_at;
    std::string       description;
    double            severity;  // 0.0 - 1.0
};

// ============================================================================
// Context Reliability Layer
// ============================================================================

/**
 * @brief Core research contribution: Context validity checking
 */
class ContextReliabilityChecker {
public:
    /**
     * @brief Check if a state can be used for current analysis
     */
    template <typename T>
    std::optional<ContaminationEvent> check_state_validity(
        const VersionedState<T>& state,
        Timestamp current_time,
        const VersionedEntity<T>& entity
    ) {
        // Check temporal validity
        if (!state.validity.is_valid_at(current_time)) {
            return ContaminationEvent{
                .type = ContaminationType::StaleState,
                .affected_entity = entity.id(),
                .contaminating_version = state.version,
                .current_correct_version = entity.current_version(),
                .detected_at = current_time,
                .description = "State expired or superseded",
                .severity = 0.8
            };
        }

        // Check if superseded
        if (state.validity.is_superseded) {
            return ContaminationEvent{
                .type = ContaminationType::StaleState,
                .affected_entity = entity.id(),
                .contaminating_version = state.version,
                .current_correct_version = entity.current_version(),
                .detected_at = current_time,
                .description = "State has been superseded by newer version",
                .severity = 0.9
            };
        }

        // Check provenance
        if (state.provenance.source == ProvenanceSource::Unknown) {
            return ContaminationEvent{
                .type = ContaminationType::SourceAmbiguity,
                .affected_entity = entity.id(),
                .contaminating_version = state.version,
                .detected_at = current_time,
                .description = "State has unknown provenance",
                .severity = 0.5
            };
        }

        // Check if model inference being used as fact
        if (state.provenance.is_model_derived() &&
            state.provenance.confidence < 0.8) {
            return ContaminationEvent{
                .type = ContaminationType::InferencePersistence,
                .affected_entity = entity.id(),
                .contaminating_version = state.version,
                .detected_at = current_time,
                .description = "Low-confidence model inference in context",
                .severity = 0.6
            };
        }

        return std::nullopt;  // No contamination detected
    }

    /**
     * @brief Build safe context by filtering contaminated states
     */
    template <typename T>
    std::vector<const VersionedState<T>*> build_safe_context(
        const std::vector<VersionedEntity<T>*>& entities,
        Timestamp current_time,
        std::vector<ContaminationEvent>& detected_issues
    ) {
        std::vector<const VersionedState<T>*> safe_context;

        for (auto* entity : entities) {
            const auto* current = entity->current_state(current_time);
            if (!current) continue;

            auto issue = check_state_validity(*current, current_time, *entity);
            if (issue) {
                detected_issues.push_back(*issue);
                // Optionally include with warning flag
            } else {
                safe_context.push_back(current);
            }
        }

        return safe_context;
    }
};

// ============================================================================
// Selective Forgetting Policy
// ============================================================================

struct ForgettingCriteria {
    Duration    max_age = 0;          // Forget states older than this
    double      min_confidence = 0;   // Forget low-confidence states
    bool        forget_superseded = true;
    bool        forget_model_inferences = false;
    std::vector<ProvenanceSource> preserve_sources;  // Always keep these
};

template <typename T>
class SelectiveForgetter {
public:
    /**
     * @brief Apply selective forgetting to entity history
     */
    std::vector<VersionedState<T>> apply(
        const std::vector<VersionedState<T>>& history,
        Timestamp current_time,
        const ForgettingCriteria& criteria
    ) {
        std::vector<VersionedState<T>> retained;

        for (const auto& state : history) {
            bool should_forget = false;

            // Age-based forgetting
            if (criteria.max_age > 0 &&
                state.validity.age_at(current_time) > criteria.max_age) {
                should_forget = true;
            }

            // Confidence-based forgetting
            if (state.provenance.confidence < criteria.min_confidence) {
                should_forget = true;
            }

            // Superseded state forgetting
            if (criteria.forget_superseded && state.validity.is_superseded) {
                should_forget = true;
            }

            // Model inference forgetting
            if (criteria.forget_model_inferences &&
                state.provenance.is_model_derived()) {
                should_forget = true;
            }

            // Check if source should be preserved
            for (auto src : criteria.preserve_sources) {
                if (state.provenance.source == src) {
                    should_forget = false;
                    break;
                }
            }

            if (!should_forget) {
                retained.push_back(state);
            }
        }

        return retained;
    }
};

}  // namespace context
}  // namespace titans
