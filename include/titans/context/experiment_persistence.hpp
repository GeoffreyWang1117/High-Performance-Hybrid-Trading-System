/**
 * @file experiment_persistence.hpp
 * @brief Experiment Result Persistence and Statistical Analysis
 *
 * Save/load experiment results, statistical significance testing,
 * and visualization data export for paper figures.
 */

#pragma once

#include <cmath>
#include <limits>

#include "evaluation_metrics.hpp"
#include "llm_interface.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <filesystem>
#include <map>
#include <tuple>

namespace titans {
namespace context {

// ============================================================================
// JSON Serialization (simplified, production would use nlohmann/json)
// ============================================================================

class JSONSerializer {
public:
    static std::string serialize(const ExperimentResult& result) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(6);
        ss << "{\n";
        ss << "  \"experiment_id\": \"" << result.experiment_id << "\",\n";
        ss << "  \"method_name\": \"" << result.method_name << "\",\n";
        ss << "  \"model_name\": \"" << result.model_name << "\",\n";
        ss << "  \"num_events\": " << result.num_events << ",\n";
        ss << "  \"num_entities\": " << result.num_entities << ",\n";
        ss << "  \"contamination_rate\": " << result.contamination_rate << ",\n";
        ss << "  \"impact_metrics\": {\n";
        ss << "    \"accuracy\": " << result.impact_metrics.accuracy << ",\n";
        ss << "    \"accuracy_clean\": " << result.impact_metrics.accuracy_clean << ",\n";
        ss << "    \"accuracy_contaminated\": " << result.impact_metrics.accuracy_contaminated << ",\n";
        ss << "    \"accuracy_delta\": " << result.impact_metrics.accuracy_delta << ",\n";
        ss << "    \"stale_reference_rate\": " << result.impact_metrics.stale_reference_rate << ",\n";
        ss << "    \"entity_confusion_rate\": " << result.impact_metrics.entity_confusion_rate << ",\n";
        ss << "    \"inference_persistence_rate\": " << result.impact_metrics.inference_persistence_rate << ",\n";
        ss << "    \"false_positive_rate\": " << result.impact_metrics.false_positive_rate << ",\n";
        ss << "    \"false_negative_rate\": " << result.impact_metrics.false_negative_rate << "\n";
        ss << "  },\n";
        ss << "  \"persistence_metrics\": {\n";
        ss << "    \"mean_persistence\": " << result.persistence_metrics.mean_persistence << ",\n";
        ss << "    \"p99_persistence\": " << result.persistence_metrics.p99_persistence << ",\n";
        ss << "    \"mean_affected_rounds\": " << result.persistence_metrics.mean_affected_rounds << "\n";
        ss << "  },\n";
        ss << "  \"system_metrics\": {\n";
        ss << "    \"avg_latency_ms\": " << result.avg_latency_ms << ",\n";
        ss << "    \"p99_latency_ms\": " << result.p99_latency_ms << ",\n";
        ss << "    \"avg_context_tokens\": " << result.avg_context_tokens << "\n";
        ss << "  }\n";
        ss << "}";
        return ss.str();
    }

    static std::string serialize(const LLMExperimentResult& result) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(6);
        ss << "{\n";
        ss << "  \"experiment_id\": \"" << result.experiment_id << "\",\n";
        ss << "  \"model_name\": \"" << result.model_name << "\",\n";
        ss << "  \"context_method\": \"" << result.context_method << "\",\n";
        ss << "  \"accuracy\": " << result.impact_metrics.accuracy << ",\n";
        ss << "  \"accuracy_clean\": " << result.impact_metrics.accuracy_clean << ",\n";
        ss << "  \"accuracy_contaminated\": " << result.impact_metrics.accuracy_contaminated << ",\n";
        ss << "  \"accuracy_delta\": " << result.impact_metrics.accuracy_delta << ",\n";
        ss << "  \"avg_latency_ms\": " << result.avg_latency_ms << ",\n";
        ss << "  \"p50_latency_ms\": " << result.p50_latency_ms << ",\n";
        ss << "  \"p99_latency_ms\": " << result.p99_latency_ms << ",\n";
        ss << "  \"total_prompt_tokens\": " << result.total_prompt_tokens << ",\n";
        ss << "  \"total_completion_tokens\": " << result.total_completion_tokens << ",\n";
        ss << "  \"estimated_cost\": " << result.estimated_cost << ",\n";
        ss << "  \"json_parse_success_rate\": " << result.json_parse_success_rate << "\n";
        ss << "}";
        return ss.str();
    }
};

