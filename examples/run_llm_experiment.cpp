/**
 * @file run_llm_experiment.cpp
 * @brief Context contamination experiment against a live language model.
 *
 * WHAT CHANGED AND WHY
 * --------------------
 * The previous version of this program could not have measured what it claimed:
 *
 *   1. It constructed a ContaminationInjector and never called it. The
 *      contamination vector was attached to the output as metadata while the
 *      prompt stayed clean, so the independent variable was never applied.
 *   2. The event carried `"type": "anomaly"` straight into the prompt, because
 *      the generator wrote the label into that field. The task was to copy a
 *      string.
 *   3. Each method ran once, so a method effect and a contamination effect
 *      could not be separated.
 *
 * This version applies contamination to the context the model actually reads
 * (ContextContaminator), uses a generator whose labels are not recoverable from
 * the event (guarded by test_task_design.cpp), and runs a PAIRED design: the
 * same events, same seed, with contamination off and on. The difference between
 * the two arms is attributable to contamination and nothing else.
 *
 * Every raw model response is written to the results file. A number nobody can
 * audit back to a prompt and a completion is not evidence.
 */

#include "titans/context/experiment_harness.hpp"
#include "titans/context/context_contaminator.hpp"
#include "titans/context/ollama_backend.hpp"
#include "titans/context/experiment_persistence.hpp"
#include "titans/context/llm_interface.hpp"
#include "titans/core/json.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <string>
#include <vector>

using namespace titans;
using namespace titans::context;

namespace {

// ============================================================================
// Options
// ============================================================================

struct Options {
    std::string backend = "auto";     // auto | ollama | vllm
    std::string host = "localhost";
    int port = 0;                     // 0 => backend default
    std::string model;                // empty => server default
    size_t num_events = 120;
    size_t num_entities = 12;
    double contamination_rate = 0.35;
    uint64_t seed = 42;
    std::string out_dir = "results/llm";
    std::vector<ContextMethod> methods = {
        ContextMethod::NoHistory,
        ContextMethod::FixedWindow,
        ContextMethod::VersionedContext,
    };
};

const char* method_label(ContextMethod m) {
    switch (m) {
        case ContextMethod::NoHistory:        return "NoHistory";
        case ContextMethod::FullHistory:      return "FullHistory";
        case ContextMethod::FixedWindow:      return "FixedWindow";
        case ContextMethod::TimeFilter:       return "TimeFilter";
        case ContextMethod::RollingSummary:   return "RollingSummary";
        case ContextMethod::VectorRetrieval:  return "VectorRetrieval";
        case ContextMethod::VersionedContext: return "VersionedContext";
        default:                              return "VersionedIntegrity";
    }
}

const char* contamination_label(ContaminationType t) {
    switch (t) {
        case ContaminationType::StaleState:              return "StaleState";
        case ContaminationType::EntityBinding:           return "EntityBinding";
        case ContaminationType::InferencePersistence:    return "InferencePersistence";
        case ContaminationType::SummaryContamination:    return "SummaryContamination";
        case ContaminationType::RetrievalContamination:  return "RetrievalContamination";
        case ContaminationType::CrossAgentContamination: return "CrossAgent";
        default:                                         return "Unknown";
    }
}

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? argv[++i] : std::string{};
        };
        if (a == "--backend")           o.backend = next();
        else if (a == "--host")         o.host = next();
        else if (a == "--port")         o.port = std::atoi(next().c_str());
        else if (a == "--model")        o.model = next();
        else if (a == "--events")       o.num_events = static_cast<size_t>(std::atoi(next().c_str()));
        else if (a == "--entities")     o.num_entities = static_cast<size_t>(std::atoi(next().c_str()));
        else if (a == "--contamination") o.contamination_rate = std::atof(next().c_str());
        else if (a == "--seed")         o.seed = static_cast<uint64_t>(std::atoll(next().c_str()));
        else if (a == "--out")          o.out_dir = next();
        else if (a == "--help" || a == "-h") {
            std::printf(
                "Usage: titans_llm_experiment [options]\n"
                "  --backend auto|ollama|vllm   inference backend (default auto)\n"
                "  --host HOST --port N         endpoint (default per backend)\n"
                "  --model NAME                 model id; empty uses server default\n"
                "  --events N                   events per arm (default 120)\n"
                "  --entities N                 distinct entities (default 12)\n"
                "  --contamination R            contamination rate (default 0.35)\n"
                "  --seed N                     RNG seed (default 42)\n"
                "  --out DIR                    results directory\n");
            std::exit(0);
        }
    }
    return o;
}

