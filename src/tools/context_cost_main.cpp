/**
 * @file context_cost_main.cpp
 * @brief Context length priced in freshness: what a richer prompt costs in age.
 *
 * THE TENSION
 * -----------
 * The research framework here studies context strategies. P0 established that
 * advice ages against the horizon of the claim it carries. Those two facts meet
 * in an awkward place: **prefill cost scales with context length, so a richer
 * context directly buys staleness.** "More context is better" and "fresher
 * advice is better" are in direct opposition, and neither slogan survives
 * contact with a measurement that puts them on the same axis.
 *
 * WHAT THIS MEASURES
 * ------------------
 * Two things, and the second is only interpretable given the first.
 *
 *   PART A -- the decay. How much a policy's accuracy falls purely because its
 *   answer arrives late, on real trades, over millions of decisions. No model
 *   is involved; the flow heuristic stands in for "a rule that needs no context
 *   at all", so this is the price of delay with the value of context set to
 *   zero. It is the reference line every model arm is read against.
 *
 *   PART B -- the purchase. What a model's answer costs in milliseconds as the
 *   context grows, measured on real hardware, and what it buys in accuracy. The
 *   same decisions are scored twice: against the trade that immediately
 *   followed the context, and against whatever trade is current once the answer
 *   actually arrives.
 *
 * TWO WAYS THIS COULD LIE, BOTH GUARDED
 * -------------------------------------
 *   - A serving stack asked for more context than it holds truncates silently.
 *     Prefill then plateaus, the answer stops changing, and the curve reads
 *     "more context does not help" -- a fact about a config file wearing the
 *     costume of a finding. `check_context_growth` refuses an arm whose prompt
 *     did not actually grow.
 *   - A model that answers the same thing regardless of context draws a
 *     perfectly flat line at exactly chance. `titans_llm_experiment` already
 *     learned this the hard way; the same 98%-one-class rule applies here and
 *     the run exits non-zero.
 */

#include "titans/context/binance_dataset.hpp"
#include "titans/context/llm_interface.hpp"
#include "titans/context/ollama_backend.hpp"
#include "titans/core/json.hpp"
#include "titans/eval/bootstrap.hpp"
#include "titans/eval/delivered.hpp"
#include "titans/eval/metrics.hpp"
#include "titans/lanes/flow_policy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

using namespace titans;
using namespace titans::context;
using namespace titans::eval;

namespace {

// ============================================================================
// Options
// ============================================================================

struct Options {
    std::string data_path;
    std::string model = "llama3.1:8b";
    int port = 11434;
    std::vector<std::size_t> contexts{8, 32, 128, 512};
    std::vector<std::int64_t> delays{0, 25, 50, 100, 250, 500, 1000, 2000, 4000};
    std::size_t points = 240;
    std::size_t max_rows = 400000;
    std::size_t window = 50;
    double quantile = 0.95;
    std::size_t calibration_n = 5000;
    /**
     * Deliberately below the repository's usual 5 bps. At 5 bps this tape is
     * 1.5% toxic, so a few hundred model calls would contain a handful of
     * positives and the true-positive rate would be noise. At 2 bps it is
     * 11%, which a feasible number of calls can actually resolve. The label is
     * a knob everywhere else too; what matters is that both parts of this
     * program use the same one, and that it is stated rather than defaulted.
     */
    double threshold_bps = 2.0;
    std::int64_t horizon_ms = 1000;
    int num_ctx = 16384;
    std::uint64_t seed = 42;
    std::string json_path;
    bool skip_model = false;
};

std::vector<std::size_t> parse_sizes(std::string list) {
    std::vector<std::size_t> out;
    while (!list.empty()) {
        const std::size_t pos = list.find(',');
        const std::string tok = list.substr(0, pos);
        if (!tok.empty()) out.push_back(std::stoul(tok));
        if (pos == std::string::npos) break;
        list.erase(0, pos + 1);
    }
    return out;
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (a == "--data") o.data_path = next();
        else if (a == "--model") o.model = next();
        else if (a == "--port") o.port = std::stoi(next());
        else if (a == "--contexts") o.contexts = parse_sizes(next());
        else if (a == "--delays") {
            o.delays.clear();
            for (std::size_t v : parse_sizes(next())) o.delays.push_back(static_cast<std::int64_t>(v));
        }
        else if (a == "--points") o.points = std::stoul(next());
        else if (a == "--max-rows") o.max_rows = std::stoul(next());
        else if (a == "--threshold-bps") o.threshold_bps = std::stod(next());
        else if (a == "--horizon-ms") o.horizon_ms = std::stoll(next());
        else if (a == "--num-ctx") o.num_ctx = std::stoi(next());
        else if (a == "--seed") o.seed = std::stoull(next());
        else if (a == "--json") o.json_path = next();
        else if (a == "--skip-model") o.skip_model = true;
        else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: titans_context_cost --data <aggTrades.csv> [options]\n\n"
                "  --model NAME        Ollama model (default llama3.1:8b)\n"
                "  --port N            Ollama port (default 11434)\n"
                "  --contexts LIST     trades of context per arm (default 8,32,128,512)\n"
                "  --points N          decision points per arm (default 240)\n"
                "  --delays LIST       delay sweep for the reference curve, ms\n"
                "  --threshold-bps X   toxic-flow label threshold (default 2.0)\n"
                "  --horizon-ms N      toxic-flow horizon (default 1000)\n"
                "  --num-ctx N         Ollama context window (default 16384)\n"
                "  --skip-model        reference curve only, no model calls\n"
                "  --json PATH         write the result\n\n"
                "exit: 0 ok, 1 error, 3 every model arm degenerate,\n"
                "      4 the prompt was truncated before it reached the model\n");
            std::exit(0);
        }
        else if (!a.empty() && a[0] != '-' && o.data_path.empty()) o.data_path = a;
        else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); std::exit(2); }
    }
    return o;
}

