/**
 * @file regression_main.cpp
 * @brief A regression gate for the benchmark series, calibrated on the noise it
 *        will actually face.
 *
 * WHAT WAS MISSING
 * ----------------
 * CI runs `titans_benchmark` and checks that it self-calibrates. It does not
 * compare against yesterday, so a 20% regression in `update_level` passes
 * silently. Nothing in this repository has ever compared two runs.
 *
 * WHY NOT A PERCENTAGE THRESHOLD
 * ------------------------------
 * Because a shared runner's own variance is bigger than the regression worth
 * catching, and a gate that fires every build is turned off within a week. The
 * continuous-performance literature is unanimous on the mechanism -- change
 * point detection, not thresholds -- and this program uses E-Divisive
 * (Matteson & James 2014), which is what MongoDB deployed for this problem.
 *
 * WHY NOT THE HARNESS'S OWN SPREAD EITHER
 * ---------------------------------------
 * The obvious calibration is the number the harness already reports:
 * `rep_spread_pct`, the spread across repetitions inside one process. It is the
 * wrong number, and not by a little. Repetitions inside a process share a cache
 * state, a page mapping, a clock domain and a thermal state, so anything that
 * perturbs the process perturbs all of them together. A run can be internally
 * consistent to 1.4% and 44% away from the same binary's median. This program
 * measures both and prints the ratio, so the gap is a reported quantity rather
 * than an assumption. See docs/REGRESSION.md.
 *
 * WHAT IT DOES
 * ------------
 * Reads a series of benchmark JSONs in chronological order, builds one series
 * per metric, finds level shifts, and fails only when the most recent shift is
 * both significant under permutation AND larger than the dispersion the series
 * itself shows with its steps removed.
 */

#include "titans/core/json.hpp"
#include "titans/eval/change_point.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using namespace titans;
using namespace titans::eval;

namespace {

// ============================================================================
// Options
// ============================================================================

struct Options {
    std::vector<std::string> inputs;
    /// Runs from other sessions, used only to report how far outside the
    /// series' own range a same-binary run can land.
    std::vector<std::string> references;
    std::string metric;              ///< empty => every metric
    std::string json_path;
    ChangePointConfig cfg;
    bool list_only = false;
};

void usage() {
    std::printf(
        "usage: titans_regression <dir | file.json ...> [options]\n\n"
        "  --reference FILE  a run from another session, for the timescale report\n"
        "  --metric NAME     gate only this metric (default: all)\n"
        "  --alpha X         permutation significance (default 0.05)\n"
        "  --sigmas K        shift must exceed K x series dispersion (default 3)\n"
        "  --permutations N  shuffles per test (default 999)\n"
        "  --seed S          permutation seed (default 42)\n"
        "  --min-segment N   points required on each side of a split (default 5)\n"
        "  --list            print the series and exit, no gating\n"
        "  --json PATH       write the verdict as JSON\n\n"
        "exit: 0 clean, 2 refused (series too short), 6 regression\n");
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (a == "--reference") o.references.push_back(next());
        else if (a == "--metric") o.metric = next();
        else if (a == "--alpha") o.cfg.alpha = std::stod(next());
        else if (a == "--sigmas") o.cfg.effect_sigmas = std::stod(next());
        else if (a == "--permutations") o.cfg.permutations = std::stoul(next());
        else if (a == "--seed") o.cfg.seed = std::stoull(next());
        else if (a == "--min-segment") o.cfg.min_segment = std::stoul(next());
        else if (a == "--list") o.list_only = true;
        else if (a == "--json") o.json_path = next();
        else if (a == "--help" || a == "-h") { usage(); std::exit(0); }
        else if (!a.empty() && a[0] != '-') o.inputs.push_back(a);
        else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); std::exit(2); }
    }
    return o;
}

// ============================================================================
// Loading
// ============================================================================

struct Run {
    std::string path;
    std::string host;
    std::map<std::string, double> cost_ns;
    std::map<std::string, double> spread_pct;
};