// ============================================================================
// Prompting
// ============================================================================

std::string build_system_prompt() {
    return
        "You are an anomaly detector for a market data feed.\n"
        "\n"
        "You receive ONE event under review and a set of CONTEXT RECORDS. Decide "
        "whether the event's `value` is anomalous FOR ITS OWN ENTITY, judged "
        "against that entity's recent values in the context. Entities sit at "
        "very different levels, so there is no global threshold: a value that is "
        "normal for one entity may be extreme for another.\n"
        "\n"
        "Context records are not all trustworthy. A record may be:\n"
        "  - outdated, restamped to look current;\n"
        "  - attributed to the wrong entity;\n"
        "  - a `prior_inference`, `summary`, or `agent_b_claim` rather than a "
        "measurement.\n"
        "Weigh records by their reliability. Prefer recent, first-hand "
        "measurements of the entity in question.\n"
        "\n"
        "Reply with ONLY a JSON object and nothing else. Keep `reasoning` to at\n"
        "most six words -- it is recorded for auditing, not scored, and long\n"
        "answers cost generation time without changing the classification:\n"
        "{\"classification\": \"anomaly\" or \"normal\", "
        "\"confidence\": 0.0 to 1.0, \"reasoning\": \"<=6 words\"}";
}

std::string format_event(const SyntheticEvent& e) {
    // NOTE: `type` here is a market category (trade/quote/cancel) drawn
    // independently of the label. It used to be the literal string "anomaly" or
    // "normal", which handed the model the answer.
    json::Value v = json::object({
        {"id", e.event_id},
        {"entity", e.entity_id},
        {"type", e.event_type},
        {"value", e.value},
        {"timestamp", static_cast<double>(e.timestamp)},
    });
    return json::stringify(v);
}

std::string format_context(const std::vector<SyntheticEvent>& ctx,
                           const SyntheticEvent& current) {
    json::Array arr;
    for (const auto& e : ctx) {
        if (e.event_id == current.event_id) continue;   // don't echo the question
        arr.push_back(json::object({
            {"entity", e.entity_id},
            {"kind", e.event_type},      // exposes prior_inference / summary / claim
            {"value", e.value},
            {"timestamp", static_cast<double>(e.timestamp)},
        }));
    }
    return json::stringify(json::Value(std::move(arr)));
}

// ============================================================================
// One trial
// ============================================================================

struct Trial {
    std::string event_id;
    std::string entity_id;
    bool ground_truth_anomaly = false;
    std::string predicted;          // "anomaly" | "normal" | "unparseable" | "error"
    double confidence = 0.0;
    bool correct = false;
    bool parse_ok = false;
    bool request_ok = false;
    double latency_ms = 0.0;
    int prompt_tokens = 0;
    int completion_tokens = 0;
    size_t context_size = 0;
    std::vector<ContaminationType> contaminations;   // effective ones only
    std::string raw_response;
    std::string error;
};

/// @brief Aggregate outcome for one (method, arm) pair.
struct ArmResult {
    std::string method;
    bool contaminated_arm = false;
    std::vector<Trial> trials;

    size_t answered() const {
        size_t n = 0;
        for (const auto& t : trials) if (t.parse_ok) ++n;
        return n;
    }

    /**
     * @brief Balanced accuracy over trials the model actually answered.
     *
     * Balanced rather than raw, because the anomaly class is ~10% and a model
     * that always says "normal" scores 0.90 raw while detecting nothing.
     * Unparseable and failed requests are excluded from the rate and reported
     * separately: folding them in as wrong answers conflates a serving problem
     * with a reasoning one.
     */
    double balanced_accuracy() const {
        int tp = 0, fn = 0, tn = 0, fp = 0;
        for (const auto& t : trials) {
            if (!t.parse_ok) continue;
            const bool pred = (t.predicted == "anomaly");
            if (t.ground_truth_anomaly) { pred ? ++tp : ++fn; }
            else                        { pred ? ++fp : ++tn; }
        }
        const double tpr = (tp + fn) ? static_cast<double>(tp) / (tp + fn) : 0.0;
        const double tnr = (tn + fp) ? static_cast<double>(tn) / (tn + fp) : 0.0;
        return 0.5 * (tpr + tnr);
    }

