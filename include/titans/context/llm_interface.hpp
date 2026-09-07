/**
 * @file llm_interface.hpp
 * @brief LLM Integration Interface for Real Model Experiments
 *
 * Abstraction layer supporting multiple LLM backends:
 * - Ollama (local)
 * - vLLM (local/remote)
 * - OpenAI-compatible APIs
 * - Anthropic Claude
 */

#pragma once

#include "versioned_entity.hpp"
#include "evaluation_metrics.hpp"
#include "experiment_harness.hpp"
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <chrono>
#include <optional>
#include <future>

namespace titans {
namespace context {

// ============================================================================
// LLM Request/Response Structures
// ============================================================================

struct LLMMessage {
    enum class Role { System, User, Assistant };
    Role role;
    std::string content;
};

struct LLMRequest {
    std::string model;
    std::vector<LLMMessage> messages;
    double temperature = 0.0;
    int max_tokens = 1024;
    bool stream = false;
    std::optional<std::string> response_format;  // "json" for structured output

    // Context metadata for experiments
    std::string request_id;
    Timestamp context_cutoff_time;
    std::vector<std::string> entity_ids_in_context;
};

struct LLMResponse {
    std::string request_id;
    std::string content;
    std::string model;

    // Token usage
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;

    // Timing
    double latency_ms = 0;
    double time_to_first_token_ms = 0;
    /**
     * @brief Prefill time alone, when the backend reports it.
     *
     * This is the quantity that scales with context length, and the reason a
     * richer context costs freshness. Kept separate from
     * `time_to_first_token_ms`, which on a cold model also carries the weight
     * load and would attribute it to the prompt.
     */
    double prefill_ms = 0;

    // For structured output
    bool parse_success = false;
    std::string parse_error;

    // Error handling
    bool success = true;
    std::string error_message;
};

// ============================================================================
// LLM Backend Interface
// ============================================================================

class LLMBackend {
public:
    virtual ~LLMBackend() = default;

    virtual std::string name() const = 0;
    virtual bool is_available() const = 0;
    virtual std::vector<std::string> available_models() const = 0;

    virtual LLMResponse complete(const LLMRequest& request) = 0;
    virtual std::future<LLMResponse> complete_async(const LLMRequest& request) = 0;

    // Batch inference for efficiency
    virtual std::vector<LLMResponse> complete_batch(
        const std::vector<LLMRequest>& requests,
        [[maybe_unused]] int max_concurrent = 4
    ) {
        std::vector<LLMResponse> responses;
        for (const auto& req : requests) {
            responses.push_back(complete(req));
        }
        return responses;
    }
};

// ============================================================================
// Ollama Backend (Primary Local LLM)
// ============================================================================

struct OllamaConfig {
    std::string host = "localhost";
    int port = 11434;
    std::string default_model = "llama3.1:8b";
    int timeout_ms = 120000;
    bool keep_alive = true;
    /**
     * @brief Context window, in tokens, sent as Ollama's `num_ctx`.
     *
     * Ollama defaults this to a few thousand tokens regardless of what the
     * model supports, and it TRUNCATES a longer prompt silently -- no error, no
     * warning, a perfectly normal-looking response. Any experiment that sweeps
     * context length past that point is then measuring the truncation and not
     * the context: prefill time plateaus, the answer stops changing, and the
     * curve says "more context does not help".
     *
     * 0 leaves the server's default in place. Anything sweeping context length
     * must set it, and must also check `prompt_tokens` against what it sent --
     * see `titans_context_cost`, which refuses an arm whose prompt did not
     * grow.
     */
    int num_ctx = 0;
};

// The concrete Ollama backend lives in ollama_backend.hpp (OllamaBackendImpl).
//
// A stub with this name used to sit here: complete() ignored the request and
// returned a fixed {"classification": "normal", ...} while reporting
// success = true, and is_available() returned true without contacting
// anything. Any experiment that picked it up produced clean-looking numbers
// from a model that was never queried. Removed rather than fixed -- two
// classes with the same responsibility and one silently fake is a trap, and
// LLMBackendFactory::auto_detect() health-checks a real endpoint instead.

// ============================================================================
// vLLM Backend (High-throughput Local Inference)
// ============================================================================

struct VLLMConfig {
    std::string host = "localhost";
    int port = 8000;
    std::string api_key = "";
    int timeout_ms = 60000;
};

// The concrete vLLM backend lives in ollama_backend.hpp (VLLMBackendImpl).
// A stub with this name used to sit here and returned "{}" for every request.

// ============================================================================
// Prompt Templates for Anomaly Detection Task
// ============================================================================

class PromptBuilder {
public:
    static std::string build_system_prompt() {
        return R"(You are an expert financial anomaly detection system. Your task is to analyze trading events and determine if they represent anomalous activity.

For each event, you will receive:
1. The current event details
2. Historical context about the entity
3. Related market conditions

You must respond with a JSON object containing:
{
  "classification": "anomaly" | "normal",
  "confidence": 0.0-1.0,
  "reasoning": "brief explanation",
  "evidence": ["list", "of", "supporting", "facts"],
  "referenced_entity_versions": [1, 2, 3]
}

IMPORTANT: Only use information that is currently valid. Be skeptical of:
- Information that may be outdated
- Inferences made by previous analysis rounds
- Claims without clear provenance)";
    }