// ============================================================================
// Experiment Result Storage
// ============================================================================

class ExperimentStore {
public:
    explicit ExperimentStore(const std::string& base_dir = "experiments")
        : base_dir_(base_dir) {
        std::filesystem::create_directories(base_dir_);
    }

    void save(const ExperimentResult& result) {
        std::string filename = base_dir_ + "/" + result.experiment_id + ".json";
        std::ofstream file(filename);
        file << JSONSerializer::serialize(result);
        file.close();

        // Append to index
        append_to_index(result.experiment_id, result.method_name);
    }

    void save(const LLMExperimentResult& result) {
        std::string filename = base_dir_ + "/" + result.experiment_id + "_llm.json";
        std::ofstream file(filename);
        file << JSONSerializer::serialize(result);
        file.close();

        append_to_index(result.experiment_id, result.model_name);
    }

    void save_batch(const std::vector<ExperimentResult>& results, const std::string& batch_id) {
        std::string batch_dir = base_dir_ + "/" + batch_id;
        std::filesystem::create_directories(batch_dir);

        std::ofstream batch_file(batch_dir + "/all_results.jsonl");
        for (const auto& result : results) {
            batch_file << JSONSerializer::serialize(result) << "\n";
        }
        batch_file.close();

        // Save summary CSV for easy analysis
        export_csv(results, batch_dir + "/summary.csv");
    }

    void export_csv(const std::vector<ExperimentResult>& results, const std::string& filename) {
        std::ofstream file(filename);
        file << "experiment_id,method,accuracy,accuracy_clean,accuracy_contaminated,"
             << "accuracy_delta,stale_ref_rate,entity_confusion_rate,fpr,fnr,"
             << "avg_latency_ms,p99_latency_ms\n";

        for (const auto& r : results) {
            file << r.experiment_id << ","
                 << r.method_name << ","
                 << r.impact_metrics.accuracy << ","
                 << r.impact_metrics.accuracy_clean << ","
                 << r.impact_metrics.accuracy_contaminated << ","
                 << r.impact_metrics.accuracy_delta << ","
                 << r.impact_metrics.stale_reference_rate << ","
                 << r.impact_metrics.entity_confusion_rate << ","
                 << r.impact_metrics.false_positive_rate << ","
                 << r.impact_metrics.false_negative_rate << ","
                 << r.avg_latency_ms << ","
                 << r.p99_latency_ms << "\n";
        }
    }

private:
    void append_to_index(const std::string& id, const std::string& method) {
        std::ofstream index(base_dir_ + "/index.csv", std::ios::app);
        auto t = std::time(nullptr);
        index << id << "," << method << "," << std::ctime(&t);
    }

    std::string base_dir_;
};

// ============================================================================
// Statistical Significance Testing
// ============================================================================

class StatisticalAnalyzer {
public:
    struct PairedTestResult {
        double mean_diff;
        double std_diff;
        double t_statistic;
        double p_value;
        bool significant_at_05;
        bool significant_at_01;
        std::string interpretation;
    };

    /**
     * @brief Regularized incomplete beta function I_x(a, b).
     *
     * Continued-fraction evaluation (Lentz's method), the standard route to
     * exact Student-t tail probabilities without pulling in a stats library.
     * Converges in well under 200 iterations for the range used here.
     */
    static double incomplete_beta(double a, double b, double x) {
        if (x <= 0.0) return 0.0;
        if (x >= 1.0) return 1.0;

        const double lbeta = std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b);
        const double front = std::exp(lbeta + a * std::log(x) + b * std::log1p(-x));