// ============================================================================
// Prompting
// ============================================================================

/**
 * @brief The task, stated as the flow rule the fast lane already applies.
 *
 * Deliberately the SAME decision the heuristic makes, so the two are comparable
 * rather than merely both present. Asking the model for something the heuristic
 * cannot do would make a nicer story and would leave no reference line.
 */
std::string system_prompt(std::int64_t horizon_ms) {
    char buf[1400];
    std::snprintf(buf, sizeof(buf),
        "You are a market microstructure analyst.\n\n"
        "You will see recent aggressive trades in time order, one per line, as a\n"
        "signed size: POSITIVE means an aggressive BUY (the buyer lifted the offer),\n"
        "NEGATIVE means an aggressive SELL (the seller hit the bid).\n\n"
        "Judge whether the CURRENT order flow is unusually one-sided compared with\n"
        "the flow shown earlier in the same list. Flow that has been persistently\n"
        "buying makes the next aggressive BUY likely to be adversely selected --\n"
        "the price moves against that buyer within %lld ms -- and vice versa.\n\n"
        "Answer with JSON only, no prose:\n"
        "  {\"one_sided\": true|false, \"direction\": \"buy\"|\"sell\"|\"none\"}\n\n"
        "\"direction\" is the side the flow has been pushing, and therefore the side\n"
        "that is dangerous. Use \"none\" when one_sided is false.",
        static_cast<long long>(horizon_ms));
    return std::string(buf);
}

std::string build_context(const std::vector<AggTrade>& trades, std::size_t end,
                          std::size_t length) {
    const std::size_t begin = end + 1 >= length ? end + 1 - length : 0;
    std::string s;
    s.reserve((end - begin + 1) * 10 + 64);
    for (std::size_t i = begin; i <= end; ++i) {
        char line[32];
        std::snprintf(line, sizeof(line), "%+.3f\n",
                      trades[i].aggressor_sign() * trades[i].quantity);
        s += line;
    }
    return s;
}

struct Answer {
    bool parsed = false;
    bool one_sided = false;
    std::int8_t direction = 0;
    std::string raw;
};

Answer parse_answer(const std::string& content) {
    Answer a;
    a.raw = content;
    // The model is asked for JSON and mostly obliges; a fenced block or a
    // sentence around it is common enough that finding the object is worth the
    // four lines. A response that cannot be parsed is counted, not guessed at.
    const std::size_t lb = content.find('{');
    const std::size_t rb = content.rfind('}');
    if (lb == std::string::npos || rb == std::string::npos || rb < lb) return a;
    const auto v = json::Parser::try_parse(content.substr(lb, rb - lb + 1));
    if (!v || !v->is_object()) return a;
    a.parsed = true;
    a.one_sided = (*v)["one_sided"].as_bool();
    const std::string dir = (*v)["direction"].as_string();
    if (dir == "buy") a.direction = +1;
    else if (dir == "sell") a.direction = -1;
    return a;
}

// ============================================================================
// Arms
// ============================================================================

/// One decision point's outcome, kept so the arm can be resampled.
struct Point {
    /// False when the model's answer could not be parsed. The slot is kept
    /// anyway so that point i means the same decision point in every arm --
    /// without that, a single parse failure in one arm silently misaligns the
    /// paired comparison below and it would still produce a number.
    bool valid = false;
    bool sized_down_at_context = false;
    bool toxic_at_context = false;
    bool delivered_scored = false;
    bool sized_down_delivered = false;
    bool toxic_delivered = false;
};

