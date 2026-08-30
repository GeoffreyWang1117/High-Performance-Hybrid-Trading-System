/**
 * @file context_contaminator.hpp
 * @brief Applies contamination to the context a model actually reads.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * The LLM experiment previously constructed a ContaminationInjector, never
 * called it, and passed a `std::vector<ContaminationType>` to the detector,
 * which recorded it as metadata on the output. The prompt was never touched.
 * The independent variable was therefore never applied: every context strategy
 * saw identical, clean input, and any difference between them was noise.
 *
 * This class closes that gap. It mutates the context that is serialized into
 * the prompt, so a contaminated run differs from a clean run in exactly one
 * respect -- what the model was shown.
 *
 * DESIGN RULE FOR EACH CONTAMINATION TYPE
 * ---------------------------------------
 * A contamination must be MISLEADING IF TRUSTED but DETECTABLE IN PRINCIPLE.
 * If it were undetectable, no context strategy could beat any other and the
 * comparison would be vacuous in the opposite direction. Each type below
 * therefore leaves a trace -- a superseded version, a provenance mismatch, an
 * unsupported prior conclusion -- that a strategy tracking that dimension can
 * find and a strategy that does not, cannot.
 *
 * Every mutation is recorded in an AppliedContamination so post-hoc analysis
 * can ask what the model was shown, not just what it answered.
 */

#pragma once

#include "experiment_harness.hpp"
#include "contamination_injector.hpp"

#include <algorithm>
#include <random>
#include <string>
#include <vector>

namespace titans {
namespace context {

/**
 * @brief A record of one mutation actually performed on a context.
 */
struct AppliedContamination {
    ContaminationType type;
    std::string target_entity;     ///< Entity the contamination is about.
    std::string description;       ///< Human-readable account of the mutation.
    /// Index in the contaminated context where the bad material sits, or -1
    /// when the contamination removed or rewrote rather than inserted.
    int context_index = -1;
    /// True when the mutation could not be applied (e.g. no history to draw a
    /// stale value from). Such attempts must NOT be counted as contaminated
    /// trials; doing so silently dilutes the treatment group.
    bool skipped = false;
    std::string skip_reason;
};

/**
 * @brief Result of contaminating one context.
 */
struct ContaminatedContext {
    std::vector<SyntheticEvent> events;              ///< What the model will see.
    std::vector<AppliedContamination> applied;       ///< What was done to it.

    /// Contaminations that were actually applied (not skipped).
    std::vector<ContaminationType> effective_types() const {
        std::vector<ContaminationType> out;
        for (const auto& a : applied) {
            if (!a.skipped) out.push_back(a.type);
        }
        return out;
    }

    bool any_applied() const {
        for (const auto& a : applied) {
            if (!a.skipped) return true;
        }
        return false;
    }
};

/**
 * @brief Mutates a context so the corruption reaches the model's prompt.
 */
class ContextContaminator {
public:
    explicit ContextContaminator(uint64_t seed = 42)
        : rng_(seed), uniform_(0.0, 1.0) {}

