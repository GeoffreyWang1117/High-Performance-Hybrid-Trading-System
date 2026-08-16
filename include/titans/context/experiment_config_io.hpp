/**
 * @file experiment_config_io.hpp
 * @brief JSON (de)serialization for experiment configurations
 *
 * Lets experiments be parameterized from config files instead of
 * recompiling: `titans_config_experiment config/experiment.json`.
 */

#pragma once

#include "experiment_harness.hpp"
#include "../core/json.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace titans {
namespace context {

// ============================================================================
// ContextMethod <-> string
// ============================================================================

inline std::optional<ContextMethod> method_from_name(const std::string& name) {
    static const std::unordered_map<std::string, ContextMethod> lookup = {
        {"NoHistory", ContextMethod::NoHistory},
        {"FullHistory", ContextMethod::FullHistory},
        {"FixedWindow", ContextMethod::FixedWindow},
        {"RollingSummary", ContextMethod::RollingSummary},
        {"VectorRetrieval", ContextMethod::VectorRetrieval},
        {"TimeFilter", ContextMethod::TimeFilter},
        {"VersionedContext", ContextMethod::VersionedContext},
        {"VersionedIntegrity", ContextMethod::FullVersionedIntegrity},
        {"FullVersionedIntegrity", ContextMethod::FullVersionedIntegrity},
    };
    auto it = lookup.find(name);
    if (it == lookup.end()) return std::nullopt;
    return it->second;
}

// ============================================================================
// Batch configuration: one file describes a whole comparison run
// ============================================================================

struct ExperimentBatchConfig {
    std::string batch_id;
    ExperimentConfig base;                 // shared parameters
    std::vector<ContextMethod> methods;    // methods to compare
    std::vector<double> contamination_rates;  // empty = just base rate
    int seeds_per_config = 1;
    std::string output_dir = "experiments";
};

class ExperimentConfigIO {
public:
    /**
     * @brief Load a batch config from a JSON file.
     * @throws std::runtime_error with a precise message on any problem.
     */
    static ExperimentBatchConfig load(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) {
            throw std::runtime_error("Cannot open config file: " + path);
        }
        std::ostringstream ss;
        ss << file.rdbuf();

        auto parsed = json::try_parse(ss.str());
        if (!parsed) {
            throw std::runtime_error("Invalid JSON in config file: " + path);
        }
        return from_json(*parsed, path);
    }

    static ExperimentBatchConfig from_json(const json::Value& v,
                                           const std::string& source = "<inline>") {
        ExperimentBatchConfig batch;

        batch.batch_id = v["batch_id"].as_string();
        if (batch.batch_id.empty()) {
            throw std::runtime_error(source + ": 'batch_id' is required");
        }

        // Base experiment parameters (all optional, defaults apply)
        const auto& base = v["base"];
        batch.base.experiment_id = batch.batch_id;
        if (base.contains("num_events"))
            batch.base.num_events = static_cast<size_t>(base["num_events"].as_int64());
        if (base.contains("num_entities"))
            batch.base.num_entities = static_cast<size_t>(base["num_entities"].as_int64());
        if (base.contains("event_interval_ns"))
            batch.base.event_interval_ns = base["event_interval_ns"].as_int64();
        if (base.contains("contamination_rate"))
            batch.base.contamination_rate = base["contamination_rate"].as_number();
        if (base.contains("seed"))
            batch.base.seed = static_cast<uint64_t>(base["seed"].as_int64());

        // Methods to compare
        if (!v["methods"].is_array() || v["methods"].size() == 0) {
            throw std::runtime_error(source + ": 'methods' must be a non-empty array");
        }
        for (const auto& m : v["methods"].as_array()) {
            auto method = method_from_name(m.as_string());
            if (!method) {
                throw std::runtime_error(
                    source + ": unknown method '" + m.as_string() + "'");
            }
            batch.methods.push_back(*method);
        }

        // Optional sweep over contamination rates
        if (v.contains("contamination_rates")) {
            for (const auto& r : v["contamination_rates"].as_array()) {
                double rate = r.as_number();
                if (rate < 0.0 || rate > 1.0) {
                    throw std::runtime_error(
                        source + ": contamination rate out of [0,1]");
                }
                batch.contamination_rates.push_back(rate);
            }
        }

        if (v.contains("seeds_per_config")) {
            batch.seeds_per_config = v["seeds_per_config"].as_int();
            if (batch.seeds_per_config < 1) {
                throw std::runtime_error(source + ": seeds_per_config must be >= 1");
            }
        }

        if (v.contains("output_dir")) {
            batch.output_dir = v["output_dir"].as_string();
        }

        return batch;
    }

    /**
     * @brief Expand a batch config into concrete experiment configs.
     */
    static std::vector<ExperimentConfig> expand(const ExperimentBatchConfig& batch) {
        std::vector<ExperimentConfig> configs;

        std::vector<double> rates = batch.contamination_rates;
        if (rates.empty()) rates.push_back(batch.base.contamination_rate);

        for (auto method : batch.methods) {
            for (double rate : rates) {
                for (int s = 0; s < batch.seeds_per_config; ++s) {
                    ExperimentConfig cfg = batch.base;
                    cfg.method = method;
                    cfg.contamination_rate = rate;
                    cfg.seed = batch.base.seed + static_cast<uint64_t>(s) * 1000;
                    cfg.experiment_id = batch.batch_id + "_" + method_name(method) +
                        "_r" + std::to_string(static_cast<int>(rate * 100)) +
                        "_s" + std::to_string(s);
                    configs.push_back(cfg);
                }
            }
        }

        return configs;
    }

    static std::string to_json_string(const ExperimentBatchConfig& batch) {
        json::Value v;
        v["batch_id"] = batch.batch_id;

        json::Value base;
        base["num_events"] = static_cast<int64_t>(batch.base.num_events);
        base["num_entities"] = static_cast<int64_t>(batch.base.num_entities);
        base["event_interval_ns"] = static_cast<int64_t>(batch.base.event_interval_ns);
        base["contamination_rate"] = batch.base.contamination_rate;
        base["seed"] = static_cast<int64_t>(batch.base.seed);
        v["base"] = base;

        json::Value methods;
        for (auto m : batch.methods) methods.push_back(method_name(m));
        v["methods"] = methods;

        if (!batch.contamination_rates.empty()) {
            json::Value rates;
            for (double r : batch.contamination_rates) rates.push_back(r);
            v["contamination_rates"] = rates;
        }

        v["seeds_per_config"] = batch.seeds_per_config;
        v["output_dir"] = batch.output_dir;

        return json::stringify(v, /*pretty=*/true);
    }
};

}  // namespace context
}  // namespace titans