    double parse_failure_rate() const {
        if (trials.empty()) return 0.0;
        size_t bad = 0;
        for (const auto& t : trials) if (!t.parse_ok) ++bad;
        return static_cast<double>(bad) / trials.size();
    }

    /**
     * @brief Fraction of answered trials given to the most common prediction.
     *
     * 1.0 means the model answered identically every time. A constant
     * classifier scores exactly 0.5 balanced accuracy no matter what it is
     * shown, so every arm ties and the experiment silently measures nothing --
     * which is what a 1.5B model did here, answering "anomaly" to all 50 events
     * in all six arms.
     */
    double majority_share() const {
        std::map<std::string, size_t> counts;
        size_t total = 0;
        for (const auto& t : trials) {
            if (!t.parse_ok) continue;
            ++counts[t.predicted];
            ++total;
        }
        if (total == 0) return 1.0;
        size_t best = 0;
        for (const auto& [k, v] : counts) best = std::max(best, v);
        return static_cast<double>(best) / total;
    }

    /// @brief The prediction the model gave most often.
    std::string majority_class() const {
        std::map<std::string, size_t> counts;
        for (const auto& t : trials) {
            if (t.parse_ok) ++counts[t.predicted];
        }
        std::string best;
        size_t n = 0;
        for (const auto& [k, v] : counts) {
            if (v > n) { n = v; best = k; }
        }
        return best;
    }

    /// @brief True when the model is effectively answering the same thing always.
    bool is_degenerate() const { return majority_share() >= 0.95; }

    double mean_latency_ms() const {
        if (trials.empty()) return 0.0;
        double s = 0;
        for (const auto& t : trials) s += t.latency_ms;
        return s / trials.size();
    }

    size_t contaminated_trials() const {
        size_t n = 0;
        for (const auto& t : trials) if (!t.contaminations.empty()) ++n;
        return n;
    }
};

std::unique_ptr<BaselineContextManager> make_context_manager(ContextMethod m) {
    switch (m) {
        case ContextMethod::NoHistory:   return std::make_unique<NoHistoryContext>();
        case ContextMethod::FullHistory: return std::make_unique<FullHistoryContext>();
        case ContextMethod::FixedWindow: return std::make_unique<FixedWindowContext>(50);
        case ContextMethod::TimeFilter:  return std::make_unique<TimeFilterContext>();
        default:                         return std::make_unique<VersionedContextManager>();
    }
}

/**
 * @brief Run one arm: all events under one method, contamination on or off.
 *
 * Both arms consume identical events and identical contamination draws. When
 * `contaminate` is false the draw is still performed and discarded, so the two
 * arms stay aligned event for event and the only difference is whether the
 * mutation reached the prompt.
 */
