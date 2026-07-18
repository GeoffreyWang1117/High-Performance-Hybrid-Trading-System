/**
 * @file run_llm_experiment.cpp
 * @brief Run Real LLM Experiments for Context Contamination Research
 *
 * Complete example demonstrating:
 * 1. LLM backend auto-detection
 * 2. Model comparison experiments
 * 3. Context method evaluation with real LLMs
 * 4. Result persistence and analysis
 */

#include "titans/context/experiment_harness.hpp"
#include "titans/context/ollama_backend.hpp"
#include "titans/context/experiment_persistence.hpp"
#include "titans/context/llm_interface.hpp"
#include <iostream>

using namespace titans::context;

// ============================================================================
// Real LLM Anomaly Detector
// ============================================================================

class RealLLMAnomalyDetector {
public:
    explicit RealLLMAnomalyDetector(std::shared_ptr<LLMBackend> backend)
        : backend_(backend) {}

    ModelOutput detect(
        const SyntheticEvent& event,
        const std::vector<SyntheticEvent>& context,
        const std::vector<ContaminationType>& active_contaminations
    ) {
        // Build prompt
        std::string event_json = format_event(event);
        std::string context_json = format_context(context);

        LLMRequest request;
        request.request_id = event.event_id;
        request.model = "";  // Use default
        request.temperature = 0.0;
        request.max_tokens = 256;
        request.response_format = "json";
        request.messages = {
            {LLMMessage::Role::System, PromptBuilder::build_system_prompt()},
            {LLMMessage::Role::User,
             "Event: " + event_json + "\n\nContext: " + context_json +
             "\n\nIs this event anomalous? Respond with JSON: {\"classification\": \"anomaly\" or \"normal\", \"confidence\": 0.0-1.0, \"reasoning\": \"...\"}"
            }
        };

        auto response = backend_->complete(request);

        ModelOutput output;
        output.task_id = event.event_id;
        output.output_time = event.timestamp;
        output.ground_truth_class = event.is_anomaly ? "anomaly" : "normal";
        output.contamination_types_present = active_contaminations;

        if (response.success) {
            auto parsed = ResponseParser::parse(response.content);
            output.predicted_class = parsed.classification;
            output.confidence = parsed.confidence;
            output.is_correct = (output.predicted_class == output.ground_truth_class);
        } else {
            output.predicted_class = "error";
            output.confidence = 0.0;
            output.is_correct = false;
        }

        return output;
    }

private:
    std::string format_event(const SyntheticEvent& e) {
        titans::json::Value v = titans::json::object({
            {"id", e.event_id},
            {"entity", e.entity_id},
            {"type", e.event_type},
            {"value", e.value},
            {"timestamp", static_cast<double>(e.timestamp)}
        });
        return titans::json::stringify(v);
    }

    std::string format_context(const std::vector<SyntheticEvent>& ctx) {
        titans::json::Array arr;
        for (const auto& e : ctx) {
            arr.push_back(titans::json::object({
                {"id", e.event_id},
                {"entity", e.entity_id},
                {"value", e.value}
            }));
        }
        return titans::json::stringify(titans::json::Value(std::move(arr)));
    }

    std::shared_ptr<LLMBackend> backend_;
};

// ============================================================================
// Real LLM Experiment Runner
// ============================================================================

class RealLLMExperimentRunner {
public:
    struct Config {
        std::string experiment_id = "llm_experiment";
        std::string model_name = "";  // Empty = use default
        ContextMethod context_method = ContextMethod::VersionedContext;
        size_t num_events = 100;
        double contamination_rate = 0.15;
        uint64_t seed = 42;
    };

    explicit RealLLMExperimentRunner(std::shared_ptr<LLMBackend> backend)
        : backend_(backend) {}