/// @brief Every .json under a directory, or the file itself. Sorted, because
///        the series' ORDER is the whole point and a directory listing is not
///        ordered by anything in particular.
std::vector<std::string> expand(const std::vector<std::string>& inputs) {
    std::vector<std::string> files;
    for (const auto& in : inputs) {
        std::error_code ec;
        if (std::filesystem::is_directory(in, ec)) {
            for (const auto& e : std::filesystem::directory_iterator(in, ec)) {
                if (e.path().extension() == ".json") files.push_back(e.path().string());
            }
        } else {
            files.push_back(in);
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool load_run(const std::string& path, Run& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    // try_parse rather than parse: a truncated or half-written file in the
    // series directory must be skipped with a warning, not abort the gate.
    const auto parsed = json::Parser::try_parse(ss.str());
    if (!parsed || !parsed->is_object()) return false;
    const json::Value& v = *parsed;

    out.path = path;
    const json::Value& machine = v["machine"];
    if (machine.is_object()) out.host = machine["hostname"].as_string();

    const json::Value& results = v["results"];
    if (!results.is_array()) return false;
    for (std::size_t i = 0; i < results.size(); ++i) {
        const json::Value& r = results[i];
        const std::string name = r["name"].as_string();
        if (name.empty()) continue;
        out.cost_ns[name] = r["cost_ns"].as_number();
        out.spread_pct[name] = r["rep_spread_pct"].as_number();
    }
    return !out.cost_ns.empty();
}

// ============================================================================
// Reporting
// ============================================================================

struct MetricReport {
    std::string name;
    std::vector<double> series;
    double median = 0.0;
    double within_pct = 0.0;     ///< median rep_spread across the series
    double between_pct = 0.0;    ///< series dispersion, level shifts removed
    double ratio = 0.0;
    GateResult gate;
};

double median_copy(std::vector<double> v) { return median_of(std::move(v)); }

const char* verdict_of(const MetricReport& m) {
    if (!m.gate.valid) return "refused";
    if (m.gate.ungated) return "bimodal";
    if (m.gate.regressed) return "REGRESSION";
    if (m.gate.improved) return "improved";
    if (m.gate.has_change) return "shift<bar";
    return "clean";
}

void print_series(const MetricReport& m) {
    std::printf("\n%s  (n=%zu)\n", m.name.c_str(), m.series.size());
    for (std::size_t i = 0; i < m.series.size(); ++i) {
        bool at_change = false;
        for (const auto& cp : m.gate.change_points) at_change |= (cp.index == i);
        std::printf("  %3zu %10.4f %s\n", i, m.series[i], at_change ? "<-- level shift" : "");
    }
}

}  // namespace

// ============================================================================

int main(int argc, char** argv) {
    const Options opts = parse(argc, argv);
    if (opts.inputs.empty()) { usage(); return 2; }

    const std::vector<std::string> files = expand(opts.inputs);
    std::vector<Run> runs;
    for (const auto& f : files) {
        Run r;
        if (load_run(f, r)) runs.push_back(std::move(r));
        else std::fprintf(stderr, "warning: could not read %s\n", f.c_str());
    }
    if (runs.empty()) {
        std::fprintf(stderr, "ERROR: no readable benchmark JSON in the inputs\n");
        return 2;
    }

    std::printf("BENCHMARK REGRESSION GATE\n");
    std::printf("  %zu runs from %s\n", runs.size(),
                runs.front().host.empty() ? "(unknown host)" : runs.front().host.c_str());
    std::printf("  E-Divisive change points, %zu permutations, alpha %.3f,\n"
                "  shift must exceed %.1fx the series' own dispersion\n",
                opts.cfg.permutations, opts.cfg.alpha, opts.cfg.effect_sigmas);

    // Build one series per metric, keeping only metrics present in every run:
    // a metric that appears halfway through is a different experiment, not a
    // continuation of one.
    std::map<std::string, std::vector<double>> series, spreads;
    for (const auto& r : runs) {
        for (const auto& [name, cost] : r.cost_ns) {
            series[name].push_back(cost);
            spreads[name].push_back(r.spread_pct.at(name));
        }
    }

    std::vector<MetricReport> reports;
    for (auto& [name, s] : series) {
        if (!opts.metric.empty() && name != opts.metric) continue;
        if (s.size() != runs.size()) {
            std::printf("  [skip] %s appears in %zu of %zu runs\n",
                        name.c_str(), s.size(), runs.size());
            continue;
        }
        MetricReport m;
        m.name = name;
        m.series = s;
        m.median = median_copy(s);
        m.within_pct = median_copy(spreads[name]);
        m.gate = evaluate_gate(s, opts.cfg);
        m.between_pct = m.median != 0.0 ? 100.0 * m.gate.sigma / m.median : 0.0;
        m.ratio = m.within_pct > 0.0 ? m.between_pct / m.within_pct : 0.0;
        reports.push_back(std::move(m));
    }

    if (opts.list_only) {
        for (const auto& m : reports) print_series(m);
        return 0;
    }

    // ------------------------------------------------------------------
    std::printf("\n%-42s %8s %8s %8s %7s %7s %8s %8s %-11s\n",
                "metric", "median", "within%", "series%", "ratio", "shift", "p", "effect", "verdict");
    std::printf("  %s\n", std::string(112, '-').c_str());
    int regressions = 0, refusals = 0;
    for (const auto& m : reports) {
        if (!m.gate.valid) ++refusals;
        if (m.gate.regressed) ++regressions;
        char shift[16] = "     -";
        char pbuf[16] = "      -";
        char effect[16] = "       -";
        if (m.gate.has_change) {
            std::snprintf(shift, sizeof(shift), "%+6.1f%%", m.gate.delta_pct);
            std::snprintf(pbuf, sizeof(pbuf), "%7.4f", m.gate.p_value);
            std::snprintf(effect, sizeof(effect), "%6.1f sd", m.gate.effect_sigmas);
        }
        std::printf("%-42s %8.3f %7.2f%% %7.2f%% %7.1f %7s %8s %8s %-11s\n",
                    m.name.c_str(), m.median, m.within_pct, m.between_pct, m.ratio,
                    shift, pbuf, effect, verdict_of(m));
    }

    // ------------------------------------------------------------------
    // Bimodal metrics, with the numbers behind the call. A label alone would
    // ask the reader to trust a threshold they cannot see.
    // ------------------------------------------------------------------
    bool any_bimodal = false;
    for (const auto& m : reports) any_bimodal |= m.gate.ungated;
    if (any_bimodal) {
        std::printf("\nTWO-MODE METRICS (detected, reported, not gated)\n");
        std::printf("  %-42s %9s %9s %9s %10s\n",
                    "metric", "fast", "slow", "gap", "separation");
        std::printf("  %s\n", std::string(86, '-').c_str());
        for (const auto& m : reports) {
            if (!m.gate.ungated) continue;
            const auto& mo = m.gate.modality;
            std::printf("  %-42s %9.3f %9.3f %8.1f%% %6.1f sd  (%.0f%% in the slow mode)\n",
                        m.name.c_str(), mo.low, mo.high, mo.gap_pct,
                        mo.separation_sigmas, 100.0 * mo.minority_fraction);
        }
        std::printf(
            "\n  These take one of two values, fixed for a process and varying\n"
            "  between them -- alignment, page colouring, and which physical core\n"
            "  the pin lands on are all decided once at start-up. The level test\n"
            "  survives it and finds nothing, which is correct. The effect-size\n"
            "  bar does not: a dispersion computed across both modes describes\n"
            "  neither, so a run of luck in the mode mix would read as a large\n"
            "  shift. They are reported and excluded from the verdict rather than\n"
            "  gated on a number that does not mean what it says.\n");
    }

    // ------------------------------------------------------------------
    // The two noise numbers, side by side. This is the part that decides
    // whether the gate is worth having.
    // ------------------------------------------------------------------
    std::vector<double> ratios;
    for (const auto& m : reports) if (m.ratio > 0.0) ratios.push_back(m.ratio);
    std::printf("\nNOISE BY TIMESCALE\n");
    std::printf("  within-run   spread across repetitions inside one process, as the\n");
    std::printf("               harness reports it. Median across metrics: %.2f%%\n",
                [&]{ std::vector<double> w; for (const auto& m : reports) w.push_back(m.within_pct);
                     return w.empty() ? 0.0 : median_copy(w); }());
    std::printf("  between-run  dispersion of THIS series with its level shifts removed.\n");
    std::printf("               Median across metrics: %.2f%%  (ratio %.1fx)\n",
                [&]{ std::vector<double> b; for (const auto& m : reports) b.push_back(m.between_pct);
                     return b.empty() ? 0.0 : median_copy(b); }(),
                ratios.empty() ? 0.0 : median_copy(ratios));

    if (!opts.references.empty()) {
        std::vector<Run> refs;
        for (const auto& f : opts.references) {
            Run r;
            if (load_run(f, r)) refs.push_back(std::move(r));
        }
        std::size_t outside = 0, checked = 0;
        double worst = 0.0;
        std::string worst_name;
        for (const auto& r : refs) {
            for (const auto& m : reports) {
                auto it = r.cost_ns.find(m.name);
                if (it == r.cost_ns.end()) continue;
                ++checked;
                const double lo = *std::min_element(m.series.begin(), m.series.end());
                const double hi = *std::max_element(m.series.begin(), m.series.end());
                if (it->second < lo || it->second > hi) ++outside;
                const double dev = m.median != 0.0
                    ? 100.0 * std::fabs(it->second - m.median) / m.median : 0.0;
                if (dev > worst) { worst = dev; worst_name = m.name; }
            }
        }
        std::printf("  between-session  %zu reference run(s) from another session, same\n",
                    refs.size());
        std::printf("               binary: %zu of %zu metrics land OUTSIDE this series'\n",
                    outside, checked);
        std::printf("               full range. Worst deviation from the series median:\n");
        std::printf("               %.1f%% on %s.\n", worst, worst_name.c_str());
        std::printf(
            "\n  A gate calibrated on the within-run number would fire on those.\n"
            "  A gate calibrated on this series would fire on some of them. The\n"
            "  dispersion a benchmark shows depends on the span it is measured\n"
            "  over, and the gate has to be calibrated at the span it runs at --\n"
            "  which for CI is across commits and days, the widest one here.\n");
    }

    // ------------------------------------------------------------------
    if (!opts.json_path.empty()) {
        std::ofstream o(opts.json_path);
        if (o) {
            o << "{\n  \"schema\": \"titans.regression.v1\",\n";
            o << "  \"runs\": " << runs.size() << ",\n";
            o << "  \"alpha\": " << opts.cfg.alpha << ",\n";
            o << "  \"effect_sigmas\": " << opts.cfg.effect_sigmas << ",\n";
            o << "  \"regressions\": " << regressions << ",\n";
            o << "  \"metrics\": [\n";
            for (std::size_t i = 0; i < reports.size(); ++i) {
                const auto& m = reports[i];
                o << "    { \"name\": \"" << m.name << "\", \"n\": " << m.series.size()
                  << ", \"median_ns\": " << m.median
                  << ", \"within_run_pct\": " << m.within_pct
                  << ", \"series_pct\": " << m.between_pct
                  << ", \"change_points\": " << m.gate.change_points.size()
                  << ", \"shift_pct\": " << (m.gate.has_change ? m.gate.delta_pct : 0.0)
                  << ", \"p_value\": " << m.gate.p_value
                  << ", \"effect_sigmas\": " << m.gate.effect_sigmas
                  << ", \"verdict\": \"" << verdict_of(m) << "\" }"
                  << (i + 1 < reports.size() ? "," : "") << "\n";
            }
            o << "  ]\n}\n";
            std::printf("\nWrote %s\n", opts.json_path.c_str());
        }
    }

    std::printf("\nVERDICT\n");
    if (refusals == static_cast<int>(reports.size()) && !reports.empty()) {
        std::printf("  REFUSED. %s\n", reports.front().gate.refusal.c_str());
        std::printf("  A short series is reported as unchecked rather than as clean;\n"
                    "  the two look identical on a green build and they are not.\n");
        return 2;
    }
    if (regressions > 0) {
        std::printf("  %d metric(s) REGRESSED: a level shift that is significant and\n"
                    "  larger than %.1fx this series' own dispersion.\n",
                    regressions, opts.cfg.effect_sigmas);
        return 6;
    }
    std::printf("  Clean. No metric shows a level shift that clears both bars.\n");
    return 0;
}