        // Reflect when x is past the distribution's centre of mass; the
        // continued fraction converges slowly on the far side.
        if (x > (a + 1.0) / (a + b + 2.0)) {
            return 1.0 - incomplete_beta(b, a, 1.0 - x);
        }

        constexpr double kTiny = 1e-30;
        double f = 1.0, c = 1.0, d = 0.0;

        for (int i = 0; i <= 300; ++i) {
            const int m = i / 2;
            double numerator;
            if (i == 0) {
                numerator = 1.0;
            } else if (i % 2 == 0) {
                numerator = (m * (b - m) * x) /
                            ((a + 2.0 * m - 1.0) * (a + 2.0 * m));
            } else {
                numerator = -((a + m) * (a + b + m) * x) /
                             ((a + 2.0 * m) * (a + 2.0 * m + 1.0));
            }

            d = 1.0 + numerator * d;
            if (std::abs(d) < kTiny) d = kTiny;
            d = 1.0 / d;

            c = 1.0 + numerator / c;
            if (std::abs(c) < kTiny) c = kTiny;

            const double cd = c * d;
            f *= cd;

            if (std::abs(1.0 - cd) < 1e-12) break;
        }
        return front * (f - 1.0) / a;
    }

    /**
     * @brief Two-sided p-value for Student's t with @p df degrees of freedom.
     * @param t   Absolute t statistic.
     * @param df  Degrees of freedom; must be >= 1.
     */
    static double student_t_two_sided_p(double t, double df) {
        if (df <= 0.0) return 1.0;
        // NaN means the statistic is undefined (typically 0/0 from a sample
        // with no variance). Report NO evidence, p=1. Returning 0 here would
        // dress an undefined statistic up as maximal significance, which is
        // the dangerous direction to fail in.
        if (std::isnan(t)) return 1.0;
        if (std::isinf(t)) return 0.0;
        const double x = df / (df + t * t);
        return incomplete_beta(0.5 * df, 0.5, x);
    }

    static PairedTestResult paired_t_test(
        const std::vector<double>& baseline,
        const std::vector<double>& treatment
    ) {
        PairedTestResult result;

        if (baseline.size() != treatment.size() || baseline.empty()) {
            result.interpretation = "Invalid input: mismatched or empty samples";
            result.p_value = 1.0;
            return result;
        }
        if (baseline.size() < 2) {
            // A paired t-test needs at least two pairs to estimate the spread
            // of the differences. With one pair there is nothing to divide by.
            result.mean_diff = treatment[0] - baseline[0];
            result.p_value = 1.0;
            result.interpretation =
                "Not testable: a single pair has no variance to estimate";
            return result;
        }

        size_t n = baseline.size();
        std::vector<double> diffs(n);

        for (size_t i = 0; i < n; ++i) {
            diffs[i] = treatment[i] - baseline[i];
        }

        // Mean of differences
        result.mean_diff = std::accumulate(diffs.begin(), diffs.end(), 0.0) / n;

        // Std of differences
        double sum_sq = 0;
        for (double d : diffs) {
            sum_sq += (d - result.mean_diff) * (d - result.mean_diff);
        }
        result.std_diff = std::sqrt(sum_sq / (n - 1));

        // T-statistic. Guard the degenerate case where every pair differs by
        // exactly the same amount (std_diff == 0): the naive division is 0/0
        // for identical arms, and x/0 otherwise.
        const double se = result.std_diff / std::sqrt(static_cast<double>(n));
        if (se == 0.0) {
            if (result.mean_diff == 0.0) {
                // The two arms are identical. There is no effect and no
                // evidence of one.
                result.t_statistic = 0.0;
                result.p_value = 1.0;
                result.significant_at_05 = false;
                result.significant_at_01 = false;
                result.interpretation = "No difference: samples are identical";
                return result;
            }
            // A perfectly constant non-zero difference across every pair. Real
            // measurements do not behave this way; it almost always means the
            // two arms were produced by the same deterministic path with a
            // fixed offset, so flag it instead of reporting p=0.
            result.t_statistic = std::numeric_limits<double>::infinity();
            result.p_value = 0.0;
            result.significant_at_05 = true;
            result.significant_at_01 = true;
            result.interpretation =
                "Degenerate: every pair differs by exactly the same amount; "
                "check whether the arms are genuinely independent";
            return result;
        }
        result.t_statistic = result.mean_diff / se;

        // Two-sided p-value from Student's t with n-1 degrees of freedom.
        //
        // This used to use a normal approximation. That is only defensible for
        // large n, and these experiments run 3-10 seeds: at n=5 (df=4) the
        // normal tail understates p by roughly a third -- t=2.78 is p=0.0054
        // under a normal and p=0.0498 under t(4). Reporting the former as
        // "highly significant (p < 0.01)" for a result that barely clears 0.05
        // is exactly the kind of overstatement this framework should not make.
        result.p_value = student_t_two_sided_p(std::abs(result.t_statistic),
                                               static_cast<double>(n - 1));

        result.significant_at_05 = result.p_value < 0.05;
        result.significant_at_01 = result.p_value < 0.01;

        if (result.significant_at_01) {
            result.interpretation = "Highly significant (p < 0.01)";
        } else if (result.significant_at_05) {
            result.interpretation = "Significant (p < 0.05)";
        } else {
            result.interpretation = "Not significant";
        }

        return result;
    }

    struct BootstrapCI {
        double lower;
        double upper;
        double mean;
    };

    static BootstrapCI bootstrap_confidence_interval(
        const std::vector<double>& data,
        int n_bootstrap = 10000,
        double alpha = 0.05,
        uint64_t seed = 42
    ) {
        std::mt19937_64 rng(seed);
        std::uniform_int_distribution<size_t> dist(0, data.size() - 1);

        std::vector<double> bootstrap_means;
        bootstrap_means.reserve(n_bootstrap);

        for (int b = 0; b < n_bootstrap; ++b) {
            double sum = 0;
            for (size_t i = 0; i < data.size(); ++i) {
                sum += data[dist(rng)];
            }
            bootstrap_means.push_back(sum / data.size());
        }

        std::sort(bootstrap_means.begin(), bootstrap_means.end());

        BootstrapCI ci;
        ci.lower = bootstrap_means[static_cast<size_t>(n_bootstrap * alpha / 2)];
        ci.upper = bootstrap_means[static_cast<size_t>(n_bootstrap * (1 - alpha / 2))];
        ci.mean = std::accumulate(data.begin(), data.end(), 0.0) / data.size();

        return ci;
    }

    /**
     * @brief Seed-paired significance test of every method against a baseline.
     *
     * Runs are paired by (contamination_rate, num_events, seed): the same
     * synthetic stream evaluated under two methods. Requires >= 2 paired
     * seeds per method; methods with fewer pairs are reported as n/a.
     */
    static void print_significance_table(
        const std::vector<ExperimentResult>& results,
        const std::string& baseline_method = "NoHistory"
    ) {
        // Index baseline runs by their experimental condition + seed
        using Key = std::tuple<double, size_t, uint64_t>;
        std::map<Key, double> baseline_acc;
        for (const auto& r : results) {
            if (r.method_name == baseline_method) {
                baseline_acc[{r.contamination_rate, r.num_events, r.seed}] =
                    r.impact_metrics.accuracy;
            }
        }

        if (baseline_acc.empty()) {
            printf("Baseline method '%s' not found\n", baseline_method.c_str());
            return;
        }

        // Collect paired samples per treatment method
        std::map<std::string, std::pair<std::vector<double>, std::vector<double>>> paired;
        for (const auto& r : results) {
            if (r.method_name == baseline_method) continue;
            auto it = baseline_acc.find({r.contamination_rate, r.num_events, r.seed});
            if (it == baseline_acc.end()) continue;
            paired[r.method_name].first.push_back(it->second);
            paired[r.method_name].second.push_back(r.impact_metrics.accuracy);
        }

        printf("\n╔═══════════════════════════════════════════════════════════════════╗\n");
        printf("║  PAIRED SIGNIFICANCE vs %-25s                 ║\n", baseline_method.c_str());
        printf("╠═══════════════════════════════════════════════════════════════════╣\n");
        printf("║ Method            │ Pairs │ Mean Δ    │ t-stat │ p-value          ║\n");
        printf("╠═══════════════════╪═══════╪═══════════╪════════╪══════════════════╣\n");

        for (const auto& [method, samples] : paired) {
            const auto& [base, treat] = samples;

            if (base.size() < 2) {
                printf("║ %-17s │ %5zu │    n/a (need >=2 paired seeds)          ║\n",
                       method.c_str(), base.size());
                continue;
            }

            auto test = paired_t_test(base, treat);
            const char* sig = test.significant_at_01 ? "**"
                             : (test.significant_at_05 ? "*" : "");

            printf("║ %-17s │ %5zu │ %+8.2f%% │ %6.2f │ %7.4f%-2s        ║\n",
                   method.c_str(),
                   base.size(),
                   test.mean_diff * 100,
                   test.t_statistic,
                   test.p_value,
                   sig);
        }

        printf("╚═══════════════════════════════════════════════════════════════════╝\n");
        printf("  * p < 0.05, ** p < 0.01 (paired t-test over matched seeds)\n");
    }

