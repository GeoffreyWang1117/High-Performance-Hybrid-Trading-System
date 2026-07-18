/**
 * @file run_distributed_experiment.cpp
 * @brief Run Large-Scale Distributed Experiments with GPU Monitoring
 *
 * Demonstrates:
 * 1. GPU metrics collection
 * 2. Parallel experiment execution
 * 3. Result aggregation with statistics
 * 4. Grid search over hyperparameters
 */

#include "titans/context/distributed_experiment.hpp"
#include "titans/context/experiment_persistence.hpp"
#include "titans/core/gpu_monitor.hpp"
#include <iostream>
#include <iomanip>

using namespace titans::context;
using namespace titans::monitoring;

int main(int argc, char* argv[]) {
    std::cout << R"(
╔═══════════════════════════════════════════════════════════════════════╗
║  Titans: Distributed Context Contamination Experiments                ║
║  Large-Scale Evaluation with GPU Monitoring                           ║
╚═══════════════════════════════════════════════════════════════════════╝
)" << std::endl;

    // ========================================================================
    // Step 1: System Diagnostics
    // ========================================================================
    std::cout << "[1/5] System Diagnostics\n" << std::endl;

    // Check GPU availability
    bool has_gpu = GPUQuery::is_nvidia_available();
    if (has_gpu) {
        int gpu_count = GPUQuery::gpu_count();
        std::cout << "  ✓ NVIDIA GPU detected: " << gpu_count << " device(s)\n";

        auto snapshot = GPUQuery::query_all();
        GPUMetricsReporter::print_snapshot(snapshot);
    } else {
        std::cout << "  ✗ No NVIDIA GPU detected (CPU-only mode)\n";
    }

    // Check available threads
    int num_threads = std::thread::hardware_concurrency();
    std::cout << "  ✓ CPU threads available: " << num_threads << "\n";

    // ========================================================================
    // Step 2: Configure Experiment Grid
    // ========================================================================
    std::cout << "\n[2/5] Configuring Experiment Grid\n" << std::endl;

    // Use quick grid for demo (change to standard_grid() for full experiments)
    bool full_grid = false;
    if (argc > 1 && std::string(argv[1]) == "--full") {
        full_grid = true;
    }

    auto grid_config = full_grid ?
        ExperimentGridGenerator::standard_grid() :
        ExperimentGridGenerator::quick_grid();

    auto configs = ExperimentGridGenerator::generate_grid(grid_config);

    std::cout << "  Grid configuration:\n";
    std::cout << "    Methods: " << grid_config.methods.size() << "\n";
    std::cout << "    Contamination rates: " << grid_config.contamination_rates.size() << "\n";
    std::cout << "    Event counts: " << grid_config.event_counts.size() << "\n";
    std::cout << "    Seeds per config: " << grid_config.seeds_per_config << "\n";
    std::cout << "    Total experiments: " << configs.size() << "\n";

    // ========================================================================
    // Step 3: Run Distributed Experiments
    // ========================================================================
    std::cout << "\n[3/5] Running Experiments\n" << std::endl;

    // Determine number of workers (use half of available threads)
    int num_workers = std::max(1, num_threads / 2);
    std::cout << "  Using " << num_workers << " parallel workers\n\n";

    LocalDistributedRunner runner(num_workers);
    auto results = runner.run_distributed(configs);

    // ========================================================================
    // Step 4: Aggregate and Analyze Results
    // ========================================================================
    std::cout << "\n[4/5] Analyzing Results\n" << std::endl;

    auto aggregated = ResultAggregator::aggregate(results);
    ResultAggregator::print_aggregated(aggregated);

    // Statistical significance testing
    std::cout << "\n";
    StatisticalAnalyzer::print_significance_table(results, "NoHistory");

    // ========================================================================
    // Step 5: Save Results
    // ========================================================================
    std::cout << "\n[5/5] Saving Results\n" << std::endl;

    std::string batch_id = "distributed_" + std::to_string(std::time(nullptr));
    ExperimentStore store("experiments");
    store.save_batch(results, batch_id);

    // Export visualization data
    VisualizationExporter exporter("figures");
    exporter.export_accuracy_comparison(results, "distributed_accuracy.csv");
    exporter.generate_latex_table(results, "distributed_results.tex");

    // ========================================================================
    // Summary
    // ========================================================================
    std::cout << "\n═══════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "EXPERIMENT COMPLETE" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════════" << std::endl;

    // Find best method
    double best_accuracy = 0;
    std::string best_method;
    for (const auto& r : aggregated) {
        if (r.accuracy_mean > best_accuracy) {
            best_accuracy = r.accuracy_mean;
            best_method = r.method;
        }
    }

    std::cout << "\nBest method: " << best_method
              << " (accuracy: " << std::fixed << std::setprecision(1)
              << best_accuracy * 100 << "%)\n";

    std::cout << "\nResults saved to:\n";
    std::cout << "  - experiments/" << batch_id << "/\n";
    std::cout << "  - figures/distributed_accuracy.csv\n";
    std::cout << "  - figures/distributed_results.tex\n";

    std::cout << "\nTo run full grid search:\n";
    std::cout << "  ./titans_distributed_experiment --full\n";

    return 0;
}