    static std::string build_event_prompt(
        const std::string& event_json,
        const std::string& context_json,
        const std::string& entity_history_json
    ) {
        return "## Current Event\n" + event_json +
               "\n\n## Entity Context\n" + context_json +
               "\n\n## Version History\n" + entity_history_json +
               "\n\nAnalyze this event and provide your assessment in JSON format.";
    }

    static std::string build_versioned_context_prompt(
        const std::string& event_json,
        const std::vector<std::pair<std::string, StateVersion>>& versioned_context
    ) {
        std::string context = "## Versioned Entity States\n";
        for (const auto& [entity_json, version] : versioned_context) {
            context += "### Entity (v" + std::to_string(version) + ")\n";
            context += entity_json + "\n\n";
        }

        return "## Current Event\n" + event_json +
               "\n\n" + context +
               "\n\nAnalyze using ONLY the latest version of each entity. "
               "Flag if you notice any version inconsistencies.";
    }
};

// ============================================================================
// Response Parser
// ============================================================================

struct ParsedLLMOutput {
    std::string classification;
    double confidence = 0.0;
    std::string reasoning;
    std::vector<std::string> evidence;
    std::vector<StateVersion> referenced_versions;
    bool valid = false;
    std::string parse_error;
};

class ResponseParser {
public:
    static ParsedLLMOutput parse(const std::string& json_response) {
        ParsedLLMOutput output;

        // Simple JSON parsing (production would use nlohmann/json or simdjson)
        auto find_string = [&](const std::string& key) -> std::string {
            size_t pos = json_response.find("\"" + key + "\"");
            if (pos == std::string::npos) return "";
            pos = json_response.find(":", pos);
            if (pos == std::string::npos) return "";
            pos = json_response.find("\"", pos);
            if (pos == std::string::npos) return "";
            size_t end = json_response.find("\"", pos + 1);
            if (end == std::string::npos) return "";
            return json_response.substr(pos + 1, end - pos - 1);
        };

        auto find_number = [&](const std::string& key) -> double {
            size_t pos = json_response.find("\"" + key + "\"");
            if (pos == std::string::npos) return 0.0;
            pos = json_response.find(":", pos);
            if (pos == std::string::npos) return 0.0;
            return std::stod(json_response.substr(pos + 1));
        };

        output.classification = find_string("classification");
        output.confidence = find_number("confidence");
        output.reasoning = find_string("reasoning");
        output.valid = !output.classification.empty();

        if (!output.valid) {
            output.parse_error = "Failed to parse classification from response";
        }

        return output;
    }
};

// ============================================================================
// LLM Experiment Runner
// ============================================================================

struct LLMExperimentConfig {
    std::string experiment_id;
    std::string model_name;
    std::shared_ptr<LLMBackend> backend;

    // Context method to test
    ContextMethod context_method = ContextMethod::VersionedContext;

    // Experiment parameters
    size_t num_events = 1000;
    double contamination_rate = 0.1;
    int batch_size = 10;

    // Cost tracking
    double cost_per_1k_input_tokens = 0.0;  // For API-based models
    double cost_per_1k_output_tokens = 0.0;

    uint64_t seed = 42;
};

struct LLMExperimentResult {
    std::string experiment_id;
    std::string model_name;
    std::string context_method;

    // Core metrics
    ContaminationImpactMetrics impact_metrics;

    // LLM-specific metrics
    double avg_latency_ms = 0;
    double p50_latency_ms = 0;
    double p99_latency_ms = 0;

    int total_prompt_tokens = 0;
    int total_completion_tokens = 0;
    double estimated_cost = 0;

    // Quality metrics
    double json_parse_success_rate = 0;
    double confidence_calibration = 0;  // How well confidence matches accuracy

    // Per-contamination-type breakdown
    std::unordered_map<ContaminationType, double> accuracy_by_contamination;

