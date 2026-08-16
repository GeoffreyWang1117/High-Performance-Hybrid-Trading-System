/**
 * @file test_contamination.cpp
 * @brief Tests for contamination injection and the experiment pipeline
 */

#include "titans/context/experiment_harness.hpp"
#include <iostream>
#include <cmath>

using namespace titans;
using namespace titans::context;

namespace {

Provenance raw_provenance(Timestamp ts) {
    return Provenance{ProvenanceSource::RawData, "test_feed", ts, 1.0, ""};
}

TemporalValidity open_validity(Timestamp ts) {
    return TemporalValidity{ts, ts, 0, 0, false};
}

bool test_stale_state_injection() {
    ContaminationInjector injector(7);
    VersionedEntity<double> entity("e1");

    // Not enough history: injection must report failure
    entity.add_state(1.0, open_validity(1000), raw_provenance(1000));
    auto fail = injector.inject_stale_state(entity, 2000, 10000);
    if (fail.injected) {
        std::cerr << "Injection should fail with <2 states" << std::endl;
        return false;
    }

    // With history the old state gets resurrected
    entity.add_state(2.0, open_validity(2000), raw_provenance(2000));
    auto ok = injector.inject_stale_state(entity, 3000, 10000);
    if (!ok.injected || ok.type != ContaminationType::StaleState) {
        std::cerr << "Stale-state injection failed" << std::endl;
        return false;
    }

    // Contamination effect: two states now claim validity
    auto valid = entity.states_valid_at(3000);
    if (valid.size() < 2) {
        std::cerr << "Resurrected state should be valid again, got "
                  << valid.size() << std::endl;
        return false;
    }

    return true;
}

bool test_inference_as_fact_injection() {
    ContaminationInjector injector(7);
    VersionedEntity<double> entity("e2");
    entity.add_state(100.0, open_validity(1000), raw_provenance(1000));

    auto r = injector.inject_inference_as_fact(entity, 999.0, 2000, 60000);
    if (!r.injected) {
        std::cerr << "Inference injection failed" << std::endl;
        return false;
    }

    // The lie: it claims RawData provenance, so provenance checks can't
    // catch it — but conflict detection can if the old state is also valid.
    const auto* cur = entity.current_state(3000);
    if (!cur || cur->value != 999.0 ||
        cur->provenance.source != ProvenanceSource::RawData) {
        std::cerr << "Injected inference should masquerade as raw data" << std::endl;
        return false;
    }

    return true;
}

bool test_entity_binding_injection() {
    ContaminationInjector injector(7);
    VersionedEntity<double> source("src");
    VersionedEntity<double> target("dst");

    source.add_state(42.0, open_validity(1000), raw_provenance(1000));
    target.add_state(7.0, open_validity(1000), raw_provenance(1000));

    auto r = injector.inject_entity_binding_error(source, target, 2000);
    if (!r.injected || r.affected_entity != "dst") {
        std::cerr << "Entity-binding injection failed" << std::endl;
        return false;
    }

    // Target now carries source's value, with provenance scrubbed
    const auto* cur = target.current_state(3000);
    if (!cur || cur->value != 42.0 ||
        cur->provenance.source != ProvenanceSource::Unknown) {
        std::cerr << "Binding error should copy value and hide provenance" << std::endl;
        return false;
    }

    return true;
}

bool test_injection_log_and_stats() {
    ContaminationInjector injector(7);
    VersionedEntity<double> entity("e3");
    entity.add_state(1.0, open_validity(1000), raw_provenance(1000));
    entity.add_state(2.0, open_validity(2000), raw_provenance(2000));

    injector.inject_stale_state(entity, 3000, 1000);
    injector.inject_inference_as_fact(entity, 3.0, 4000, 1000);

    auto stats = injector.get_stats();
    if (stats.total_injections != 2) {
        std::cerr << "Expected 2 logged injections, got "
                  << stats.total_injections << std::endl;
        return false;
    }
    if (stats.by_type.at(ContaminationType::StaleState) != 1 ||
        stats.by_type.at(ContaminationType::InferencePersistence) != 1) {
        std::cerr << "Per-type stats wrong" << std::endl;
        return false;
    }

    injector.clear_log();
    if (injector.get_stats().total_injections != 0) {
        std::cerr << "clear_log failed" << std::endl;
        return false;
    }

    return true;
}

bool test_experiment_reproducibility() {
    ExperimentConfig config;
    config.experiment_id = "repro";
    config.num_events = 500;
    config.num_entities = 10;
    config.contamination_rate = 0.2;
    config.method = ContextMethod::VersionedContext;
    config.seed = 123;

    ExperimentRunner runner;
    auto a = runner.run_experiment(config);
    auto b = runner.run_experiment(config);

    if (a.impact_metrics.accuracy != b.impact_metrics.accuracy) {
        std::cerr << "Same seed must give identical accuracy: "
                  << a.impact_metrics.accuracy << " vs "
                  << b.impact_metrics.accuracy << std::endl;
        return false;
    }

    // Sanity: metrics in valid ranges
    if (a.impact_metrics.accuracy < 0.0 || a.impact_metrics.accuracy > 1.0) {
        std::cerr << "Accuracy out of range" << std::endl;
        return false;
    }

    return true;
}

bool test_mitigation_beats_full_history() {
    // The core hypothesis, exercised end-to-end: under contamination the
    // versioned methods must beat naive full-history accumulation.
    ExperimentConfig config;
    config.experiment_id = "hypothesis";
    config.num_events = 2000;
    config.num_entities = 20;
    config.contamination_rate = 0.15;
    config.seed = 42;

    ExperimentRunner runner;

    config.method = ContextMethod::FullHistory;
    auto full = runner.run_experiment(config);

    config.method = ContextMethod::VersionedContext;
    auto versioned = runner.run_experiment(config);

    config.method = ContextMethod::FullVersionedIntegrity;
    auto integrity = runner.run_experiment(config);

    if (versioned.impact_metrics.accuracy <= full.impact_metrics.accuracy) {
        std::cerr << "VersionedContext (" << versioned.impact_metrics.accuracy
                  << ") should beat FullHistory ("
                  << full.impact_metrics.accuracy << ")" << std::endl;
        return false;
    }

    if (integrity.impact_metrics.accuracy < versioned.impact_metrics.accuracy) {
        std::cerr << "FullVersionedIntegrity should not be worse than "
                  << "VersionedContext" << std::endl;
        return false;
    }

    return true;
}

bool test_detection_only_in_mitigating_methods() {
    // The reliability layer must actually fire: mitigating methods detect
    // contamination, baselines never inspect provenance so they detect none.
    ExperimentConfig config;
    config.experiment_id = "detection";
    config.num_events = 2000;
    config.num_entities = 20;
    config.contamination_rate = 0.3;
    config.seed = 42;

    ExperimentRunner runner;

    config.method = ContextMethod::FullVersionedIntegrity;
    auto integrity = runner.run_experiment(config);

    config.method = ContextMethod::FullHistory;
    auto baseline = runner.run_experiment(config);

    if (integrity.detected_contaminations == 0) {
        std::cerr << "FullVersionedIntegrity should detect contamination "
                  << "under 30% injection" << std::endl;
        return false;
    }
    if (baseline.detected_contaminations != 0) {
        std::cerr << "Baseline methods must not report detections, got "
                  << baseline.detected_contaminations << std::endl;
        return false;
    }

    // Full integrity must catch strictly more than plain VersionedContext
    // (it additionally catches conflicts and cross-agent claims).
    config.method = ContextMethod::VersionedContext;
    auto versioned = runner.run_experiment(config);
    if (integrity.detected_contaminations <= versioned.detected_contaminations) {
        std::cerr << "Integrity mode should detect more than VersionedContext: "
                  << integrity.detected_contaminations << " vs "
                  << versioned.detected_contaminations << std::endl;
        return false;
    }

    return true;
}

}  // namespace

bool run_contamination_tests() {
    struct { const char* name; bool (*fn)(); } cases[] = {
        {"stale_state_injection", test_stale_state_injection},
        {"inference_as_fact_injection", test_inference_as_fact_injection},
        {"entity_binding_injection", test_entity_binding_injection},
        {"injection_log_and_stats", test_injection_log_and_stats},
        {"experiment_reproducibility", test_experiment_reproducibility},
        {"mitigation_beats_full_history", test_mitigation_beats_full_history},
        {"detection_only_in_mitigating_methods", test_detection_only_in_mitigating_methods},
    };

    for (const auto& c : cases) {
        std::cout << "  test_" << c.name << "... ";
        if (!c.fn()) return false;
        std::cout << "OK\n";
    }
    return true;
}