struct ArmResult {
    std::size_t context_trades = 0;
    std::size_t queries = 0;
    std::size_t parse_failures = 0;
    /// Counts of the model's ANSWER, keyed (one_sided, direction). Degeneracy
    /// has to be judged on what the model said, not on what the decision rule
    /// did with it: a model that answers {true, "buy"} every single time still
    /// produces decisions that vary, because the rule also reads the side of
    /// the trade being judged. Scoring that variation would credit the model
    /// for the aggressor-side leak the label audit already documents.
    std::size_t answer_counts[6] = {0, 0, 0, 0, 0, 0};
    std::vector<Point> points;

    double prompt_tokens = 0;      ///< median
    double prefill_ms = 0;         ///< median
    double total_ms = 0;           ///< median
    double total_p90_ms = 0;

    PolicyOutcome at_context;
    PolicyOutcome delivered;
    std::size_t delivered_unscorable = 0;

    /// Share taken by the single most common answer.
    double majority_share() const {
        std::size_t total = 0, best = 0;
        for (const std::size_t c : answer_counts) { total += c; if (c > best) best = c; }
        return total ? static_cast<double>(best) / static_cast<double>(total) : 1.0;
    }
    bool degenerate() const { return majority_share() >= 0.98; }
};

/// @brief Index into ArmResult::answer_counts for one answer.
std::size_t answer_slot(bool one_sided, std::int8_t direction) {
    return (one_sided ? 3u : 0u) + static_cast<std::size_t>(direction + 1);
}

PolicyOutcome outcome_at_context(const std::vector<Point>& pts,
                                 const std::vector<std::size_t>& idx) {
    PolicyOutcome o;
    for (const std::size_t i : idx) {
        if (!pts[i].valid) continue;
        if (pts[i].sized_down_at_context) {
            if (pts[i].toxic_at_context) ++o.avoided_toxic; else ++o.forgone_benign;
        } else {
            if (pts[i].toxic_at_context) ++o.missed_toxic; else ++o.kept_benign;
        }
    }
    return o;
}

PolicyOutcome outcome_delivered(const std::vector<Point>& pts,
                                const std::vector<std::size_t>& idx) {
    PolicyOutcome o;
    for (const std::size_t i : idx) {
        if (!pts[i].valid || !pts[i].delivered_scored) continue;
        if (pts[i].sized_down_delivered) {
            if (pts[i].toxic_delivered) ++o.avoided_toxic; else ++o.forgone_benign;
        } else {
            if (pts[i].toxic_delivered) ++o.missed_toxic; else ++o.kept_benign;
        }
    }
    return o;
}

/**
 * @brief Percentile interval for an arm's informedness, by resampling points.
 *
 * A point estimate from two hundred prompts is not a measurement, and this
 * repository refuses those elsewhere. Resampling the decision points rather
 * than the trades, because the points are what was sampled.
 */
struct Interval { double lo = 0.0, hi = 0.0; bool valid = false; };

Interval informedness_ci(const std::vector<Point>& pts, bool delivered,
                         std::uint64_t seed, std::size_t resamples = 2000) {
    Interval iv;
    if (pts.size() < 20) return iv;
    Rng rng(seed);
    std::vector<double> draws;
    draws.reserve(resamples);
    std::vector<std::size_t> idx(pts.size());
    for (std::size_t b = 0; b < resamples; ++b) {
        for (std::size_t i = 0; i < pts.size(); ++i) {
            idx[i] = static_cast<std::size_t>(rng.below(pts.size()));
        }
        const PolicyOutcome o = delivered ? outcome_delivered(pts, idx)
                                          : outcome_at_context(pts, idx);
        if (o.toxic() == 0 || o.benign() == 0) continue;   // no rate to form
        draws.push_back(o.informedness());
    }
    if (draws.size() < resamples / 2) return iv;
    std::sort(draws.begin(), draws.end());
    iv.lo = draws[static_cast<std::size_t>(draws.size() * 0.025)];
    iv.hi = draws[std::min(draws.size() - 1, static_cast<std::size_t>(draws.size() * 0.975))];
    iv.valid = true;
    return iv;
}

double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
double pct_of(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<std::size_t>(v.size() * p / 100.0))];
}

}  // namespace

// ============================================================================