ArmResult run_arm(const std::shared_ptr<LLMBackend>& backend,
                  const Options& opts,
                  ContextMethod method,
                  const std::vector<SyntheticEvent>& events,
                  bool contaminate) {
    ArmResult arm;
    arm.method = method_label(method);
    arm.contaminated_arm = contaminate;

    auto ctx_mgr = make_context_manager(method);
    ContextContaminator contaminator(opts.seed + 1);

    std::mt19937_64 rng(opts.seed + 2);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);

    std::vector<SyntheticEvent> history;
    history.reserve(events.size());

    for (size_t i = 0; i < events.size(); ++i) {
        const auto& event = events[i];

        // Draw identically in both arms so the pairing holds.
        std::vector<ContaminationType> drawn;
        if (uniform(rng) < opts.contamination_rate) {
            const int which = static_cast<int>(uniform(rng) * 6) + 1;
            drawn.push_back(static_cast<ContaminationType>(which));
        }

        auto context = ctx_mgr->get_context(event, history);

        Trial t;
        t.event_id = event.event_id;
        t.entity_id = event.entity_id;
        t.ground_truth_anomaly = event.is_anomaly;

        if (contaminate && !drawn.empty()) {
            auto cc = contaminator.apply(event, context, history, drawn);
            context = std::move(cc.events);
            // Only contaminations that were actually applied count as treatment.
            // Counting skipped attempts would silently dilute the treated group.
            t.contaminations = cc.effective_types();
        }
        t.context_size = context.size();

        LLMRequest req;
        req.request_id = event.event_id;
        req.model = opts.model;
        req.temperature = 0.0;
        req.max_tokens = 64;
        req.messages = {
            {LLMMessage::Role::System, build_system_prompt()},
            {LLMMessage::Role::User,
             "## Event under review\n" + format_event(event) +
             "\n\n## Context records\n" + format_context(context, event) +
             "\n\nIs the event's value anomalous for its entity? JSON only."},
        };

        const auto t0 = std::chrono::steady_clock::now();
        const auto resp = backend->complete(req);
        const auto t1 = std::chrono::steady_clock::now();
        t.latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        t.request_ok = resp.success;
        t.raw_response = resp.content;
        t.prompt_tokens = resp.prompt_tokens;
        t.completion_tokens = resp.completion_tokens;

        if (!resp.success) {
            t.predicted = "error";
            t.error = resp.error_message;
        } else {
            const auto parsed = ResponseParser::parse(resp.content);
            if (parsed.valid &&
                (parsed.classification == "anomaly" || parsed.classification == "normal")) {
                t.parse_ok = true;
                t.predicted = parsed.classification;
                t.confidence = parsed.confidence;
                t.correct = (t.predicted == (event.is_anomaly ? "anomaly" : "normal"));
            } else {
                t.predicted = "unparseable";
                t.error = parsed.parse_error;
            }
        }

        arm.trials.push_back(std::move(t));
        history.push_back(event);

        if ((i + 1) % 10 == 0 || i + 1 == events.size()) {
            std::printf("\r    %s / %s: %zu/%zu",
                        arm.method.c_str(), contaminate ? "contaminated" : "clean",
                        i + 1, events.size());
            std::fflush(stdout);
        }
    }
    std::printf("\n");
    return arm;
}

// ============================================================================
// Reporting
// ============================================================================