private:
    static double normal_cdf(double x) {
        return 0.5 * std::erfc(-x / std::sqrt(2.0));
    }
};

// ============================================================================
// Visualization Data Export (for Python/matplotlib)
// ============================================================================

class VisualizationExporter {
public:
    explicit VisualizationExporter(const std::string& output_dir = "figures")
        : output_dir_(output_dir) {
        std::filesystem::create_directories(output_dir_);
    }

    void export_accuracy_comparison(
        const std::vector<ExperimentResult>& results,
        const std::string& filename = "accuracy_comparison.csv"
    ) {
        std::ofstream file(output_dir_ + "/" + filename);
        file << "method,accuracy,accuracy_clean,accuracy_contaminated\n";
        for (const auto& r : results) {
            file << r.method_name << ","
                 << r.impact_metrics.accuracy << ","
                 << r.impact_metrics.accuracy_clean << ","
                 << r.impact_metrics.accuracy_contaminated << "\n";
        }
    }

    void export_sensitivity_curve(
        const std::vector<std::pair<double, std::vector<ExperimentResult>>>& sensitivity_data,
        const std::string& filename = "sensitivity_curve.csv"
    ) {
        std::ofstream file(output_dir_ + "/" + filename);

        // Header
        file << "contamination_rate";
        if (!sensitivity_data.empty() && !sensitivity_data[0].second.empty()) {
            for (const auto& r : sensitivity_data[0].second) {
                file << "," << r.method_name;
            }
        }
        file << "\n";

        // Data
        for (const auto& [rate, results] : sensitivity_data) {
            file << rate;
            for (const auto& r : results) {
                file << "," << r.impact_metrics.accuracy;
            }
            file << "\n";
        }
    }