    LLMExperimentResult run(const Config& config) {
        std::cout << "Running experiment: " << config.experiment_id << std::endl;
        std::cout << "  Model: " << (config.model_name.empty() ? "(default)" : config.model_name) << std::endl;
        std::cout << "  Method: " << method_name(config.context_method) << std::endl;
        std::cout << "  Events: " << config.num_events << std::endl;
        std::cout << std::endl;

        // Generate synthetic events
        SyntheticDataGenerator gen(config.seed);
        auto events = gen.generate_event_stream(
            config.num_events, 20, 1000000, 0.1
        );

        // Create context manager
        auto ctx_mgr = create_context_manager(config.context_method);

        // Create detector
        RealLLMAnomalyDetector detector(backend_);

        // Contamination injector
        ContaminationInjector injector(config.seed + 1);
        std::mt19937_64 rng(config.seed + 2);
        std::uniform_real_distribution<double> uniform(0.0, 1.0);

        // Run inference
        std::vector<ModelOutput> outputs;
        std::vector<double> latencies;
        std::vector<SyntheticEvent> history;
        int total_prompt_tokens = 0, total_completion_tokens = 0;

        auto start_time = std::chrono::high_resolution_clock::now();

        for (size_t i = 0; i < events.size(); ++i) {
            const auto& event = events[i];

            // Determine contamination
            std::vector<ContaminationType> contaminations;
            if (uniform(rng) < config.contamination_rate) {
                contaminations.push_back(static_cast<ContaminationType>(
                    static_cast<int>(uniform(rng) * 6) + 1
                ));
            }

            // Get context
            auto context = ctx_mgr->get_context(event, history);

            // Detect
            auto iter_start = std::chrono::high_resolution_clock::now();
            auto output = detector.detect(event, context, contaminations);
            auto iter_end = std::chrono::high_resolution_clock::now();

            double latency = std::chrono::duration<double, std::milli>(iter_end - iter_start).count();
            latencies.push_back(latency);

            outputs.push_back(output);
            history.push_back(event);

            // Progress
            if ((i + 1) % 10 == 0) {
                std::cout << "\r  Progress: " << (i + 1) << "/" << events.size()
                         << " (" << (output.is_correct ? "✓" : "✗") << ")" << std::flush;
            }
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        std::cout << "\r  Progress: " << events.size() << "/" << events.size() << " Done.     " << std::endl;

        // Calculate metrics
        MetricCalculator calc;
        auto impact_metrics = calc.calculate_impact_metrics(outputs);

        // Build result
        LLMExperimentResult result;
        result.experiment_id = config.experiment_id;
        result.model_name = config.model_name.empty() ? backend_->name() : config.model_name;
        result.context_method = method_name(config.context_method);
        result.impact_metrics = impact_metrics;

        // Latency stats
        std::sort(latencies.begin(), latencies.end());
        result.avg_latency_ms = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
        result.p50_latency_ms = latencies[latencies.size() / 2];
        result.p99_latency_ms = latencies[latencies.size() * 99 / 100];

        return result;
    }

private:
    std::unique_ptr<BaselineContextManager> create_context_manager(ContextMethod method) {
        switch (method) {
            case ContextMethod::NoHistory:
                return std::make_unique<NoHistoryContext>();
            case ContextMethod::FullHistory:
                return std::make_unique<FullHistoryContext>();
            case ContextMethod::FixedWindow:
                return std::make_unique<FixedWindowContext>(50);
            case ContextMethod::TimeFilter:
                return std::make_unique<TimeFilterContext>();
            default:
                return std::make_unique<VersionedContextManager>();
        }
    }

    std::shared_ptr<LLMBackend> backend_;
};

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    std::cout << R"(
╔═══════════════════════════════════════════════════════════════════════╗
║  Titans: Real LLM Context Contamination Experiments                   ║
║  Evaluating LLM Decision Quality Under Context Contamination          ║
╚═══════════════════════════════════════════════════════════════════════╝
)" << std::endl;

    // ========================================================================
    // Step 1: Diagnose LLM backends
    // ========================================================================
    std::cout << "[1/4] Detecting available LLM backends...\n" << std::endl;

    auto diagnostics = run_diagnostics();
    diagnostics.print();

    // Select backend
    auto backend = LLMBackendFactory::auto_detect();
    if (!backend) {
        std::cerr << "\n✗ No LLM backend available!" << std::endl;
        std::cerr << "Please ensure Ollama or vLLM is running:" << std::endl;
        std::cerr << "  - Ollama: ollama serve" << std::endl;
        std::cerr << "  - vLLM:   python -m vllm.entrypoints.openai.api_server --model <model>" << std::endl;
        return 1;
    }

    std::cout << "\n✓ Using backend: " << backend->name() << std::endl;

    // ========================================================================
    // Step 2: Run baseline experiment
    // ========================================================================
    std::cout << "\n[2/4] Running baseline experiment (simulated)...\n" << std::endl;

    ExperimentConfig sim_config;
    sim_config.experiment_id = "baseline_simulated";
    sim_config.num_events = 1000;
    sim_config.contamination_rate = 0.15;

    ExperimentRunner sim_runner;
    auto sim_results = sim_runner.run_comparison(sim_config, {
        ContextMethod::NoHistory,
        ContextMethod::VersionedContext
    });
    sim_runner.print_comparison(sim_results);

    // ========================================================================
    // Step 3: Run real LLM experiment (small scale)
    // ========================================================================
    std::cout << "\n[3/4] Running real LLM experiment...\n" << std::endl;

    RealLLMExperimentRunner llm_runner(backend);

    std::vector<LLMExperimentResult> llm_results;

    // Test each context method with real LLM
    std::vector<ContextMethod> methods_to_test = {
        ContextMethod::NoHistory,
        ContextMethod::VersionedContext
    };

    for (auto method : methods_to_test) {
        RealLLMExperimentRunner::Config config;
        config.experiment_id = "real_llm_" + method_name(method);
        config.context_method = method;
        config.num_events = 50;  // Small for demo, increase for real experiments
        config.contamination_rate = 0.15;

        auto result = llm_runner.run(config);
        result.print_summary();
        llm_results.push_back(result);
    }

    // ========================================================================
    // Step 4: Save results
    // ========================================================================
    std::cout << "\n[4/4] Saving results...\n" << std::endl;

    ExperimentStore store("experiments/llm_results");
    for (const auto& result : llm_results) {
        store.save(result);
    }

    // ========================================================================
    // Summary
    // ========================================================================
    std::cout << "\n═══════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "EXPERIMENT COMPLETE" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════════" << std::endl;

    if (llm_results.size() >= 2) {
        double improvement = llm_results[1].impact_metrics.accuracy -
                            llm_results[0].impact_metrics.accuracy;
        std::cout << "\nVersionedContext vs NoHistory:" << std::endl;
        std::cout << "  Accuracy improvement: " << std::showpos << std::fixed
                  << std::setprecision(2) << improvement * 100 << "%" << std::endl;
    }

    std::cout << "\nResults saved to: experiments/llm_results/" << std::endl;
    std::cout << "Generate figures with: python python/research/generate_figures.py" << std::endl;

    return 0;
}
