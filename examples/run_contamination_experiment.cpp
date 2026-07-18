/**
 * @file run_contamination_experiment.cpp
 * @brief Example: Running Context Contamination Experiments
 *
 * Demonstrates the full experiment pipeline for the research paper.
 */

#include "titans/context/experiment_harness.hpp"
#include <iostream>

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
        ContextMethod::VersionedContext
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

    AblationRunner ablation_runner;
    auto ablations = AblationRunner::standard_ablations();
    auto ablation_results = ablation_runner.run_ablation_study(base_config, ablations);

    printf("\n╔════════════════════════════════════════════════════════════════╗\n");
    printf("║           ABLATION STUDY RESULTS                               ║\n");
    printf("╠════════════════════════════════════════════════════════════════╣\n");
    printf("║ Configuration        │ Accuracy │ Δ Clean │ Stale Ref │ FPR    ║\n");
    printf("╠══════════════════════╪══════════╪═════════╪═══════════╪════════╣\n");

    for (const auto& r : ablation_results) {
        printf("║ %-20s │ %7.2f%% │ %6.2f%% │ %8.2f%% │ %5.2f%% ║\n",
               r.method_name.c_str(),
               r.impact_metrics.accuracy * 100,
               r.impact_metrics.accuracy_delta * 100,
               r.impact_metrics.stale_reference_rate * 100,
               r.impact_metrics.false_positive_rate * 100);
    }

    printf("╚════════════════════════════════════════════════════════════════╝\n");

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

    printf("\nBest Method: %s (%.2f%% accuracy)\n", best_method.c_str(), best_accuracy * 100);
    printf("Worst Method: %s (%.2f%% accuracy)\n", worst_method.c_str(), worst_accuracy * 100);
    printf("Improvement: +%.2f%% absolute, %.1fx relative\n",
           (best_accuracy - worst_accuracy) * 100,
           best_accuracy / worst_accuracy);

    std::cout << "\n✓ All experiments completed successfully." << std::endl;
    std::cout << "Results can be used for paper Tables 1-3." << std::endl;

    return 0;
}