    void export_ablation_heatmap(
        const std::vector<ExperimentResult>& ablation_results,
        const std::string& filename = "ablation_heatmap.csv"
    ) {
        std::ofstream file(output_dir_ + "/" + filename);
        file << "configuration,accuracy,stale_ref,entity_confusion,inference_persist\n";

        for (const auto& r : ablation_results) {
            file << r.method_name << ","
                 << r.impact_metrics.accuracy << ","
                 << r.impact_metrics.stale_reference_rate << ","
                 << r.impact_metrics.entity_confusion_rate << ","
                 << r.impact_metrics.inference_persistence_rate << "\n";
        }
    }

    void export_latency_distribution(
        const std::vector<double>& latencies,
        const std::string& method_name,
        const std::string& filename = "latency_dist.csv"
    ) {
        std::ofstream file(output_dir_ + "/" + filename, std::ios::app);

        // Header on first write
        static bool header_written = false;
        if (!header_written) {
            file << "method,latency_ms\n";
            header_written = true;
        }

        for (double lat : latencies) {
            file << method_name << "," << lat << "\n";
        }
    }

    void generate_latex_table(
        const std::vector<ExperimentResult>& results,
        const std::string& filename = "results_table.tex"
    ) {
        std::ofstream file(output_dir_ + "/" + filename);

        file << "\\begin{table}[t]\n";
        file << "\\centering\n";
        file << "\\caption{Context Method Comparison Results}\n";
        file << "\\label{tab:results}\n";
        file << "\\begin{tabular}{lcccc}\n";
        file << "\\toprule\n";
        file << "Method & Accuracy & Clean Acc. & Contam. Acc. & $\\Delta$ \\\\\n";
        file << "\\midrule\n";

        for (const auto& r : results) {
            file << r.method_name << " & "
                 << std::fixed << std::setprecision(1)
                 << r.impact_metrics.accuracy * 100 << "\\% & "
                 << r.impact_metrics.accuracy_clean * 100 << "\\% & "
                 << r.impact_metrics.accuracy_contaminated * 100 << "\\% & "
                 << r.impact_metrics.accuracy_delta * 100 << "\\% \\\\\n";
        }

        file << "\\bottomrule\n";
        file << "\\end{tabular}\n";
        file << "\\end{table}\n";
    }

private:
    std::string output_dir_;
};

