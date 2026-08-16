/**
 * @file test_versioned_entity.cpp
 * @brief Tests for the versioned entity store and reliability checker
 */

#include "titans/context/versioned_entity.hpp"
#include <iostream>

using namespace titans;
using namespace titans::context;

namespace {

Provenance raw_provenance(Timestamp ts) {
    return Provenance{ProvenanceSource::RawData, "test_feed", ts, 1.0, ""};
}

TemporalValidity open_validity(Timestamp ts) {
    return TemporalValidity{ts, ts, 0, 0, false};
}

bool test_add_and_supersede() {
    VersionedEntity<double> entity("e1");

    StateVersion v1 = entity.add_state(100.0, open_validity(1000), raw_provenance(1000));
    StateVersion v2 = entity.add_state(200.0, open_validity(2000), raw_provenance(2000));

    if (v1 != 1 || v2 != 2) {
        std::cerr << "Version numbering wrong" << std::endl;
        return false;
    }

    // Older state must be superseded automatically
    const auto* s1 = entity.state_at_version(v1);
    if (!s1 || !s1->validity.is_superseded) {
        std::cerr << "v1 should be superseded after v2 added" << std::endl;
        return false;
    }

    // current_state must return the newest valid state
    const auto* cur = entity.current_state(3000);
    if (!cur || cur->version != v2 || cur->value != 200.0) {
        std::cerr << "current_state returned wrong version" << std::endl;
        return false;
    }

    return true;
}

bool test_temporal_expiry() {
    VersionedEntity<double> entity("e2");

    TemporalValidity expiring{1000, 1000, 5000, 0, false};
    entity.add_state(1.0, expiring, raw_provenance(1000));

    if (entity.current_state(3000) == nullptr) {
        std::cerr << "State should be valid before expiry" << std::endl;
        return false;
    }
    if (entity.current_state(6000) != nullptr) {
        std::cerr << "State should be invalid after effective_until" << std::endl;
        return false;
    }

    // max_age based expiry
    VersionedEntity<double> aged("e3");
    TemporalValidity max_aged{1000, 1000, 0, 2000, false};
    aged.add_state(1.0, max_aged, raw_provenance(1000));

    if (aged.current_state(2500) == nullptr) {
        std::cerr << "State within max_age should be valid" << std::endl;
        return false;
    }
    if (aged.current_state(4000) != nullptr) {
        std::cerr << "State past max_age should be invalid" << std::endl;
        return false;
    }

    return true;
}

bool test_conflict_detection() {
    VersionedEntity<double> entity("e4");

    entity.add_state(100.0, open_validity(1000), raw_provenance(1000));
    if (entity.has_conflict(2000)) {
        std::cerr << "Single state cannot conflict" << std::endl;
        return false;
    }

    // Simulate contamination: resurrect the old state alongside a new one
    entity.add_state(200.0, open_validity(3000), raw_provenance(3000));
    auto& hist = const_cast<std::vector<VersionedState<double>>&>(entity.history());
    hist[0].validity.is_superseded = false;

    if (!entity.has_conflict(4000)) {
        std::cerr << "Two valid states with different values must conflict" << std::endl;
        return false;
    }

    return true;
}

bool test_reliability_checker_detects_stale() {
    VersionedEntity<double> entity("e5");
    entity.add_state(1.0, open_validity(1000), raw_provenance(1000));
    entity.add_state(2.0, open_validity(2000), raw_provenance(2000));

    ContextReliabilityChecker checker;

    // The superseded v1 must be flagged as StaleState
    const auto* s1 = entity.state_at_version(1);
    auto issue = checker.check_state_validity(*s1, 3000, entity);
    if (!issue || issue->type != ContaminationType::StaleState) {
        std::cerr << "Checker failed to flag superseded state" << std::endl;
        return false;
    }

    // The current state must pass
    const auto* s2 = entity.state_at_version(2);
    if (checker.check_state_validity(*s2, 3000, entity)) {
        std::cerr << "Checker wrongly flagged current state" << std::endl;
        return false;
    }

    return true;
}

bool test_reliability_checker_provenance() {
    VersionedEntity<double> entity("e6");
    Provenance unknown{ProvenanceSource::Unknown, "", 1000, 0.5, ""};
    entity.add_state(1.0, open_validity(1000), unknown);

    ContextReliabilityChecker checker;
    auto issue = checker.check_state_validity(
        *entity.current_state(2000), 2000, entity);

    if (!issue || issue->type != ContaminationType::SourceAmbiguity) {
        std::cerr << "Checker failed to flag unknown provenance" << std::endl;
        return false;
    }

    // Low-confidence model inference must also be flagged
    VersionedEntity<double> inferred("e7");
    Provenance llm{ProvenanceSource::LLMHypothesis, "model", 1000, 0.5, ""};
    inferred.add_state(1.0, open_validity(1000), llm);

    auto llm_issue = checker.check_state_validity(
        *inferred.current_state(2000), 2000, inferred);
    if (!llm_issue || llm_issue->type != ContaminationType::InferencePersistence) {
        std::cerr << "Checker failed to flag low-confidence inference" << std::endl;
        return false;
    }

    return true;
}

bool test_selective_forgetting() {
    VersionedEntity<double> entity("e8");
    entity.add_state(1.0, open_validity(1000), raw_provenance(1000));

    Provenance llm{ProvenanceSource::LLMInference, "model", 5000, 0.9, ""};
    entity.add_state(2.0, open_validity(5000), llm);

    SelectiveForgetter<double> forgetter;

    // Forget model inferences, preserve raw data (even superseded)
    ForgettingCriteria criteria;
    criteria.forget_superseded = false;
    criteria.forget_model_inferences = true;
    criteria.preserve_sources = {ProvenanceSource::RawData};

    auto retained = forgetter.apply(entity.history(), 6000, criteria);
    if (retained.size() != 1 ||
        retained[0].provenance.source != ProvenanceSource::RawData) {
        std::cerr << "Forgetting should keep only the raw-data state, kept "
                  << retained.size() << std::endl;
        return false;
    }

    // Age-based forgetting drops everything old
    ForgettingCriteria age_criteria;
    age_criteria.forget_superseded = false;
    age_criteria.max_age = 100;
    auto after_age = forgetter.apply(entity.history(), 100000, age_criteria);
    if (!after_age.empty()) {
        std::cerr << "All states exceed max_age, none should remain" << std::endl;
        return false;
    }

    return true;
}

}  // namespace

bool run_versioned_entity_tests() {
    struct { const char* name; bool (*fn)(); } cases[] = {
        {"add_and_supersede", test_add_and_supersede},
        {"temporal_expiry", test_temporal_expiry},
        {"conflict_detection", test_conflict_detection},
        {"reliability_checker_detects_stale", test_reliability_checker_detects_stale},
        {"reliability_checker_provenance", test_reliability_checker_provenance},
        {"selective_forgetting", test_selective_forgetting},
    };

    for (const auto& c : cases) {
        std::cout << "  test_" << c.name << "... ";
        if (!c.fn()) return false;
        std::cout << "OK\n";
    }
    return true;
}