    void print_summary() const {
        printf("\n╔══════════════════════════════════════════════════════════════╗\n");
        printf("║  LLM EXPERIMENT RESULTS: %-35s ║\n", experiment_id.c_str());
        printf("╠══════════════════════════════════════════════════════════════╣\n");
        printf("║  Model: %-52s ║\n", model_name.c_str());
        printf("║  Context Method: %-43s ║\n", context_method.c_str());
        printf("╠══════════════════════════════════════════════════════════════╣\n");
        printf("║  Accuracy: %6.2f%% (Clean: %6.2f%%, Contaminated: %6.2f%%)  ║\n",
               impact_metrics.accuracy * 100,
               impact_metrics.accuracy_clean * 100,
               impact_metrics.accuracy_contaminated * 100);
        printf("║  Accuracy Delta: %6.2f%%                                     ║\n",
               impact_metrics.accuracy_delta * 100);
        printf("╠══════════════════════════════════════════════════════════════╣\n");
        printf("║  Latency: avg=%.1fms, p50=%.1fms, p99=%.1fms               ║\n",
               avg_latency_ms, p50_latency_ms, p99_latency_ms);
        printf("║  Tokens: %d prompt, %d completion                        ║\n",
               total_prompt_tokens, total_completion_tokens);
        printf("║  Est. Cost: $%.4f                                          ║\n", estimated_cost);
        printf("╚══════════════════════════════════════════════════════════════╝\n");
    }
};

class LLMExperimentRunner {
public:
    LLMExperimentResult run(const LLMExperimentConfig& config) {
        LLMExperimentResult result;
        result.experiment_id = config.experiment_id;
        result.model_name = config.model_name;
        result.context_method = method_name(config.context_method);

        // Generate synthetic events
        SyntheticDataGenerator data_gen(config.seed);
        auto events = data_gen.generate_event_stream(
            config.num_events, 50, 1000000
        );

        std::vector<ModelOutput> outputs;
        std::vector<double> latencies;
        int total_prompt = 0, total_completion = 0;
        int parse_successes = 0;

        // Run inference
        for (size_t i = 0; i < events.size(); ++i) {
            LLMRequest request;
            request.request_id = events[i].event_id;
            request.model = config.model_name;
            request.messages = {
                {LLMMessage::Role::System, PromptBuilder::build_system_prompt()},
                {LLMMessage::Role::User, PromptBuilder::build_event_prompt(
                    "{\"event\": \"" + events[i].event_id + "\", \"value\": " +
                        std::to_string(events[i].value) + "}",
                    "{}", "{}"
                )}
            };

            auto response = config.backend->complete(request);

            latencies.push_back(response.latency_ms);
            total_prompt += response.prompt_tokens;
            total_completion += response.completion_tokens;

            auto parsed = ResponseParser::parse(response.content);
            if (parsed.valid) ++parse_successes;

            ModelOutput output;
            output.task_id = events[i].event_id;
            output.output_time = events[i].timestamp;
            output.predicted_class = parsed.classification;
            output.ground_truth_class = events[i].is_anomaly ? "anomaly" : "normal";
            output.confidence = parsed.confidence;
            output.is_correct = (output.predicted_class == output.ground_truth_class);
            outputs.push_back(output);
        }

        // Calculate metrics
        MetricCalculator calc;
        result.impact_metrics = calc.calculate_impact_metrics(outputs);

        // Latency stats
        std::sort(latencies.begin(), latencies.end());
        result.avg_latency_ms = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
        result.p50_latency_ms = latencies[latencies.size() / 2];
        result.p99_latency_ms = latencies[latencies.size() * 99 / 100];

        // Token stats
        result.total_prompt_tokens = total_prompt;
        result.total_completion_tokens = total_completion;
        result.estimated_cost = (total_prompt / 1000.0) * config.cost_per_1k_input_tokens +
                               (total_completion / 1000.0) * config.cost_per_1k_output_tokens;

        result.json_parse_success_rate = static_cast<double>(parse_successes) / events.size();

        return result;
    }
};

// ============================================================================
// Multi-Model Comparison
// ============================================================================

class ModelComparisonRunner {
public:
    std::vector<LLMExperimentResult> compare_models(
        const std::vector<std::pair<std::string, std::shared_ptr<LLMBackend>>>& models,
        const std::vector<ContextMethod>& methods,
        size_t num_events = 500,
        double contamination_rate = 0.15
    ) {
        std::vector<LLMExperimentResult> results;
        LLMExperimentRunner runner;

        for (const auto& [model_name, backend] : models) {
            for (auto method : methods) {
                LLMExperimentConfig config;
                config.experiment_id = model_name + "_" + method_name(method);
                config.model_name = model_name;
                config.backend = backend;
                config.context_method = method;
                config.num_events = num_events;
                config.contamination_rate = contamination_rate;

                results.push_back(runner.run(config));
            }
        }

        return results;
    }

    void print_comparison_table(const std::vector<LLMExperimentResult>& results) {
        printf("\n╔═══════════════════════════════════════════════════════════════════════════╗\n");
        printf("║                    MODEL x METHOD COMPARISON                              ║\n");
        printf("╠════════════════════════╦══════════╦═════════╦═════════╦══════════╦════════╣\n");
        printf("║ Configuration          ║ Accuracy ║ Δ Clean ║ Latency ║ Tokens   ║ Cost   ║\n");
        printf("╠════════════════════════╬══════════╬═════════╬═════════╬══════════╬════════╣\n");

        for (const auto& r : results) {
            printf("║ %-22s ║ %7.2f%% ║ %6.2f%% ║ %5.0fms ║ %8d ║ $%5.3f ║\n",
                   r.experiment_id.c_str(),
                   r.impact_metrics.accuracy * 100,
                   r.impact_metrics.accuracy_delta * 100,
                   r.avg_latency_ms,
                   r.total_prompt_tokens + r.total_completion_tokens,
                   r.estimated_cost);
        }

        printf("╚════════════════════════╩══════════╩═════════╩═════════╩══════════╩════════╝\n");
    }
};

}  // namespace context
}  // namespace titans