// ============================================================================
// Cross-Validation Framework
// ============================================================================

class CrossValidator {
public:
    struct CVResult {
        std::vector<double> fold_accuracies;
        double mean_accuracy;
        double std_accuracy;
        StatisticalAnalyzer::BootstrapCI confidence_interval;
    };

    static CVResult k_fold_cv(
        ExperimentRunner& runner,
        const ExperimentConfig& base_config,
        int k = 5
    ) {
        CVResult result;
        result.fold_accuracies.reserve(k);

        for (int fold = 0; fold < k; ++fold) {
            ExperimentConfig fold_config = base_config;
            fold_config.seed = base_config.seed + fold * 1000;
            fold_config.experiment_id = base_config.experiment_id + "_fold" + std::to_string(fold);

            auto exp_result = runner.run_experiment(fold_config);
            result.fold_accuracies.push_back(exp_result.impact_metrics.accuracy);
        }

        // Calculate statistics
        result.mean_accuracy = std::accumulate(
            result.fold_accuracies.begin(),
            result.fold_accuracies.end(), 0.0
        ) / k;

        double sum_sq = 0;
        for (double acc : result.fold_accuracies) {
            sum_sq += (acc - result.mean_accuracy) * (acc - result.mean_accuracy);
        }
        result.std_accuracy = std::sqrt(sum_sq / (k - 1));

        result.confidence_interval = StatisticalAnalyzer::bootstrap_confidence_interval(
            result.fold_accuracies
        );

        return result;
    }

    static void print_cv_results(
        const std::unordered_map<std::string, CVResult>& results
    ) {
        printf("\n╔═══════════════════════════════════════════════════════════════╗\n");
        printf("║  CROSS-VALIDATION RESULTS (k=5)                               ║\n");
        printf("╠═══════════════════════════════════════════════════════════════╣\n");
        printf("║ Method            │ Mean Acc │ Std Dev │ 95%% CI              ║\n");
        printf("╠═══════════════════╪══════════╪═════════╪══════════════════════╣\n");

        for (const auto& [method, cv] : results) {
            printf("║ %-17s │ %7.2f%% │ %6.2f%% │ [%5.2f%%, %5.2f%%]      ║\n",
                   method.c_str(),
                   cv.mean_accuracy * 100,
                   cv.std_accuracy * 100,
                   cv.confidence_interval.lower * 100,
                   cv.confidence_interval.upper * 100);
        }

        printf("╚═══════════════════════════════════════════════════════════════╝\n");
    }
};

}  // namespace context
}  // namespace titans
