/**
 * @file run_config_experiment.cpp
 * @brief Config-driven experiment runner
 *
 * Usage:
 *   titans_config_experiment config/experiment.json
 *
 * Reads a batch config, expands it into (method x rate x seed) runs,
 * executes them in parallel, then prints aggregated results and
 * seed-paired significance tests. No recompilation to change parameters.
 */

#include "titans/context/experiment_config_io.hpp"
#include "titans/context/distributed_experiment.hpp"
#include "titans/context/experiment_persistence.hpp"
#include <iostream>
#include <thread>

using namespace titans::context;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <config.json>\n\n"
                  << "Example config:\n"
                  << ExperimentConfigIO::to_json_string(ExperimentBatchConfig{
                         .batch_id = "example",
                         .base = {},
                         .methods = {ContextMethod::NoHistory,
                                     ContextMethod::VersionedContext},
                         .contamination_rates = {0.1, 0.2},
                         .seeds_per_config = 5,
                         .output_dir = "experiments"})
                  << std::endl;
        return 1;
    }

    // 1. Load and expand configuration
    ExperimentBatchConfig batch;
    try {
        batch = ExperimentConfigIO::load(argv[1]);
    } catch (const std::exception& e) {
        std::cerr << "Config error: " << e.what() << std::endl;
        return 1;
    }

    auto configs = ExperimentConfigIO::expand(batch);

    std::cout << "Batch: " << batch.batch_id << "\n"
              << "  methods: " << batch.methods.size()
              << ", rates: " << (batch.contamination_rates.empty() ? 1 : batch.contamination_rates.size())
              << ", seeds: " << batch.seeds_per_config
              << " -> " << configs.size() << " runs\n" << std::endl;

    // 2. Run in parallel
    int workers = std::max(1u, std::thread::hardware_concurrency() / 2);
    LocalDistributedRunner runner(workers);
    auto results = runner.run_distributed(configs);

    // 3. Aggregate and test significance
    auto aggregated = ResultAggregator::aggregate(results);
    ResultAggregator::print_aggregated(aggregated);

    if (batch.seeds_per_config >= 2) {
        StatisticalAnalyzer::print_significance_table(
            results, method_name(batch.methods.front()));
    } else {
        std::cout << "\n(seeds_per_config < 2: skipping significance tests)\n";
    }

    // 4. Persist
    ExperimentStore store(batch.output_dir);
    store.save_batch(results, batch.batch_id);

    std::cout << "\nResults saved to " << batch.output_dir << "/"
              << batch.batch_id << "/" << std::endl;

    return 0;
}
