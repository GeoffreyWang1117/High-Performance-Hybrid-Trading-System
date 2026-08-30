/**
 * @file run_contamination_experiment.cpp
 * @brief Example: Running Context Contamination Experiments
 *
 * Demonstrates the full experiment pipeline for the research paper.
 */

#include "titans/context/experiment_harness.hpp"
#include <iostream>
#include <string>

using namespace titans::context;

int main() {
    std::cout << R"(
╔═══════════════════════════════════════════════════════════════════════╗
║  Titans: Context Contamination Research Experiment Suite              ║
║  Evaluating LLM Reliability in Event-Driven Streaming Systems         ║
╚═══════════════════════════════════════════════════════════════════════╝
)" << std::endl;

    // ========================================================================
    // Experiment 1: Baseline Method Comparison
    // ========================================================================
    std::cout << "\n";
    std::cout << "MODE: assumed-degradation stand-in (no model is queried).\n";
    std::cout << "      Use titans_llm_experiment for claims about real models.\n";
    std::cout << "\n[1/3] Running Baseline Method Comparison...\n" << std::endl;

    ExperimentConfig base_config;
    base_config.experiment_id = "baseline_comparison";
    base_config.description = "Compare context management methods under contamination";
    base_config.num_events = 10000;
    base_config.num_entities = 50;
    base_config.contamination_rate = 0.15;  // 15% contamination
    base_config.seed = 42;

    // Add contamination configurations
    base_config.contamination_configs = {
        {ContaminationType::StaleState, 0.4, 0.7, 0.1, 0, 5000000000LL, false, "Stale state"},
        {ContaminationType::EntityBinding, 0.2, 0.8, 0.1, 0, 3000000000LL, false, "Entity binding"},
        {ContaminationType::InferencePersistence, 0.2, 0.6, 0.15, 0, 10000000000LL, false, "Inference"},
        {ContaminationType::CrossAgentContamination, 0.2, 0.75, 0.1, 0, 8000000000LL, true, "Cross-agent"}
    };

    ExperimentRunner runner;

    std::vector<ContextMethod> methods = {
        ContextMethod::NoHistory,
        ContextMethod::FullHistory,
        ContextMethod::FixedWindow,
        ContextMethod::TimeFilter,
        ContextMethod::VersionedContext,
        ContextMethod::FullVersionedIntegrity
    };

    auto comparison_results = runner.run_comparison(base_config, methods);
    runner.print_comparison(comparison_results);

    // ========================================================================
    // Experiment 2: Contamination Rate Sensitivity
    // ========================================================================
    std::cout << "\n[2/3] Running Contamination Rate Sensitivity Analysis...\n" << std::endl;

    std::vector<double> contamination_rates = {0.05, 0.10, 0.15, 0.20, 0.30, 0.50};

    printf("\n╔════════════════════════════════════════════════════════════════╗\n");
    printf("║           CONTAMINATION RATE SENSITIVITY                       ║\n");
    printf("╠════════════════════════════════════════════════════════════════╣\n");
    printf("║ Rate   │ NoHistory │ FullHist │ FixedWin │ TimeFlt │ Versioned ║\n");
    printf("╠════════╪═══════════╪══════════╪══════════╪═════════╪═══════════╣\n");

    for (double rate : contamination_rates) {
        ExperimentConfig rate_config = base_config;
        rate_config.contamination_rate = rate;
        rate_config.experiment_id = "sensitivity_" + std::to_string(static_cast<int>(rate * 100));

        auto rate_results = runner.run_comparison(rate_config, methods);

        printf("║ %5.0f%% │", rate * 100);
        for (const auto& r : rate_results) {
            printf(" %8.1f%% │", r.impact_metrics.accuracy * 100);
        }
        printf("\n");
    }

    printf("╚════════════════════════════════════════════════════════════════╝\n");

    // ========================================================================
    // Experiment 3: Ablation Study
    // ========================================================================
    std::cout << "\n[3/3] Running Ablation Study...\n" << std::endl;

    // Ablations run against the strongest method, so that the conflict- and
    // cross-agent checks -- which only execute under FullVersionedIntegrity --
    // are actually on the code path being ablated.
    ExperimentConfig ablation_base = base_config;
    ablation_base.method = ContextMethod::FullVersionedIntegrity;

    AblationRunner ablation_runner;
    auto ablations = AblationRunner::standard_ablations();
    auto outcomes = ablation_runner.run_ablation_study_checked(ablation_base, ablations);

    printf("\n%-18s %10s %9s %11s %9s   %s\n",
           "Configuration", "Accuracy", "d Clean", "Stale Ref", "FPR", "status");
    printf("%s\n", std::string(88, '-').c_str());

    int inert_count = 0;
    for (const auto& o : outcomes) {
        printf("%-18s %9.2f%% %8.2f%% %10.2f%% %8.2f%%   %s\n",
               o.name.c_str(),
               o.result.impact_metrics.accuracy * 100,
               o.result.impact_metrics.accuracy_delta * 100,
               o.result.impact_metrics.stale_reference_rate * 100,
               o.result.impact_metrics.false_positive_rate * 100,
               o.inert ? "INERT" : "");
        if (o.inert) ++inert_count;
    }

    if (inert_count > 0) {
        printf("\n%d of %zu ablations were INERT -- identical to the full system,\n",
               inert_count, outcomes.size());
        printf("meaning the disabled component never executed. These rows say\n");
        printf("nothing about whether the component matters:\n");
        for (const auto& o : outcomes) {
            if (o.inert) printf("  - %-16s %s\n", o.name.c_str(), o.inert_reason.c_str());
        }
    }

    // ========================================================================
    // Summary Statistics
    // ========================================================================
    std::cout << "\n═══════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "EXPERIMENT SUMMARY" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════════" << std::endl;

    // Find best and worst methods
    double best_accuracy = 0, worst_accuracy = 1.0;
    std::string best_method, worst_method;

    for (const auto& r : comparison_results) {
        if (r.impact_metrics.accuracy > best_accuracy) {
            best_accuracy = r.impact_metrics.accuracy;
            best_method = r.method_name;
        }
        if (r.impact_metrics.accuracy < worst_accuracy) {
            worst_accuracy = r.impact_metrics.accuracy;
            worst_method = r.method_name;
        }
    }

    printf("\nHighest scoring method: %s (%.2f%%)\n",
           best_method.c_str(), best_accuracy * 100);
    printf("Lowest scoring method:  %s (%.2f%%)\n",
           worst_method.c_str(), worst_accuracy * 100);
    printf("Spread: %.2f points absolute, %.1fx relative\n",
           (best_accuracy - worst_accuracy) * 100,
           best_accuracy / worst_accuracy);

    printf("\n");
    printf("================================================================\n");
    printf(" WHAT THESE NUMBERS ARE\n");
    printf("================================================================\n");
    printf(
        "Every accuracy above was produced by AssumedDegradationModel, whose\n"
        "response to contamination is a product of hardcoded multipliers\n"
        "(0.5 for entity binding, 0.7 for stale state, ...) followed by one\n"
        "Bernoulli draw. No model was queried.\n"
        "\n"
        "So these numbers CAN support:\n"
        "  - that injection, mitigation, and metrics are wired together;\n"
        "  - that context strategies differ in how much contaminated material\n"
        "    they retain, which is a property of the strategies themselves;\n"
        "  - regression detection across refactors.\n"
        "\n"
        "They CANNOT support any claim about how a language model behaves\n"
        "under contamination. The ranking of contamination types here is the\n"
        "ranking of the constants in experiment_harness.hpp -- reading it as a\n"
        "finding is circular.\n"
        "\n"
        "For claims about real models, run:  titans_llm_experiment\n"
        "which queries a live backend and records the model, prompt, and raw\n"
        "responses alongside the metrics.\n");

    return 0;
}