void write_results(const Options& opts,
                   const std::string& backend_name,
                   const std::vector<ArmResult>& arms) {
    std::string cmd = "mkdir -p '" + opts.out_dir + "'";
    if (std::system(cmd.c_str()) != 0) {
        std::fprintf(stderr, "warning: could not create %s\n", opts.out_dir.c_str());
    }
    const std::string path = opts.out_dir + "/llm_experiment.json";
    std::ofstream f(path);
    if (!f) {
        std::fprintf(stderr, "warning: could not write %s\n", path.c_str());
        return;
    }

    f << "{\n";
    f << "  \"schema\": \"titans.llm_experiment.v1\",\n";
    f << "  \"backend\": \"" << backend_name << "\",\n";
    f << "  \"model\": \"" << (opts.model.empty() ? "(server default)" : opts.model) << "\",\n";
    f << "  \"events_per_arm\": " << opts.num_events << ",\n";
    f << "  \"entities\": " << opts.num_entities << ",\n";
    f << "  \"contamination_rate\": " << opts.contamination_rate << ",\n";
    f << "  \"seed\": " << opts.seed << ",\n";
    f << "  \"arms\": [\n";
    for (size_t a = 0; a < arms.size(); ++a) {
        const auto& arm = arms[a];
        f << "    {\n";
        f << "      \"method\": \"" << arm.method << "\",\n";
        f << "      \"contaminated\": " << (arm.contaminated_arm ? "true" : "false") << ",\n";
        f << "      \"balanced_accuracy\": " << arm.balanced_accuracy() << ",\n";
        f << "      \"answered\": " << arm.answered() << ",\n";
        f << "      \"parse_failure_rate\": " << arm.parse_failure_rate() << ",\n";
        f << "      \"majority_share\": " << arm.majority_share() << ",\n";
        f << "      \"majority_class\": \"" << arm.majority_class() << "\",\n";
        f << "      \"degenerate\": " << (arm.is_degenerate() ? "true" : "false") << ",\n";
        f << "      \"contaminated_trials\": " << arm.contaminated_trials() << ",\n";
        f << "      \"mean_latency_ms\": " << arm.mean_latency_ms() << ",\n";
        f << "      \"trials\": [\n";
        for (size_t i = 0; i < arm.trials.size(); ++i) {
            const auto& t = arm.trials[i];
            f << "        {\"event\": \"" << t.event_id << "\""
              << ", \"entity\": \"" << t.entity_id << "\""
              << ", \"truth\": \"" << (t.ground_truth_anomaly ? "anomaly" : "normal") << "\""
              << ", \"pred\": \"" << t.predicted << "\""
              << ", \"confidence\": " << t.confidence
              << ", \"parse_ok\": " << (t.parse_ok ? "true" : "false")
              << ", \"context_size\": " << t.context_size
              << ", \"latency_ms\": " << t.latency_ms
              << ", \"prompt_tokens\": " << t.prompt_tokens
              << ", \"contaminations\": [";
            for (size_t c = 0; c < t.contaminations.size(); ++c) {
                f << "\"" << contamination_label(t.contaminations[c]) << "\"";
                if (c + 1 < t.contaminations.size()) f << ", ";
            }
            f << "]"
              << ", \"raw\": " << json::stringify(json::Value(t.raw_response))
              << "}";
            if (i + 1 < arm.trials.size()) f << ",";
            f << "\n";
        }
        f << "      ]\n";
        f << "    }";
        if (a + 1 < arms.size()) f << ",";
        f << "\n";
    }
    f << "  ]\n";
    f << "}\n";
    std::printf("\nRaw trials (prompt metadata + every model response) written to %s\n",
                path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    const Options opts = parse_args(argc, argv);

    std::printf("Titans: context contamination against a live model\n\n");

    // ------------------------------------------------------------------
    // Backend
    // ------------------------------------------------------------------
    std::shared_ptr<LLMBackend> backend;
    if (opts.backend == "ollama") {
        OllamaConfig c;
        c.host = opts.host;
        if (opts.port) c.port = opts.port;
        if (!opts.model.empty()) c.default_model = opts.model;
        backend = LLMBackendFactory::create_ollama(c);
    } else if (opts.backend == "vllm") {
        VLLMConfig c;
        c.host = opts.host;
        if (opts.port) c.port = opts.port;
        backend = LLMBackendFactory::create_vllm(c);
    } else {
        backend = LLMBackendFactory::auto_detect();
    }

    if (!backend || !backend->is_available()) {
        std::fprintf(stderr,
            "No reachable inference backend.\n\n"
            "This program requires a live model. It will not fall back to a\n"
            "stand-in, because a number produced without querying a model is\n"
            "not evidence about a model.\n\n"
            "Start one of:\n"
            "  vLLM   vllm serve <model> --port 8000\n"
            "  Ollama ollama serve\n"
            "  CPU    python python/serving/cpu_shim.py --port 8011\n"
            "         (then pass --backend vllm --port 8011)\n");
        return 1;
    }
    std::printf("Backend: %s", backend->name().c_str());
    const auto models = backend->available_models();
    if (!models.empty()) std::printf("  models: %s", models.front().c_str());
    std::printf("\n");

    // ------------------------------------------------------------------
    // Data: one stream, reused by every arm so comparisons are paired.
    // ------------------------------------------------------------------
    SyntheticDataGenerator gen(opts.seed);
    const auto events = gen.generate_event_stream(
        opts.num_events, opts.num_entities, /*interval_ns=*/1000000, /*anomaly_rate=*/0.15);

    size_t n_anom = 0;
    for (const auto& e : events) n_anom += e.is_anomaly ? 1 : 0;
    std::printf("Data: %zu events, %zu entities, %zu anomalies (%.1f%%)\n\n",
                events.size(), opts.num_entities, n_anom,
                100.0 * n_anom / events.size());

    // ------------------------------------------------------------------
    // Run both arms for every method.
    // ------------------------------------------------------------------
    std::vector<ArmResult> arms;
    for (auto method : opts.methods) {
        arms.push_back(run_arm(backend, opts, method, events, /*contaminate=*/false));
        arms.push_back(run_arm(backend, opts, method, events, /*contaminate=*/true));
    }

    // ------------------------------------------------------------------
    // Report
    // ------------------------------------------------------------------
    std::printf("\n%-18s %8s %12s %9s %10s %9s %10s\n",
                "Method", "clean", "contaminated", "delta", "contam. n",
                "unparsed", "1-class");
    std::printf("%s\n", std::string(84, '-').c_str());

    for (size_t i = 0; i + 1 < arms.size(); i += 2) {
        const auto& clean = arms[i];
        const auto& dirty = arms[i + 1];
        const double d = dirty.balanced_accuracy() - clean.balanced_accuracy();
        std::printf("%-18s %8.4f %12.4f %+9.4f %10zu %8.1f%% %9.0f%%\n",
                    clean.method.c_str(),
                    clean.balanced_accuracy(),
                    dirty.balanced_accuracy(),
                    d,
                    dirty.contaminated_trials(),
                    100.0 * dirty.parse_failure_rate(),
                    100.0 * std::max(clean.majority_share(), dirty.majority_share()));
    }

    std::printf(
        "\nBalanced accuracy (mean of per-class recall; chance = 0.5) over trials\n"
        "the model answered in parseable form. Unparsed responses are reported\n"
        "separately rather than scored as wrong, so a serving problem is not\n"
        "mistaken for a reasoning one.\n"
        "\n"
        "Both arms of each pair consume identical events, identical seeds, and\n"
        "identical contamination draws; the only difference is whether the\n"
        "mutation reached the prompt. 'contam. n' is the number of trials where\n"
        "a contamination was actually applied -- attempts that could not be\n"
        "applied (too little history to draw a stale value from, for instance)\n"
        "are excluded rather than counted as treated.\n");

    // ------------------------------------------------------------------
    // Degeneracy gate. This runs BEFORE any interpretation of the deltas,
    // because a constant classifier makes every delta zero by construction.
    // ------------------------------------------------------------------
    size_t degenerate_arms = 0;
    for (const auto& a : arms) {
        if (a.is_degenerate()) ++degenerate_arms;
    }
    if (degenerate_arms > 0) {
        std::printf(
            "\n"
            "================================================================\n"
            " DEGENERATE MODEL -- THE NUMBERS ABOVE MEASURE NOTHING\n"
            "================================================================\n");
        for (const auto& a : arms) {
            if (!a.is_degenerate()) continue;
            std::printf("  %-18s %-13s answered \"%s\" on %.0f%% of trials\n",
                        a.method.c_str(),
                        a.contaminated_arm ? "(contaminated)" : "(clean)",
                        a.majority_class().c_str(), a.majority_share() * 100.0);
        }
        std::printf(
            "\n"
            "  %zu of %zu arms are effectively constant classifiers. A model that\n"
            "  answers the same thing regardless of input scores exactly 0.5\n"
            "  balanced accuracy no matter what it is shown, so every arm ties\n"
            "  and every delta is zero -- by construction, not by finding.\n"
            "\n"
            "  Nothing about contamination can be concluded from this run. The\n"
            "  pipeline is fine (context sizes and latencies differ per method,\n"
            "  and contaminations reached the prompt); the model is not doing\n"
            "  the task. Use a larger model, or fix the prompt, and re-run.\n",
            degenerate_arms, arms.size());
    }

    if (opts.num_events < 200) {
        std::printf(
            "\nNOTE: %zu events per arm is a smoke-scale run. Deltas of a few\n"
            "points are within sampling noise at this size and should not be\n"
            "reported as effects. Use --events 1000 or more, and several seeds,\n"
            "before drawing conclusions.\n", opts.num_events);
    }

    write_results(opts, backend->name(), arms);

    // Exit non-zero on a degenerate run. A pipeline that treats "it produced a
    // results file" as success would otherwise archive a table of 0.5000s as
    // though it were data.
    return degenerate_arms > 0 ? 3 : 0;
}