    /**
     * @brief Apply @p types to the context for @p current_event.
     *
     * @param current_event  The event being classified. Never mutated: the
     *                       contamination corrupts the CONTEXT, not the
     *                       question, so that accuracy changes are
     *                       attributable to what surrounded the event.
     * @param context        Context as assembled by the strategy under test.
     * @param full_history   All events so far, used as source material for
     *                       stale and cross-entity contamination.
     */
    ContaminatedContext apply(
        const SyntheticEvent& current_event,
        const std::vector<SyntheticEvent>& context,
        const std::vector<SyntheticEvent>& full_history,
        const std::vector<ContaminationType>& types
    ) {
        ContaminatedContext out;
        out.events = context;

        for (auto type : types) {
            switch (type) {
                case ContaminationType::StaleState:
                    apply_stale_state(current_event, full_history, out);
                    break;
                case ContaminationType::EntityBinding:
                    apply_entity_binding(current_event, full_history, out);
                    break;
                case ContaminationType::InferencePersistence:
                    apply_inference_persistence(current_event, out);
                    break;
                case ContaminationType::RetrievalContamination:
                    apply_retrieval(current_event, full_history, out);
                    break;
                case ContaminationType::SummaryContamination:
                    apply_summary(current_event, out);
                    break;
                case ContaminationType::CrossAgentContamination:
                    apply_cross_agent(current_event, out);
                    break;
                default:
                    out.applied.push_back({type, current_event.entity_id,
                                           "unhandled contamination type", -1,
                                           true, "no mutation implemented"});
                    break;
            }
        }
        return out;
    }

private:
    /**
     * @brief Resurrect an old observation of this entity with a fresh timestamp.
     *
     * The entity's level drifts, so an old value presented as current is
     * genuinely wrong about where the entity sits now. Detectable because the
     * resurrected entry duplicates an entity already present at a different
     * value, and its stated age conflicts with the newer entry.
     */
    void apply_stale_state(const SyntheticEvent& cur,
                           const std::vector<SyntheticEvent>& history,
                           ContaminatedContext& out) {
        std::vector<const SyntheticEvent*> own;
        for (const auto& e : history) {
            if (e.entity_id == cur.entity_id && e.event_id != cur.event_id) {
                own.push_back(&e);
            }
        }
        // Needs genuinely OLD material: the oldest third of this entity's
        // history. Without enough of it there is nothing stale to resurrect.
        if (own.size() < 30) {
            out.applied.push_back({ContaminationType::StaleState, cur.entity_id,
                                   "", -1, true,
                                   "entity has only " + std::to_string(own.size()) +
                                   " prior observations; need >= 30 for a stale one"});
            return;
        }

        const size_t pick = static_cast<size_t>(uniform_(rng_) * (own.size() / 3));
        SyntheticEvent stale = *own[pick];
        const double original_value = stale.value;

        // Presented as if it were recent: this is what makes it contamination
        // rather than an honest historical record.
        stale.timestamp = cur.timestamp - 1;
        stale.event_id = stale.event_id + "_resurrected";

        out.events.push_back(stale);
        out.applied.push_back({
            ContaminationType::StaleState, cur.entity_id,
            "resurrected observation " + stale.event_id + " (value " +
                std::to_string(original_value) + ") and restamped it as current",
            static_cast<int>(out.events.size()) - 1, false, ""});
    }

    /**
     * @brief Attribute another entity's observation to this entity.
     *
     * Entity levels are drawn from [50, 500], so a mislabelled observation
     * typically lands far from the true level and pulls any comparison badly
     * off. Detectable because the provenance of the relabelled entry does not
     * match the entity it claims to describe.
     */
    void apply_entity_binding(const SyntheticEvent& cur,
                              const std::vector<SyntheticEvent>& history,
                              ContaminatedContext& out) {
        std::vector<const SyntheticEvent*> others;
        for (const auto& e : history) {
            if (e.entity_id != cur.entity_id) others.push_back(&e);
        }
        if (others.empty()) {
            out.applied.push_back({ContaminationType::EntityBinding, cur.entity_id,
                                   "", -1, true, "no other entity observed yet"});
            return;
        }

        const size_t pick = static_cast<size_t>(uniform_(rng_) * others.size()) % others.size();
        SyntheticEvent borrowed = *others[pick];
        const std::string true_owner = borrowed.entity_id;

        borrowed.entity_id = cur.entity_id;             // the lie
        borrowed.timestamp = cur.timestamp - 2;
        borrowed.event_id = borrowed.event_id + "_misbound";

        out.events.push_back(borrowed);
        out.applied.push_back({
            ContaminationType::EntityBinding, cur.entity_id,
            "relabelled an observation of " + true_owner + " (value " +
                std::to_string(borrowed.value) + ") as belonging to " + cur.entity_id,
            static_cast<int>(out.events.size()) - 1, false, ""});
    }

    /**
     * @brief Insert a prior conclusion that no longer has supporting evidence.
     *
     * Carried as a pseudo-observation whose event_type marks it an inference
     * rather than a measurement. A strategy that distinguishes derived claims
     * from observations can discount it; one that treats all context as fact
     * cannot.
     */
    void apply_inference_persistence(const SyntheticEvent& cur,
                                     ContaminatedContext& out) {
        SyntheticEvent claim;
        claim.event_id = cur.event_id + "_prior_inference";
        claim.entity_id = cur.entity_id;
        claim.timestamp = cur.timestamp - 3;
        // A confident but unsupported assertion about the entity's level.
        claim.value = cur.value * (1.6 + uniform_(rng_) * 0.6);
        claim.event_type = "prior_inference";
        claim.is_anomaly = false;

        out.events.push_back(claim);
        out.applied.push_back({
            ContaminationType::InferencePersistence, cur.entity_id,
            "injected an unsupported prior inference asserting a level of " +
                std::to_string(claim.value),
            static_cast<int>(out.events.size()) - 1, false, ""});
    }

