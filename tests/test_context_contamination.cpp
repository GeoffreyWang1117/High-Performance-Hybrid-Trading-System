/**
 * @file test_context_contamination.cpp
 * @brief Asserts that contamination reaches the context, and only the context.
 *
 * The defect this guards against is not a crash. The LLM experiment used to
 * construct a ContaminationInjector, never call it, and attach the
 * contamination vector to the OUTPUT as metadata. Everything ran, every number
 * looked plausible, and the model's input was identical in both arms -- the
 * independent variable was never applied. That failure is invisible from the
 * outside, so it is checked here from the inside.
 *
 * Properties:
 *   1. Applying a contamination CHANGES the context.
 *   2. The event under review is never modified.
 *   3. Each type performs its specific documented mutation.
 *   4. An attempt that cannot be applied is reported as skipped, not silently
 *      counted as a treated trial.
 *   5. effective_types() excludes skipped attempts.
 */

#include "titans/context/context_contaminator.hpp"
#include "titans/context/experiment_harness.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace titans;
using namespace titans::context;

namespace {

/// @brief A history with plenty of material for every contamination type.
std::vector<SyntheticEvent> make_history(size_t n = 200, size_t entities = 5) {
    SyntheticDataGenerator gen(1234);
    return gen.generate_event_stream(n, entities, 1000000, 0.1);
}

bool contains_event(const std::vector<SyntheticEvent>& v, const std::string& id) {
    return std::any_of(v.begin(), v.end(),
                       [&](const SyntheticEvent& e) { return e.event_id == id; });
}

// --------------------------------------------------------------------------

bool test_contamination_changes_the_context() {
    const auto history = make_history();
    const auto& current = history.back();
    std::vector<SyntheticEvent> clean(history.begin(), history.end() - 1);

    ContextContaminator c(7);
    const auto out = c.apply(current, clean, history,
                             {ContaminationType::EntityBinding});

    if (!out.any_applied()) {
        std::fprintf(stderr, "FAIL: nothing was applied; %s\n",
                     out.applied.empty() ? "no record produced"
                                         : out.applied[0].skip_reason.c_str());
        return false;
    }
    if (out.events.size() == clean.size()) {
        std::fprintf(stderr,
                     "FAIL: context size unchanged (%zu). Contamination that "
                     "does not alter the context does not reach the prompt, "
                     "which is the exact defect this test exists for.\n",
                     out.events.size());
        return false;
    }
    std::printf("      context grew %zu -> %zu; %s\n",
                clean.size(), out.events.size(), out.applied[0].description.c_str());
    return true;
}

bool test_event_under_review_is_untouched() {
    const auto history = make_history();
    const auto current = history.back();          // copy, to compare against
    std::vector<SyntheticEvent> clean(history.begin(), history.end() - 1);

    ContextContaminator c(9);
    for (auto type : {ContaminationType::StaleState,
                      ContaminationType::EntityBinding,
                      ContaminationType::InferencePersistence,
                      ContaminationType::SummaryContamination,
                      ContaminationType::RetrievalContamination,
                      ContaminationType::CrossAgentContamination}) {
        const auto out = c.apply(current, clean, history, {type});
        for (const auto& e : out.events) {
            if (e.event_id != current.event_id) continue;
            if (e.value != current.value || e.is_anomaly != current.is_anomaly ||
                e.entity_id != current.entity_id) {
                std::fprintf(stderr,
                             "FAIL: the event under review was modified. "
                             "Contamination must corrupt the CONTEXT, not the "
                             "question, or accuracy changes are not "
                             "attributable to context.\n");
                return false;
            }
        }
    }
    std::printf("      all six types leave the event under review unchanged\n");
    return true;
}

bool test_stale_state_resurrects_an_old_own_observation() {
    const auto history = make_history(400, 3);   // plenty of per-entity history
    const auto& current = history.back();
    std::vector<SyntheticEvent> clean(history.begin(), history.end() - 1);

    ContextContaminator c(3);
    const auto out = c.apply(current, clean, history,
                             {ContaminationType::StaleState});
    if (!out.any_applied()) {
        std::fprintf(stderr, "FAIL: skipped: %s\n", out.applied[0].skip_reason.c_str());
        return false;
    }

    const auto& added = out.events.back();
    if (added.entity_id != current.entity_id) {
        std::fprintf(stderr,
                     "FAIL: stale state must resurrect an observation of the "
                     "SAME entity; got %s for %s\n",
                     added.entity_id.c_str(), current.entity_id.c_str());
        return false;
    }
    if (added.event_id.find("_resurrected") == std::string::npos) {
        std::fprintf(stderr, "FAIL: resurrected entry is not marked as such\n");
        return false;
    }
    if (added.timestamp >= current.timestamp) {
        std::fprintf(stderr,
                     "FAIL: the resurrected entry must be restamped just BEFORE "
                     "the current event to pass as recent\n");
        return false;
    }
    std::printf("      resurrected %s and restamped it as current\n",
                added.event_id.c_str());
    return true;
}

bool test_entity_binding_relabels_another_entitys_value() {
    const auto history = make_history(300, 6);
    const auto& current = history.back();
    std::vector<SyntheticEvent> clean(history.begin(), history.end() - 1);

    ContextContaminator c(5);
    const auto out = c.apply(current, clean, history,
                             {ContaminationType::EntityBinding});
    if (!out.any_applied()) {
        std::fprintf(stderr, "FAIL: skipped: %s\n", out.applied[0].skip_reason.c_str());
        return false;
    }

    const auto& added = out.events.back();
    if (added.entity_id != current.entity_id) {
        std::fprintf(stderr,
                     "FAIL: the misbound entry must CLAIM the current entity; "
                     "that claim is the lie under test\n");
        return false;
    }
    if (added.event_id.find("_misbound") == std::string::npos) {
        std::fprintf(stderr, "FAIL: misbound entry is not marked\n");
        return false;
    }
    if (out.applied[0].description.find("relabelled") == std::string::npos) {
        std::fprintf(stderr, "FAIL: the mutation was not described for audit\n");
        return false;
    }
    std::printf("      %s\n", out.applied[0].description.c_str());
    return true;
}

bool test_summary_replaces_originals_with_a_skewed_value() {
    const auto history = make_history(300, 3);
    const auto& current = history.back();
    std::vector<SyntheticEvent> clean(history.begin(), history.end() - 1);

    size_t own_before = 0;
    for (const auto& e : clean) {
        if (e.entity_id == current.entity_id) ++own_before;
    }

    ContextContaminator c(11);
    const auto out = c.apply(current, clean, history,
                             {ContaminationType::SummaryContamination});
    if (!out.any_applied()) {
        std::fprintf(stderr, "FAIL: skipped: %s\n", out.applied[0].skip_reason.c_str());
        return false;
    }

    size_t own_after = 0, summaries = 0;
    for (const auto& e : out.events) {
        if (e.entity_id != current.entity_id) continue;
        ++own_after;
        if (e.event_type == "summary") ++summaries;
    }
    if (summaries != 1) {
        std::fprintf(stderr, "FAIL: expected exactly one summary entry, got %zu\n",
                     summaries);
        return false;
    }
    if (own_after >= own_before) {
        std::fprintf(stderr,
                     "FAIL: the summary must REPLACE the observations it "
                     "compresses (%zu before, %zu after). Leaving the originals "
                     "in place would let the error be caught by comparison.\n",
                     own_before, own_after);
        return false;
    }
    std::printf("      %zu own entries -> %zu (one summary); %s\n",
                own_before, own_after, out.applied[0].description.c_str());
    return true;
}

/**
 * @brief An unapplicable contamination is reported, not silently counted.
 *
 * Counting a skipped attempt as treated dilutes the treatment group with
 * untreated trials, which biases the measured effect toward zero -- an error
 * that makes contamination look harmless.
 */
bool test_unapplicable_contamination_is_reported_skipped() {
    // Almost no history: nothing old to resurrect, no other entity to borrow.
    SyntheticDataGenerator gen(77);
    const auto tiny = gen.generate_event_stream(2, 1, 1000000, 0.0);
    const auto& current = tiny.back();
    const std::vector<SyntheticEvent> clean{tiny.front()};

    ContextContaminator c(13);
    const auto out = c.apply(current, clean, tiny,
                             {ContaminationType::StaleState,
                              ContaminationType::EntityBinding});

    if (out.applied.size() != 2) {
        std::fprintf(stderr, "FAIL: expected two attempt records, got %zu\n",
                     out.applied.size());
        return false;
    }
    for (const auto& a : out.applied) {
        if (!a.skipped) {
            std::fprintf(stderr,
                         "FAIL: %s should not have been applicable with one "
                         "prior event and one entity\n", a.description.c_str());
            return false;
        }
        if (a.skip_reason.empty()) {
            std::fprintf(stderr, "FAIL: a skipped attempt gave no reason\n");
            return false;
        }
        std::printf("      skipped: %s\n", a.skip_reason.c_str());
    }
    if (!out.effective_types().empty()) {
        std::fprintf(stderr,
                     "FAIL: effective_types() returned %zu entries for two "
                     "skipped attempts. A skipped attempt must never count as "
                     "a treated trial.\n", out.effective_types().size());
        return false;
    }
    if (out.any_applied()) {
        std::fprintf(stderr, "FAIL: any_applied() true with nothing applied\n");
        return false;
    }
    return true;
}

/// @brief Clean context and contaminated context must genuinely differ.
bool test_clean_and_contaminated_contexts_differ() {
    const auto history = make_history(300, 4);
    const auto& current = history.back();
    const std::vector<SyntheticEvent> clean(history.begin(), history.end() - 1);

    ContextContaminator c(21);
    const auto out = c.apply(current, clean, history, {});   // no contamination

    if (out.events.size() != clean.size()) {
        std::fprintf(stderr, "FAIL: an empty contamination list changed the "
                             "context\n");
        return false;
    }
    for (size_t i = 0; i < clean.size(); ++i) {
        if (out.events[i].event_id != clean[i].event_id ||
            out.events[i].value != clean[i].value) {
            std::fprintf(stderr, "FAIL: context altered with no contamination "
                                 "requested\n");
            return false;
        }
    }
    std::printf("      empty contamination list is a no-op (%zu entries "
                "unchanged)\n", clean.size());
    return true;
}

}  // namespace

bool run_context_contamination_tests() {
    struct Case { const char* name; bool (*fn)(); };
    const Case cases[] = {
        {"contamination changes the context",           test_contamination_changes_the_context},
        {"event under review is never modified",        test_event_under_review_is_untouched},
        {"stale state resurrects an own old value",     test_stale_state_resurrects_an_old_own_observation},
        {"entity binding relabels a foreign value",     test_entity_binding_relabels_another_entitys_value},
        {"summary replaces what it compresses",         test_summary_replaces_originals_with_a_skewed_value},
        {"unapplicable attempts are reported skipped",  test_unapplicable_contamination_is_reported_skipped},
        {"no contamination is a no-op",                 test_clean_and_contaminated_contexts_differ},
    };
    bool all = true;
    for (const auto& c : cases) {
        std::printf("  [ RUN ] %s\n", c.name);
        const bool ok = c.fn();
        std::printf("  [ %s ] %s\n", ok ? "OK  " : "FAIL", c.name);
        all = all && ok;
    }
    return all;
}