int main(int argc, char** argv) {
    const Options opts = parse(argc, argv);
    if (opts.data_path.empty()) {
        std::fprintf(stderr,
            "usage: titans_context_cost --data <aggTrades.csv> [options]\n");
        return 2;
    }

    ToxicFlowLabelConfig lab;
    lab.horizon_ms = opts.horizon_ms;
    lab.threshold_bps = opts.threshold_bps;
    BinanceToxicFlowDataset ds(lab);
    std::printf("CONTEXT LENGTH AS A FRESHNESS COST\n");
    std::printf("  Loading %s\n", opts.data_path.c_str());
    if (!ds.load(opts.data_path, opts.max_rows)) {
        std::fprintf(stderr, "ERROR: %s\n", ds.error().c_str());
        return 1;
    }
    const std::vector<AggTrade>& trades = ds.trades();
    const std::vector<std::int8_t> labels = ds.build_labels();
    std::size_t toxic = 0, labelled = 0;
    for (auto l : labels) { if (l >= 0) { ++labelled; if (l > 0) ++toxic; } }
    std::printf("  %zu trades, %zu labelled, %.2f%% toxic at %.1f bps / %lld ms\n",
                trades.size(), labelled,
                labelled ? 100.0 * static_cast<double>(toxic) / static_cast<double>(labelled) : 0.0,
                opts.threshold_bps, static_cast<long long>(opts.horizon_ms));

    // ------------------------------------------------------------------
    // Threshold for the reference policy, from the first calibration_n warm
    // samples. Same shape as titans_lanes so the two are comparable.
    // ------------------------------------------------------------------
    lanes::FlowPolicy::Config cfg;
    cfg.window = opts.window;
    {
        lanes::FlowPolicy warm(cfg);
        lanes::FlowCalibrator calib;
        for (const auto& t : trades) {
            warm.observe(t.aggressor_sign() * t.quantity);
            if (!warm.warm()) continue;
            calib.add(std::abs(warm.decide().net));
            if (calib.size() + 1 >= opts.calibration_n) break;
        }
        calib.commit();
        cfg.warn_above = calib.quantile(opts.quantile);
    }
    std::printf("  reference policy: trailing flow over %zu trades, warn above %.4f\n",
                opts.window, cfg.warn_above);

    // ------------------------------------------------------------------
    // PART A -- what delay costs, with the value of context set to zero
    // ------------------------------------------------------------------
    std::printf("\nPART A -- ACCURACY AGAINST DELIVERY DELAY\n");
    std::printf("  The flow heuristic needs no context at all, so this is the price\n"
                "  of arriving late and nothing else. Every model arm below is read\n"
                "  against this line.\n\n");
    std::printf("  %8s %12s %12s %10s %12s\n",
                "delay", "informedness", "action rate", "scored", "unscorable");
    std::printf("  %s\n", std::string(60, '-').c_str());

    struct DelayPoint { std::int64_t delay_ms; double j; double action; double scorable; };
    std::vector<DelayPoint> decay;
    for (const std::int64_t d : opts.delays) {
        const DeliveredOutcome r = run_policy_delivered(trades, labels, cfg, d);
        decay.push_back({d, r.outcome.informedness(), r.outcome.action_rate(),
                         r.scorable_fraction()});
        std::printf("  %6lld ms %+12.4f %11.2f%% %10zu %12zu\n",
                    static_cast<long long>(d), r.outcome.informedness(),
                    r.outcome.action_rate() * 100.0, r.scored, r.unscorable);
        std::fflush(stdout);
    }
    if (!decay.empty()) {
        const double j0 = decay.front().j;
        std::printf("\n  At zero delay the rule is worth %+.4f. ", j0);
        // Where does half of it go?
        std::int64_t half_life = -1;
        for (const auto& p : decay) {
            if (p.j <= 0.5 * j0) { half_life = p.delay_ms; break; }
        }
        if (half_life >= 0) {
            std::printf("Half of that is gone by\n  %lld ms.\n",
                        static_cast<long long>(half_life));
        } else {
            std::printf("It has not halved anywhere in\n  the swept range, up to %lld ms.\n",
                        static_cast<long long>(decay.back().delay_ms));
        }
    }

    // Declared here so both exit paths write the same file. The first version
    // returned before the writer on --skip-model, so the reference curve was
    // printed and never recorded -- and the check downstream had nothing to
    // read, which is how the omission was found.
    std::vector<ArmResult> arms;
    ContextGrowth growth;
    std::size_t degenerate = 0;
    std::size_t points_used = 0;
    auto write_json = [&]() {
        if (opts.json_path.empty()) return;
        std::ofstream f(opts.json_path);
        if (!f) {
            std::fprintf(stderr, "WARNING: cannot write %s\n", opts.json_path.c_str());
            return;
        }
        f << "{\n  \"schema\": \"titans.context_cost.v1\",\n";
        f << "  \"model\": \"" << (opts.skip_model ? "" : opts.model) << "\",\n";
        f << "  \"skip_model\": " << (opts.skip_model ? "true" : "false") << ",\n";
        f << "  \"num_ctx\": " << opts.num_ctx << ",\n";
        f << "  \"points\": " << points_used << ",\n";
        f << "  \"threshold_bps\": " << opts.threshold_bps << ",\n";
        f << "  \"horizon_ms\": " << opts.horizon_ms << ",\n";
        f << "  \"truncated\": " << (growth.truncated ? "true" : "false") << ",\n";
        f << "  \"degenerate_arms\": " << degenerate << ",\n";
        f << "  \"decay\": [\n";
        for (std::size_t i = 0; i < decay.size(); ++i) {
            f << "    {\"delay_ms\": " << decay[i].delay_ms
              << ", \"informedness\": " << decay[i].j
              << ", \"action_rate\": " << decay[i].action
              << ", \"scorable\": " << decay[i].scorable << "}"
              << (i + 1 < decay.size() ? "," : "") << "\n";
        }
        f << "  ],\n  \"arms\": [\n";
        for (std::size_t i = 0; i < arms.size(); ++i) {
            const auto& a = arms[i];
            f << "    {\"context_trades\": " << a.context_trades
              << ", \"prompt_tokens\": " << a.prompt_tokens
              << ", \"prefill_ms\": " << a.prefill_ms
              << ", \"total_ms\": " << a.total_ms
              << ", \"total_p90_ms\": " << a.total_p90_ms
              << ", \"informedness_at_context\": " << a.at_context.informedness()
              << ", \"informedness_delivered\": " << a.delivered.informedness()
              << ", \"delivered_toxic\": " << a.delivered.toxic()
              << ", \"delivered_benign\": " << a.delivered.benign()
              << ", \"parse_failures\": " << a.parse_failures
              << ", \"majority_share\": " << a.majority_share()
              << ", \"points\": " << a.points.size()
              << ", \"degenerate\": " << (a.degenerate() ? "true" : "false") << "}"
              << (i + 1 < arms.size() ? "," : "") << "\n";
        }
        f << "  ]\n}\n";
        std::printf("\nWrote %s\n", opts.json_path.c_str());
    };

    if (opts.skip_model) {
        std::printf("\n  --skip-model: no model was queried.\n");
        write_json();
        return 0;
    }

    // ------------------------------------------------------------------
    // PART B -- what context costs, on real hardware
    // ------------------------------------------------------------------
    OllamaConfig oc;
    oc.port = opts.port;
    oc.default_model = opts.model;
    oc.num_ctx = opts.num_ctx;
    auto backend = LLMBackendFactory::create_ollama(oc);
    if (!backend->is_available()) {
        std::fprintf(stderr,
            "\nERROR: no Ollama backend on port %d. Start one, or pass --skip-model\n"
            "to get the reference curve alone. This program does not fall back to a\n"
            "stub: a stub that answers plausibly is how a run produces clean numbers\n"
            "from a model that was never queried.\n", opts.port);
        return 1;
    }

    // Decision points, stratified so the toxic class has support. Informedness
    // is TPR - FPR, both class-conditional, so it does not move with prevalence
    // -- which is exactly why stratifying is legitimate here and would not be
    // for a metric like accuracy.
    std::vector<std::size_t> pos, neg;
    const std::size_t first = std::max<std::size_t>(opts.contexts.empty() ? 1
                                                    : *std::max_element(opts.contexts.begin(),
                                                                        opts.contexts.end()),
                                                    opts.window);
    for (std::size_t i = first; i + 1 < trades.size(); ++i) {
        if (labels[i + 1] < 0) continue;
        (labels[i + 1] > 0 ? pos : neg).push_back(i);
    }
    Rng rng(opts.seed);
    auto take = [&](std::vector<std::size_t>& src, std::size_t n) {
        for (std::size_t i = src.size(); i > 1; --i) {
            std::swap(src[i - 1], src[static_cast<std::size_t>(rng.below(i))]);
        }
        if (src.size() > n) src.resize(n);
    };
    const std::size_t half = opts.points / 2;
    take(pos, half);
    take(neg, opts.points - half);
    std::vector<std::size_t> where;
    where.insert(where.end(), pos.begin(), pos.end());
    where.insert(where.end(), neg.begin(), neg.end());
    std::sort(where.begin(), where.end());

    std::printf("\nPART B -- WHAT CONTEXT COSTS AND WHAT IT BUYS\n");
    std::printf("  model %s, num_ctx %d, %zu decision points per arm\n",
                opts.model.c_str(), opts.num_ctx, where.size());
    std::printf("  points are stratified %zu toxic / %zu benign on the trade that\n"
                "  immediately follows the context. Informedness is TPR - FPR, which\n"
                "  does not move with prevalence, so this costs nothing but calls.\n",
                pos.size(), neg.size());
    if (pos.size() < 20 || neg.size() < 20) {
        std::printf("\n  [refused] %zu toxic and %zu benign points is too few to resolve\n"
                    "  a rate. Raise --points or lower --threshold-bps.\n",
                    pos.size(), neg.size());
        return 1;
    }

    const std::string sys = system_prompt(opts.horizon_ms);
    points_used = where.size();

    for (const std::size_t L : opts.contexts) {
        ArmResult arm;
        arm.context_trades = L;
        std::vector<double> toks, prefill, total;
        std::printf("\n  context %zu trades ... ", L);
        std::fflush(stdout);

        for (const std::size_t k : where) {
            LLMRequest req;
            req.model = opts.model;
            req.temperature = 0.0;
            req.max_tokens = 64;
            req.response_format = "json";
            req.messages.push_back({LLMMessage::Role::System, sys});
            req.messages.push_back({LLMMessage::Role::User,
                "Trades (oldest first):\n" + build_context(trades, k, L) +
                "\nAnswer JSON only."});

            const LLMResponse resp = backend->complete(req);
            if (!resp.success) {
                std::fprintf(stderr, "\nERROR: backend failure: %s\n",
                             resp.error_message.c_str());
                return 1;
            }
            ++arm.queries;
            toks.push_back(resp.prompt_tokens);
            prefill.push_back(resp.prefill_ms);
            total.push_back(resp.latency_ms);

            const Answer ans = parse_answer(resp.content);
            if (!ans.parsed) {
                ++arm.parse_failures;
                arm.points.push_back(Point{});   // keep the slot, see Point::valid
                continue;
            }
            ++arm.answer_counts[answer_slot(ans.one_sided, ans.direction)];

            // Scored twice against the same answer: once at the trade that
            // followed the context, once at whatever trade is current when the
            // answer actually lands.
            auto sized = [&](std::size_t target) {
                return ans.one_sided &&
                       ans.direction == static_cast<std::int8_t>(trades[target].aggressor_sign());
            };
            Point pt;
            pt.valid = true;
            pt.sized_down_at_context = sized(k + 1);
            pt.toxic_at_context = labels[k + 1] > 0;

            const std::size_t j = index_after_delay(
                trades, k, static_cast<std::int64_t>(resp.latency_ms));
            if (j >= trades.size() || labels[j] < 0) {
                ++arm.delivered_unscorable;
            } else {
                pt.delivered_scored = true;
                pt.sized_down_delivered = sized(j);
                pt.toxic_delivered = labels[j] > 0;
            }
            arm.points.push_back(pt);

            if (pt.sized_down_at_context) {
                if (pt.toxic_at_context) ++arm.at_context.avoided_toxic;
                else                     ++arm.at_context.forgone_benign;
            } else {
                if (pt.toxic_at_context) ++arm.at_context.missed_toxic;
                else                     ++arm.at_context.kept_benign;
            }
            if (pt.delivered_scored) {
                if (pt.sized_down_delivered) {
                    if (pt.toxic_delivered) ++arm.delivered.avoided_toxic;
                    else                    ++arm.delivered.forgone_benign;
                } else {
                    if (pt.toxic_delivered) ++arm.delivered.missed_toxic;
                    else                    ++arm.delivered.kept_benign;
                }
            }
        }

        arm.prompt_tokens = median_of(toks);
        arm.prefill_ms = median_of(prefill);
        arm.total_ms = median_of(total);
        arm.total_p90_ms = pct_of(total, 90);
        std::printf("%zu calls, %.0f tokens, %.0f ms\n",
                    arm.queries, arm.prompt_tokens, arm.total_ms);
        std::fflush(stdout);
        arms.push_back(arm);
    }

    // ------------------------------------------------------------------
    // Did the context reach the model?
    // ------------------------------------------------------------------
    std::vector<std::size_t> asked;
    std::vector<double> got;
    for (const auto& a : arms) { asked.push_back(a.context_trades); got.push_back(a.prompt_tokens); }
    growth = check_context_growth(asked, got);

    // ------------------------------------------------------------------
    // Report
    // ------------------------------------------------------------------
    std::printf("\n  %8s %9s %10s %10s %8s %26s %26s\n",
                "trades", "tokens", "prefill", "total", "answers",
                "J at context (95% CI)", "J delivered (95% CI)");
    std::printf("  %s\n", std::string(100, '-').c_str());
    std::uint64_t ci_seed = opts.seed + 1000;
    for (const auto& a : arms) {
        // A degenerate arm's informedness is exactly chance by construction, so
        // printing it invites someone to quote a number that means nothing.
        // Refused, the way an unresolvable percentile is refused elsewhere.
        char jc[40], jd[40];
        if (a.degenerate()) {
            std::snprintf(jc, sizeof(jc), "%25s", "degenerate");
            std::snprintf(jd, sizeof(jd), "%25s", "degenerate");
        } else {
            const Interval ic = informedness_ci(a.points, false, ci_seed++);
            const Interval id = informedness_ci(a.points, true, ci_seed++);
            if (ic.valid) {
                std::snprintf(jc, sizeof(jc), "%+.4f [%+.4f,%+.4f]",
                              a.at_context.informedness(), ic.lo, ic.hi);
            } else {
                std::snprintf(jc, sizeof(jc), "%25s", "too few to interval");
            }
            if (id.valid && a.delivered.toxic() >= 10 && a.delivered.benign() >= 10) {
                std::snprintf(jd, sizeof(jd), "%+.4f [%+.4f,%+.4f]",
                              a.delivered.informedness(), id.lo, id.hi);
            } else {
                std::snprintf(jd, sizeof(jd), "%25s", "too few to interval");
            }
        }
        std::printf("  %8zu %9.0f %8.0f ms %8.0f ms %7.0f%% %26s %26s\n",
                    a.context_trades, a.prompt_tokens, a.prefill_ms, a.total_ms,
                    100.0 * a.majority_share(), jc, jd);
    }
    std::printf("\n  \"answers\" is the share taken by the single most common answer.\n"
                "  At 100%% the model said exactly the same thing to every prompt.\n");

    // ------------------------------------------------------------------
    // What the two columns say when read separately
    // ------------------------------------------------------------------
    std::printf("\n  READ THE TWO COLUMNS APART. Every arm answers the SAME %zu\n"
                "  decision points, so the trend down each column is a within-sample\n"
                "  comparison and is the result. The absolute level is not comparable\n"
                "  with Part A: those points were stratified on the trade following\n"
                "  the context, which enriches the delivered sample in a way a\n"
                "  full-tape number is not subject to.\n", where.size());

    if (arms.size() >= 2) {
        const auto& first = arms.front();
        const auto& last = arms.back();
        std::printf("\n  at context   %+.4f -> %+.4f  across %zux more context\n",
                    first.at_context.informedness(), last.at_context.informedness(),
                    last.context_trades / std::max<std::size_t>(1, first.context_trades));
        std::printf("  delivered    %+.4f -> %+.4f  across %.0f ms -> %.0f ms of age\n",
                    first.delivered.informedness(), last.delivered.informedness(),
                    first.total_ms, last.total_ms);
        std::printf(
            "\n  If the first line is flat and the second falls, more context did not\n"
            "  make the model better and did make its answer later, and the delay is\n"
            "  doing all the work. That is the trade this item exists to price.\n");
    }

    // ------------------------------------------------------------------
    // Paired against the smallest arm. Every arm answered the SAME points, so
    // resampling them jointly and taking the difference removes the variance
    // the points themselves contribute. Comparing two independent intervals
    // instead would throw that away and call a resolved difference unresolved.
    // ------------------------------------------------------------------
    if (arms.size() >= 2) {
        std::printf("\n  PAIRED AGAINST THE SMALLEST CONTEXT (same points, same resample)\n");
        std::printf("  %8s %28s %28s\n", "trades",
                    "at context, difference", "delivered, difference");
        std::printf("  %s\n", std::string(68, '-').c_str());
        const std::vector<Point>& base = arms.front().points;
        for (std::size_t ai = 1; ai < arms.size(); ++ai) {
            const std::vector<Point>& other = arms[ai].points;
            if (other.size() != base.size()) {
                std::printf("  %8zu %28s %28s\n", arms[ai].context_trades,
                            "arms are not aligned", "arms are not aligned");
                continue;
            }
            auto paired = [&](bool delivered) {
                Rng rng(opts.seed + 7777 + ai * 2 + (delivered ? 1 : 0));
                std::vector<double> draws;
                std::vector<std::size_t> idx(base.size());
                for (std::size_t b = 0; b < 2000; ++b) {
                    for (std::size_t i = 0; i < idx.size(); ++i) {
                        idx[i] = static_cast<std::size_t>(rng.below(idx.size()));
                    }
                    const PolicyOutcome o1 = delivered ? outcome_delivered(other, idx)
                                                       : outcome_at_context(other, idx);
                    const PolicyOutcome o0 = delivered ? outcome_delivered(base, idx)
                                                       : outcome_at_context(base, idx);
                    if (o1.toxic() == 0 || o1.benign() == 0 ||
                        o0.toxic() == 0 || o0.benign() == 0) continue;
                    draws.push_back(o1.informedness() - o0.informedness());
                }
                Interval iv;
                if (draws.size() < 1000) return iv;
                std::sort(draws.begin(), draws.end());
                iv.lo = draws[static_cast<std::size_t>(draws.size() * 0.025)];
                iv.hi = draws[std::min(draws.size() - 1,
                                       static_cast<std::size_t>(draws.size() * 0.975))];
                iv.valid = true;
                return iv;
            };
            const Interval dc = paired(false);
            const Interval dd = paired(true);
            char bc[40], bd[40];
            const double at_d = arms[ai].at_context.informedness() -
                                arms.front().at_context.informedness();
            const double de_d = arms[ai].delivered.informedness() -
                                arms.front().delivered.informedness();
            if (dc.valid) std::snprintf(bc, sizeof(bc), "%+.4f [%+.4f,%+.4f]%s",
                                        at_d, dc.lo, dc.hi,
                                        (dc.lo > 0.0 || dc.hi < 0.0) ? " *" : "  ");
            else std::snprintf(bc, sizeof(bc), "%27s", "unresolvable");
            if (dd.valid) std::snprintf(bd, sizeof(bd), "%+.4f [%+.4f,%+.4f]%s",
                                        de_d, dd.lo, dd.hi,
                                        (dd.lo > 0.0 || dd.hi < 0.0) ? " *" : "  ");
            else std::snprintf(bd, sizeof(bd), "%27s", "unresolvable");
            std::printf("  %8zu %28s %28s\n", arms[ai].context_trades, bc, bd);
        }
        std::printf("\n  * marks an interval that excludes zero.\n");
    }

    std::printf("\n  For scale, what the same age costs a rule that needs no context\n"
                "  at all -- Part A, read at each arm's own median latency:\n\n");
    std::printf("  %8s %10s %18s %18s\n",
                "trades", "age", "model J delivered", "heuristic J");
    std::printf("  %s\n", std::string(60, '-').c_str());
    for (const auto& a : arms) {
        // Nearest measured delay, rather than an interpolation between two
        // points that were never measured together.
        double best = 1e300; double ref = 0.0; std::int64_t at = 0;
        for (const auto& p : decay) {
            const double dist = std::fabs(static_cast<double>(p.delay_ms) - a.total_ms);
            if (dist < best) { best = dist; ref = p.j; at = p.delay_ms; }
        }
        char jd[24];
        if (a.degenerate()) std::snprintf(jd, sizeof(jd), "%17s", "degenerate");
        else std::snprintf(jd, sizeof(jd), "%+17.4f", a.delivered.informedness());
        std::printf("  %8zu %7.0f ms %s %+18.4f  (Part A at %lld ms)\n",
                    a.context_trades, a.total_ms, jd, ref,
                    static_cast<long long>(at));
    }

    // ------------------------------------------------------------------
    // Guards
    // ------------------------------------------------------------------
    std::size_t parse_fail = 0;
    for (const auto& a : arms) { if (a.degenerate()) ++degenerate; parse_fail += a.parse_failures; }
    if (parse_fail) {
        std::printf("\n  %zu of %zu responses could not be parsed as JSON and were\n"
                    "  counted, not guessed at.\n",
                    parse_fail, arms.size() * where.size());
    }

    if (growth.checked) {
        std::printf("\n  Prompt size fits %.0f tokens of fixed overhead plus %.2f tokens\n"
                    "  per trade of context.\n",
                    growth.overhead_tokens, growth.tokens_per_unit);
    } else {
        std::printf("\n  [context growth not checked] %s\n", growth.reason.c_str());
    }

    if (growth.truncated) {
        std::printf("\nTRUNCATED -- THE CURVE ABOVE IS ABOUT A CONFIG FILE\n");
        std::printf("  %s\n", growth.reason.c_str());
        std::printf("  Past that arm the prompt was cut before the model saw it, so\n"
                    "  prefill flattens and the answer stops changing for a reason that\n"
                    "  has nothing to do with context being unhelpful. Raise --num-ctx,\n"
                    "  or use a model whose window is large enough.\n");
        return 4;
    }

    if (degenerate == arms.size()) {
        std::printf("\nDEGENERATE MODEL -- THE NUMBERS ABOVE MEASURE NOTHING\n");
        for (const auto& a : arms) {
            std::printf("  context %4zu: answered the same way on %.1f%% of prompts\n",
                        a.context_trades, 100.0 * a.majority_share());
        }
        std::printf("\n  A model that answers the same thing regardless of what it is\n"
                    "  shown scores exactly chance whatever it is shown, so every arm\n"
                    "  ties and the curve is flat by construction. That is not\n"
                    "  \"context does not matter\".\n");
    }

    write_json();

    return degenerate == arms.size() ? 3 : 0;
}