    /**
     * @brief Retrieve unrelated entities' observations as if they were relevant.
     *
     * Models the failure mode of a similarity search that returns
     * topically-near but referentially-wrong material.
     */
    void apply_retrieval(const SyntheticEvent& cur,
                         const std::vector<SyntheticEvent>& history,
                         ContaminatedContext& out) {
        std::vector<const SyntheticEvent*> others;
        for (const auto& e : history) {
            if (e.entity_id != cur.entity_id) others.push_back(&e);
        }
        if (others.size() < 3) {
            out.applied.push_back({ContaminationType::RetrievalContamination,
                                   cur.entity_id, "", -1, true,
                                   "fewer than 3 foreign observations available"});
            return;
        }

        int inserted = 0;
        for (int k = 0; k < 3; ++k) {
            const size_t pick =
                static_cast<size_t>(uniform_(rng_) * others.size()) % others.size();
            SyntheticEvent e = *others[pick];
            e.event_id = e.event_id + "_retrieved";
            out.events.push_back(e);
            ++inserted;
        }
        out.applied.push_back({
            ContaminationType::RetrievalContamination, cur.entity_id,
            "inserted " + std::to_string(inserted) +
                " observations of unrelated entities as retrieved context",
            static_cast<int>(out.events.size()) - 1, false, ""});
    }

    /**
     * @brief Replace part of the context with a summary that misstates it.
     *
     * Compresses several real observations into one entry whose value is wrong.
     * The originals are removed, so the error cannot be caught by comparison
     * against them -- only by noticing that a summary is not a measurement.
     */
    void apply_summary(const SyntheticEvent& cur, ContaminatedContext& out) {
        std::vector<size_t> own_idx;
        for (size_t i = 0; i < out.events.size(); ++i) {
            if (out.events[i].entity_id == cur.entity_id &&
                out.events[i].event_id != cur.event_id) {
                own_idx.push_back(i);
            }
        }
        if (own_idx.size() < 4) {
            out.applied.push_back({ContaminationType::SummaryContamination,
                                   cur.entity_id, "", -1, true,
                                   "fewer than 4 entries to summarize"});
            return;
        }

        double sum = 0;
        for (size_t i : own_idx) sum += out.events[i].value;
        const double true_mean = sum / own_idx.size();
        const double skewed = true_mean * (1.35 + uniform_(rng_) * 0.35);

        // Remove the originals, back to front so indices stay valid.
        for (auto it = own_idx.rbegin(); it != own_idx.rend(); ++it) {
            out.events.erase(out.events.begin() + static_cast<long>(*it));
        }

        SyntheticEvent summary;
        summary.event_id = cur.event_id + "_summary";
        summary.entity_id = cur.entity_id;
        summary.timestamp = cur.timestamp - 4;
        summary.value = skewed;
        summary.event_type = "summary";
        summary.is_anomaly = false;
        out.events.push_back(summary);

        out.applied.push_back({
            ContaminationType::SummaryContamination, cur.entity_id,
            "replaced " + std::to_string(own_idx.size()) +
                " observations with a summary stating " + std::to_string(skewed) +
                " against a true mean of " + std::to_string(true_mean),
            static_cast<int>(out.events.size()) - 1, false, ""});
    }

    /**
     * @brief Insert a claim attributed to a different agent.
     *
     * Distinct from InferencePersistence in provenance: the claim carries
     * another agent's identity, which a strategy doing cross-agent validation
     * can quarantine and one that does not, cannot.
     */
    void apply_cross_agent(const SyntheticEvent& cur, ContaminatedContext& out) {
        SyntheticEvent claim;
        claim.event_id = cur.event_id + "_agent_claim";
        claim.entity_id = cur.entity_id;
        claim.timestamp = cur.timestamp - 5;
        claim.value = cur.value * (0.35 + uniform_(rng_) * 0.25);
        claim.event_type = "agent_b_claim";
        claim.is_anomaly = false;

        out.events.push_back(claim);
        out.applied.push_back({
            ContaminationType::CrossAgentContamination, cur.entity_id,
            "injected a claim from agent_b asserting a level of " +
                std::to_string(claim.value),
            static_cast<int>(out.events.size()) - 1, false, ""});
    }

    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> uniform_;
};

}  // namespace context
}  // namespace titans
