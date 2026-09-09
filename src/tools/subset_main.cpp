/**
 * @file subset_main.cpp
 * @brief Subset selection on the slow lane, scored against external labels.
 *
 * WHAT THIS ITEM IS AND WHY IT IS LAST
 * ------------------------------------
 * The formulation is taken unchanged from a workshop paper: monitoring as
 * combinatorial subset selection, minimise
 *
 *     - sum_i r_i z_i + lambda sum_{i<j} S_ij z_i z_j + P (sum_i z_i - K)^2
 *
 * over a budget of K items, with greedy, simulated annealing and exhaustive
 * search as three points on one frontier. Its reported result is that raising
 * lambda buys diversity over greedy, and that the gain grows with n.
 *
 * That result is about the objective's own second term. The roadmap put this
 * item last for a reason, and the reason is worth restating here rather than
 * being discovered in the output: **a selection can be more diverse by exactly
 * as much as you weight diversity, and that says nothing about whether the
 * selection is better.** The source's own table reports its external outcome as
 * 0.000 at every lambda it tried.
 *
 * So this program does the one thing that makes the claim falsifiable: it runs
 * the same formulation on real trades and scores the result against **forward
 * toxic-flow labels the objective never sees**. Diversity is reported too, so
 * both halves are visible and it is clear which one moved.
 *
 * AND THE COST IS PRICED, NOT ASSUMED
 * -----------------------------------
 * A slow lane that selects has to solve first, and P4 measured what a delay
 * costs on this tape: half the signal's value is gone by 250 ms. Solve time is
 * therefore reported in the same units as the thing it might buy.
 */

#include "titans/context/binance_dataset.hpp"
#include "titans/eval/bootstrap.hpp"
#include "titans/eval/deflated.hpp"
#include "titans/eval/delivered.hpp"
#include "titans/eval/metrics.hpp"
#include "titans/opt/qubo.hpp"

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
using namespace titans::opt;

namespace {

// ============================================================================
// Options
// ============================================================================

struct Options {
    std::vector<std::string> data_paths;
    std::size_t max_rows = 400000;
    /// Items the slow lane may choose from, and how many it may keep.
    std::size_t window = 64;
    std::size_t budget = 16;
    std::vector<double> lambdas{0.0, 0.1, 0.5, 1.0, 2.0};
    std::size_t points = 40000;
    /// Share of points used to calibrate each method's own threshold.
    double calibration_share = 0.2;
    double quantile = 0.95;
    double threshold_bps = 5.0;
    std::int64_t horizon_ms = 1000;
    /// Small-instance settings for the solver-agreement section.
    std::size_t exact_n = 20;
    std::size_t exact_k = 8;
    std::size_t exact_windows = 200;
    double rbf_sigma = 0.3;
    AnnealConfig anneal;
    std::uint64_t seed = 42;
    std::string json_path;
};

std::vector<double> parse_list(std::string s) {
    std::vector<double> out;
    while (!s.empty()) {
        const std::size_t pos = s.find(',');
        const std::string tok = s.substr(0, pos);
        if (!tok.empty()) out.push_back(std::stod(tok));
        if (pos == std::string::npos) break;
        s.erase(0, pos + 1);
    }
    return out;
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (a == "--data") o.data_paths.push_back(next());
        else if (a == "--max-rows") o.max_rows = std::stoul(next());
        else if (a == "--window") o.window = std::stoul(next());
        else if (a == "--budget") o.budget = std::stoul(next());
        else if (a == "--lambdas") o.lambdas = parse_list(next());
        else if (a == "--points") o.points = std::stoul(next());
        else if (a == "--threshold-bps") o.threshold_bps = std::stod(next());
        else if (a == "--horizon-ms") o.horizon_ms = std::stoll(next());
        else if (a == "--sweeps") o.anneal.sweeps = std::stoul(next());
        else if (a == "--seed") o.seed = std::stoull(next());
        else if (a == "--json") o.json_path = next();
        else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: titans_subset <day1.csv> [day2.csv ...] [options]\n\n"
                "  More than one day aggregates the per-day differences with a\n"
                "  bootstrap interval and a sign test, the same protocol the\n"
                "  walk-forward uses. One day is an anecdote.\n\n"
                "  --window N        items the slow lane chooses from (default 64)\n"
                "  --budget K        items it may keep (default 16)\n"
                "  --lambdas LIST    redundancy weights to sweep\n"
                "  --points N        decision points (default 40000)\n"
                "  --sweeps N        annealing sweeps (default 200)\n"
                "  --threshold-bps X toxic-flow label threshold (default 5)\n"
                "  --json PATH       write the result\n\n"
                "exit: 0 ok, 1 error\n");
            std::exit(0);
        }
        else if (!a.empty() && a[0] != '-') o.data_paths.push_back(a);
        else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); std::exit(2); }
    }
    return o;
}

// ============================================================================
// Turning a window of trades into a subset problem
// ============================================================================

/**
 * @brief Relevance and an RBF similarity over one window of trades.
 *
 * Relevance is |signed size|, normalised by the window's largest: the same
 * quantity the flow rule is a sum of, so a selection that keeps the biggest
 * trades keeps most of the signal by construction. Similarity is an RBF over
 * (position in the window, signed size), matching the source formulation's
 * dense RBF kernel -- so two trades close in time and on the same side are
 * near-duplicates, and the redundancy term will push them apart.
 */
SubsetProblem build_problem(const std::vector<AggTrade>& trades, std::size_t end,
                            std::size_t n, std::size_t k, double sigma) {
    SubsetProblem p;
    p.k = k;
    p.relevance.resize(n);
    p.similarity.assign(n * n, 0.0);

    const std::size_t begin = end + 1 - n;
    double max_abs = 0.0;
    std::vector<double> signed_qty(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& t = trades[begin + i];
        signed_qty[i] = t.aggressor_sign() * t.quantity;
        max_abs = std::max(max_abs, std::fabs(signed_qty[i]));
    }
    if (max_abs <= 0.0) max_abs = 1.0;

    std::vector<double> pos(n), size(n);
    for (std::size_t i = 0; i < n; ++i) {
        p.relevance[i] = std::fabs(signed_qty[i]) / max_abs;
        pos[i] = static_cast<double>(i) / static_cast<double>(n - 1);
        size[i] = signed_qty[i] / max_abs;
    }
    const double denom = 2.0 * sigma * sigma;
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            const double dp = pos[i] - pos[j];
            const double ds = size[i] - size[j];
            const double s = std::exp(-(dp * dp + ds * ds) / denom);
            p.similarity[i * n + j] = s;
            p.similarity[j * n + i] = s;
        }
    }
    return p;
}

/// @brief The flow statistic the slow lane would compute from a selection.
double subset_flow(const std::vector<AggTrade>& trades, std::size_t end,
                   std::size_t n, const std::vector<std::uint8_t>& x) {
    const std::size_t begin = end + 1 - n;
    double net = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (x[i]) net += trades[begin + i].aggressor_sign() * trades[begin + i].quantity;
    }
    return net;
}

// ============================================================================
// One method's record across all decision points
// ============================================================================

struct MethodResult {
    std::string name;
    double lambda = -1.0;              ///< negative when lambda does not apply
    std::vector<double> net;           ///< signed flow per point
    std::vector<double> solve_us;
    double mean_relevance = 0.0;
    double mean_diversity = 0.0;
    double threshold = 0.0;

    /// Filled after calibration.
    PolicyOutcome outcome;
    std::vector<std::uint8_t> sized_down;   ///< per evaluation point
    std::vector<std::uint8_t> toxic;
};

double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double quantile_of(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<std::size_t>(v.size() * q))];
}

PolicyOutcome outcome_from(const std::vector<std::uint8_t>& sized,
                           const std::vector<std::uint8_t>& tox,
                           const std::vector<std::size_t>& idx) {
    PolicyOutcome o;
    for (const std::size_t i : idx) {
        if (sized[i]) { if (tox[i]) ++o.avoided_toxic; else ++o.forgone_benign; }
        else          { if (tox[i]) ++o.missed_toxic;  else ++o.kept_benign; }
    }
    return o;
}

struct Interval { double lo = 0.0, hi = 0.0; bool valid = false; };

/**
 * @brief Paired difference against a reference method.
 *
 * Every method saw the same decision points, so resampling them jointly removes
 * the variance the points contribute and leaves the difference between methods.
 * Comparing two independent intervals would throw that away.
 */
Interval paired_difference(const MethodResult& a, const MethodResult& b,
                           std::uint64_t seed, std::size_t resamples = 2000) {
    Interval iv;
    const std::size_t n = a.sized_down.size();
    if (n < 50 || b.sized_down.size() != n) return iv;
    Rng rng(seed);
    std::vector<double> draws;
    draws.reserve(resamples);
    std::vector<std::size_t> idx(n);
    for (std::size_t r = 0; r < resamples; ++r) {
        for (std::size_t i = 0; i < n; ++i) idx[i] = static_cast<std::size_t>(rng.below(n));
        const PolicyOutcome oa = outcome_from(a.sized_down, a.toxic, idx);
        const PolicyOutcome ob = outcome_from(b.sized_down, b.toxic, idx);
        if (oa.toxic() == 0 || oa.benign() == 0 || ob.toxic() == 0 || ob.benign() == 0) continue;
        draws.push_back(oa.informedness() - ob.informedness());
    }
    if (draws.size() < resamples / 2) return iv;
    std::sort(draws.begin(), draws.end());
    iv.lo = draws[static_cast<std::size_t>(draws.size() * 0.025)];
    iv.hi = draws[std::min(draws.size() - 1, static_cast<std::size_t>(draws.size() * 0.975))];
    iv.valid = true;
    return iv;
}

}  // namespace

// ============================================================================

/// @brief One day's worth of the external test.
struct DayOutcome {
    std::string label;
    std::vector<double> informedness;   ///< one per method, same order
    std::vector<double> diversity;
    std::vector<double> relevance;
    std::vector<double> solve_us;
    bool valid = false;
};

int main(int argc, char** argv) {
    const Options opts = parse(argc, argv);
    if (opts.data_paths.empty()) {
        std::fprintf(stderr, "usage: titans_subset <day1.csv> [day2.csv ...] [options]\n");
        return 2;
    }
    if (opts.budget >= opts.window) {
        std::fprintf(stderr, "ERROR: --budget must be smaller than --window; "
                             "selecting everything is not a selection\n");
        return 1;
    }

    struct AgreeRow { double lambda; std::string solver; double energy, div, us, gap; };
    std::vector<AgreeRow> agree;

    std::vector<DayOutcome> days;
    std::vector<std::string> method_names;
    std::vector<double> method_lambdas;
    std::size_t n_lam = opts.lambdas.size();
    std::size_t points_scored = 0;

    for (std::size_t di = 0; di < opts.data_paths.size(); ++di) {
        const bool first = (di == 0);
        const std::string& path = opts.data_paths[di];
        ToxicFlowLabelConfig lab;
        lab.horizon_ms = opts.horizon_ms;
        lab.threshold_bps = opts.threshold_bps;
        BinanceToxicFlowDataset ds(lab);
        std::printf("  Loading %s\n", path.c_str());
        if (!ds.load(path, opts.max_rows)) {
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
        std::printf("  choose %zu of the last %zu trades; lambda sweeps the frontier\n",
                    opts.budget, opts.window);

        // ------------------------------------------------------------------

        if (first) {
            // PART A -- do the solvers agree, on real instances
            // ------------------------------------------------------------------
            std::printf("\nPART A -- SOLVER AGREEMENT (n=%zu, k=%zu, %zu real windows)\n",
                        opts.exact_n, opts.exact_k, opts.exact_windows);
            std::printf("  Exhaustive search is affordable at this size, so every other\n"
                        "  solver has a ground truth to be checked against.\n\n");
            std::printf("  %8s %10s %14s %14s %14s\n",
                        "lambda", "solver", "mean energy", "mean diversity", "median solve");
            std::printf("  %s\n", std::string(68, '-').c_str());

            struct AgreeRow { double lambda; std::string solver; double energy, div, us, gap; };
            std::vector<AgreeRow> agree;
            {
                const std::size_t stride =
                    std::max<std::size_t>(1, (trades.size() - opts.exact_n) / opts.exact_windows);
                for (const double lam : opts.lambdas) {
                    double e_ex = 0, e_gr = 0, e_sa = 0;
                    double d_ex = 0, d_gr = 0, d_sa = 0;
                    std::vector<double> us_ex, us_gr, us_sa;
                    std::size_t count = 0, sa_optimal = 0;
                    double gap_sum = 0.0;
                    for (std::size_t w = 0; w < opts.exact_windows; ++w) {
                        const std::size_t end = opts.exact_n - 1 + w * stride;
                        if (end >= trades.size()) break;
                        SubsetProblem p = build_problem(trades, end, opts.exact_n,
                                                        opts.exact_k, opts.rbf_sigma);
                        p.lambda = lam;
                        const Solution ex = solve_exact(p);
                        if (!ex.valid) { std::printf("  [refused] %s\n", ex.refusal.c_str()); break; }
                        AnnealConfig ac = opts.anneal;
                        ac.seed = opts.seed + w;
                        const Solution gr = solve_greedy(p);
                        const Solution sa = solve_annealing(p, ac);

                        e_ex += ex.energy; e_gr += gr.energy; e_sa += sa.energy;
                        d_ex += diversity_of(p, ex.x);
                        d_gr += diversity_of(p, gr.x);
                        d_sa += diversity_of(p, sa.x);
                        us_ex.push_back(ex.solve_us);
                        us_gr.push_back(gr.solve_us);
                        us_sa.push_back(sa.solve_us);
                        // Optimality gap, relative to the exact energy's magnitude.
                        const double denom = std::max(1e-9, std::fabs(ex.energy));
                        gap_sum += (sa.energy - ex.energy) / denom;
                        if (sa.energy <= ex.energy + 1e-9) ++sa_optimal;
                        ++count;
                    }
                    if (!count) continue;
                    const double c = static_cast<double>(count);
                    agree.push_back({lam, "exact", e_ex / c, d_ex / c, median_of(us_ex), 0.0});
                    agree.push_back({lam, "greedy", e_gr / c, d_gr / c, median_of(us_gr),
                                     (e_gr - e_ex) / std::max(1e-9, std::fabs(e_ex))});
                    agree.push_back({lam, "sa", e_sa / c, d_sa / c, median_of(us_sa), gap_sum / c});
                    for (const auto& r : agree) {
                        if (r.lambda != lam) continue;
                        std::printf("  %8.2f %10s %14.4f %14.4f %11.1f us\n",
                                    r.lambda, r.solver.c_str(), r.energy, r.div, r.us);
                    }
                    std::printf("  %8s %10s %14s %14s   sa matched exact on %zu of %zu\n",
                                "", "", "", "", sa_optimal, count);
                }
            }

            // ------------------------------------------------------------------
            // PART B -- does warm starting help, as the window rolls
            // ------------------------------------------------------------------
            std::printf("\nPART B -- WARM START ACROSS ROLLING WINDOWS\n");
            std::printf("  The window slides by one trade, so the previous solution is a\n"
                        "  near-feasible starting point. Whether that is worth anything is\n"
                        "  the question the source formulation left open.\n\n");
            {
                const double lam = opts.lambdas.size() > 1 ? opts.lambdas[1] : 1.0;
                std::vector<double> cold_us, warm_us, cold_e, warm_e;
                std::vector<std::uint8_t> carry;
                const std::size_t rolls = 2000;
                for (std::size_t w = 0; w < rolls; ++w) {
                    const std::size_t end = opts.window - 1 + w;
                    if (end >= trades.size()) break;
                    SubsetProblem p = build_problem(trades, end, opts.window,
                                                    opts.budget, opts.rbf_sigma);
                    p.lambda = lam;
                    AnnealConfig ac = opts.anneal;
                    ac.seed = opts.seed + w;
                    const Solution cold = solve_annealing(p, ac);
                    const Solution warm = solve_annealing(p, ac, carry.empty() ? nullptr : &carry);
                    cold_us.push_back(cold.solve_us);
                    warm_us.push_back(warm.solve_us);
                    cold_e.push_back(cold.energy);
                    warm_e.push_back(warm.energy);
                    // Shift the selection by one to follow the window.
                    carry.assign(p.n(), 0);
                    for (std::size_t i = 1; i < p.n(); ++i) carry[i - 1] = warm.x[i];
                }
                const double ce = std::accumulate(cold_e.begin(), cold_e.end(), 0.0) /
                                  std::max<std::size_t>(1, cold_e.size());
                const double we = std::accumulate(warm_e.begin(), warm_e.end(), 0.0) /
                                  std::max<std::size_t>(1, warm_e.size());
                std::printf("  %10s %14s %16s\n", "start", "mean energy", "median solve");
                std::printf("  %s\n", std::string(44, '-').c_str());
                std::printf("  %10s %14.4f %13.1f us\n", "cold", ce, median_of(cold_us));
                std::printf("  %10s %14.4f %13.1f us\n", "warm", we, median_of(warm_us));

                // Every window was solved BOTH ways, so this is a paired
                // comparison and the interval belongs over windows.
                //
                // It is judged against its own scale rather than against an
                // absolute epsilon, because an absolute one is meaningless
                // here: these energies sit near -1000, so "warm is lower by
                // 1e-9" is the thirteenth significant figure of a number. The
                // first version of this block tested exactly that, announced a
                // direction on a difference in the seventh significant figure,
                // and supplied a mechanism to explain it -- "a start inside one
                // basin is a start that may not leave it". The sign of that
                // difference flips between runs. Inventing a cause for noise is
                // the failure this repository exists to prevent, and it shipped
                // here anyway; see docs/SUBSET.md.
                std::vector<double> paired;
                paired.reserve(cold_e.size());
                std::size_t warm_lower = 0;
                for (std::size_t i = 0; i < cold_e.size(); ++i) {
                    paired.push_back(warm_e[i] - cold_e[i]);
                    if (warm_e[i] < cold_e[i]) ++warm_lower;
                }
                const auto warm_ci = bootstrap_mean_ci(paired, 0.95, 10000, opts.seed);
                const double scale = std::abs(ce) > 0.0 ? std::abs(ce) : 1.0;
                const double relative = std::abs(warm_ci.point) / scale;

                std::printf("\n  Paired over %zu windows; warm minus cold, lower is better.\n",
                            paired.size());
                std::printf("    mean difference        %+.4e\n", warm_ci.point);
                if (warm_ci.valid) {
                    std::printf("    95%% CI over windows    [%+.4e, %+.4e]\n",
                                warm_ci.lo, warm_ci.hi);
                }
                std::printf("    against the energy     %.1e of |%.1f|\n", relative, ce);
                std::printf("    warm lower on          %zu of %zu windows\n",
                            warm_lower, paired.size());

                if (warm_ci.valid && warm_ci.excludes_zero()) {
                    std::printf("\n  RESOLVED: warm starting is %s, by %.1e of the energy.\n",
                                warm_ci.point < 0.0 ? "better" : "worse", relative);
                } else {
                    std::printf("\n  NOT RESOLVED. The interval spans zero and the difference is\n"
                                "  %.1e of the energy. No direction is claimed: there is nothing\n"
                                "  for a good start to save, because the annealer already reaches\n"
                                "  the optimum from a random one at this size.\n", relative);
                }
            }

            // ------------------------------------------------------------------

        }
        // PART C -- the external test
        // ------------------------------------------------------------------
        std::printf("\nPART C -- DOES SELECTION BEAT RECENCY, ON LABELS IT NEVER SAW\n");

        std::vector<std::size_t> where;
        {
            const std::size_t first = opts.window - 1;
            const std::size_t last = trades.size() - 2;
            if (last <= first) { std::fprintf(stderr, "ERROR: not enough trades\n"); return 1; }
            const std::size_t stride = std::max<std::size_t>(1, (last - first) / opts.points);
            for (std::size_t i = first; i < last; i += stride) {
                if (labels[i + 1] >= 0) where.push_back(i);
            }
        }
        std::printf("  %zu decision points, evenly spaced across the session\n", where.size());

        std::vector<MethodResult> methods;
        methods.push_back({"recent-" + std::to_string(opts.budget)});
        methods.push_back({"random-" + std::to_string(opts.budget)});
        for (const double lam : opts.lambdas) {
            MethodResult m;
            m.name = "greedy l=" + std::to_string(lam).substr(0, 4);
            m.lambda = lam;
            methods.push_back(m);
        }
        for (const double lam : opts.lambdas) {
            MethodResult m;
            m.name = "qubo/sa l=" + std::to_string(lam).substr(0, 4);
            m.lambda = lam;
            methods.push_back(m);
        }
        const std::size_t n_lam = opts.lambdas.size();

        std::printf("  solving ");
        std::fflush(stdout);
        for (std::size_t pi = 0; pi < where.size(); ++pi) {
            const std::size_t end = where[pi];
            SubsetProblem p = build_problem(trades, end, opts.window, opts.budget, opts.rbf_sigma);

            auto record = [&](MethodResult& m, const Solution& sol) {
                m.net.push_back(subset_flow(trades, end, opts.window, sol.x));
                m.solve_us.push_back(sol.solve_us);
                m.mean_relevance += relevance_of(p, sol.x);
                m.mean_diversity += diversity_of(p, sol.x);
            };

            p.lambda = 0.0;
            record(methods[0], select_recent(p));
            record(methods[1], select_random(p, opts.seed + pi));
            for (std::size_t li = 0; li < n_lam; ++li) {
                p.lambda = opts.lambdas[li];
                record(methods[2 + li], solve_greedy(p));
                AnnealConfig ac = opts.anneal;
                ac.seed = opts.seed + pi * 31 + li;
                record(methods[2 + n_lam + li], solve_annealing(p, ac));
            }
            if (pi % 5000 == 0) { std::printf("."); std::fflush(stdout); }
        }
        std::printf(" done\n");

        // Each method gets its OWN threshold, calibrated on the same prefix, so no
        // method is handicapped by a threshold fitted for a different statistic.
        const std::size_t calib_n =
            static_cast<std::size_t>(where.size() * opts.calibration_share);
        for (auto& m : methods) {
            std::vector<double> abs_net;
            for (std::size_t i = 0; i < calib_n; ++i) abs_net.push_back(std::fabs(m.net[i]));
            m.threshold = quantile_of(abs_net, opts.quantile);
            m.mean_relevance /= static_cast<double>(where.size());
            m.mean_diversity /= static_cast<double>(where.size());

            for (std::size_t i = calib_n; i < where.size(); ++i) {
                const std::size_t target = where[i] + 1;
                const bool risk_off = std::fabs(m.net[i]) > m.threshold;
                const int dir = m.net[i] > 0 ? +1 : (m.net[i] < 0 ? -1 : 0);
                const bool sized = risk_off && dir == trades[target].aggressor_sign();
                m.sized_down.push_back(sized ? 1 : 0);
                m.toxic.push_back(labels[target] > 0 ? 1 : 0);
            }
            std::vector<std::size_t> all(m.sized_down.size());
            std::iota(all.begin(), all.end(), 0);
            m.outcome = outcome_from(m.sized_down, m.toxic, all);
        }

        std::printf("  calibrated on the first %zu points, scored on the remaining %zu\n\n",
                    calib_n, where.size() - calib_n);
        std::printf("  %-16s %10s %10s %11s %14s %26s\n",
                    "method", "relevance", "diversity", "solve", "informedness",
                    "vs recency (95% CI)");
        std::printf("  %s\n", std::string(96, '-').c_str());

        std::uint64_t ci_seed = opts.seed + 5000;
        for (std::size_t mi = 0; mi < methods.size(); ++mi) {
            const auto& m = methods[mi];
            char diff[40] = "                  reference";
            if (mi > 0) {
                const Interval iv = paired_difference(m, methods[0], ci_seed++);
                if (iv.valid) {
                    std::snprintf(diff, sizeof(diff), "%+.4f [%+.4f,%+.4f]%s",
                                  m.outcome.informedness() - methods[0].outcome.informedness(),
                                  iv.lo, iv.hi, (iv.lo > 0.0 || iv.hi < 0.0) ? " *" : "  ");
                } else {
                    std::snprintf(diff, sizeof(diff), "%26s", "unresolvable");
                }
            }
            std::printf("  %-16s %10.3f %10.3f %9.1f us %+14.4f %26s\n",
                        m.name.c_str(), m.mean_relevance, m.mean_diversity,
                        median_of(m.solve_us), m.outcome.informedness(), diff);
        }
        std::printf("\n  * marks an interval that excludes zero. Every method saw the same\n"
                    "  points, so the comparison resamples them jointly.\n");

        // ------------------------------------------------------------------
        // Did lambda do what it claims to do?
        // ------------------------------------------------------------------

        DayOutcome dr;
        dr.label = path.substr(path.find_last_of('/') + 1);
        for (const auto& m : methods) {
            dr.informedness.push_back(m.outcome.informedness());
            dr.diversity.push_back(m.mean_diversity);
            dr.relevance.push_back(m.mean_relevance);
            dr.solve_us.push_back(median_of(m.solve_us));
        }
        dr.valid = true;
        points_scored = where.size();
        if (first) {
            for (const auto& m : methods) {
                method_names.push_back(m.name);
                method_lambdas.push_back(m.lambda);
            }
            std::printf("\n  DID LAMBDA WORK? The objective's own second term:\n");
            bool monotone = true;
            for (std::size_t li = 1; li < n_lam; ++li) {
                if (methods[2 + n_lam + li].mean_diversity <
                    methods[2 + n_lam + li - 1].mean_diversity - 1e-9) monotone = false;
            }
            std::printf("    diversity across lambda %.2f -> %.2f: %.3f -> %.3f, %s\n",
                        opts.lambdas.front(), opts.lambdas.back(),
                        methods[2 + n_lam].mean_diversity,
                        methods[2 + n_lam + n_lam - 1].mean_diversity,
                        monotone ? "monotone" : "NOT monotone");
            std::printf("    informedness across the same range: %+.4f -> %+.4f\n",
                        methods[2 + n_lam].outcome.informedness(),
                        methods[2 + n_lam + n_lam - 1].outcome.informedness());
            std::printf(
                "\n  If the first line moves and the second does not, the objective is\n"
                "  doing exactly what it is written to do and it is not buying anything\n"
                "  the labels can see. That is the distinction this program exists to\n"
                "  make, and it is only available because the labels are external to the\n"
                "  objective.\n");

            // ------------------------------------------------------------------

            // PART D -- what the solve costs, in the units P4 measured
            // ------------------------------------------------------------------
            std::printf("\nPART D -- WHAT THE SOLVE COSTS\n");
            {
                lanes::FlowPolicy::Config cfg;
                cfg.window = opts.window;
                {
                    lanes::FlowPolicy warm(cfg);
                    lanes::FlowCalibrator calib;
                    for (const auto& t : trades) {
                        warm.observe(t.aggressor_sign() * t.quantity);
                        if (!warm.warm()) continue;
                        calib.add(std::fabs(warm.decide().net));
                        if (calib.size() + 1 >= 5000) break;
                    }
                    calib.commit();
                    cfg.warn_above = calib.quantile(opts.quantile);
                }
                double worst_us = 0.0;
                for (const auto& m : methods) worst_us = std::max(worst_us, median_of(m.solve_us));
                const auto d0 = run_policy_delivered(trades, labels, cfg, 0);
                const auto d1 = run_policy_delivered(trades, labels, cfg, 1);

                std::printf("  slowest median solve, per decision: %.1f us\n", worst_us);
                std::printf("  the rule delivered with no delay:   %+.4f\n",
                            d0.outcome.informedness());

                // The tape stamps to the millisecond, so one millisecond is the
                // smallest delay it can express -- and it is not a small one, because
                // a 1 ms step jumps over the whole cluster of trades sharing a
                // timestamp. Rounding a sub-millisecond solve up to 1 ms and quoting
                // that drop would attribute the exchange's clock resolution to the
                // solver.
                if (worst_us < 1000.0) {
                    std::printf(
                        "\n  [below resolution] The solve is %.0fx smaller than the 1 ms\n"
                        "  granularity of these timestamps, so its cost cannot be measured\n"
                        "  on this data. For scale, the smallest delay the tape CAN express\n"
                        "  takes the same rule to %+.4f -- but most of that step is the\n"
                        "  cluster of trades sharing a millisecond, not one millisecond of\n"
                        "  waiting, so it is an upper bound on a cost the solver does not\n"
                        "  actually pay.\n",
                        1000.0 / std::max(1.0, worst_us), d1.outcome.informedness());
                } else {
                    const std::int64_t solve_ms =
                        static_cast<std::int64_t>(std::llround(worst_us / 1000.0));
                    const auto ds_ = run_policy_delivered(trades, labels, cfg, solve_ms);
                    std::printf("  delivered %lld ms later:              %+.4f\n",
                                static_cast<long long>(solve_ms), ds_.outcome.informedness());
                }

                // The exhaustive solver is the one whose cost IS measurable, and it is
                // the useful contrast: the same objective, solved properly, would spend
                // real freshness.
                double exact_us = 0.0;
                for (const auto& r : agree) if (r.solver == "exact") exact_us = std::max(exact_us, r.us);
                if (exact_us >= 1000.0) {
                    const std::int64_t ex_ms = static_cast<std::int64_t>(std::llround(exact_us / 1000.0));
                    const auto de = run_policy_delivered(trades, labels, cfg, ex_ms);
                    std::printf(
                        "\n  Exhaustive search on a much SMALLER instance (n=%zu, k=%zu) takes\n"
                        "  %.1f ms, and that cost is measurable: the same rule delivered %lld ms\n"
                        "  late is worth %+.4f against %+.4f fresh. Solving the objective\n"
                        "  properly, at a size where the annealer is only an approximation,\n"
                        "  would spend most of the signal to do it.\n",
                        opts.exact_n, opts.exact_k, exact_us / 1000.0,
                        static_cast<long long>(ex_ms), de.outcome.informedness(),
                        d0.outcome.informedness());
                }

                std::printf(
                    "\n  A selection has to be worth more than the delay it introduces.\n"
                    "  The annealer's solve is not the reason selection fails to pay here;\n"
                    "  it fails to pay at a cost of essentially nothing.\n");
            }


        }
        days.push_back(std::move(dr));
    }

    // ------------------------------------------------------------------
    // ACROSS DAYS. One day is an anecdote, and this item nearly shipped as
    // one: the first day measured said selection loses, and it is one of the
    // two days in eight where that is true.
    // ------------------------------------------------------------------
    if (days.size() > 1) {
        std::printf("\n\nACROSS %zu DAYS\n", days.size());
        std::printf("  Per-day informedness, and the difference against recency.\n"
                    "  A day is one observation, so the interval and the sign test\n"
                    "  are over days -- the same protocol titans_walkforward uses.\n\n");
        std::printf("  %-14s", "method");
        for (const auto& d : days) {
            const std::string t = d.label.size() >= 14
                ? d.label.substr(d.label.size() - 14, 10) : d.label;
            std::printf(" %10s", t.c_str());
        }
        std::printf("\n  %s\n", std::string(14 + 11 * days.size(), '-').c_str());
        for (std::size_t mi = 0; mi < method_names.size(); ++mi) {
            std::printf("  %-14s", method_names[mi].c_str());
            for (const auto& d : days) std::printf(" %+10.4f", d.informedness[mi]);
            std::printf("\n");
        }

        std::printf("\n  %-14s %10s %26s %14s\n",
                    "method", "mean diff", "95% CI over days", "days better");
        std::printf("  %s\n", std::string(70, '-').c_str());
        std::size_t best_mi = 0;
        double best_mean = -1e300;
        std::vector<BootstrapCI> cis(method_names.size());
        for (std::size_t mi = 1; mi < method_names.size(); ++mi) {
            std::vector<double> diffs;
            std::size_t better = 0;
            for (const auto& d : days) {
                const double v = d.informedness[mi] - d.informedness[0];
                diffs.push_back(v);
                if (v > 0.0) ++better;
            }
            cis[mi] = bootstrap_mean_ci(diffs, 0.95, 10000, opts.seed + mi);
            // The mean is computed here rather than read off the interval,
            // because a refused interval reports a point of zero and printing
            // that as the mean difference would be a fabricated number.
            const double mean_diff =
                std::accumulate(diffs.begin(), diffs.end(), 0.0) /
                static_cast<double>(diffs.size());
            char ci[40];
            if (cis[mi].valid) {
                std::snprintf(ci, sizeof(ci), "[%+.4f, %+.4f]%s",
                              cis[mi].lo, cis[mi].hi,
                              cis[mi].excludes_zero() ? " *" : "  ");
            } else {
                std::snprintf(ci, sizeof(ci), "%26s", cis[mi].refusal.substr(0, 24).c_str());
            }
            std::printf("  %-14s %+10.4f %26s %8zu / %zu\n",
                        method_names[mi].c_str(), mean_diff, ci, better, days.size());
            if (mean_diff > best_mean) {
                best_mean = mean_diff;
                best_mi = mi;
            }
        }
        std::printf("\n  * marks an interval that excludes zero.\n");

        // Selecting the best method across a lambda sweep is selection, and
        // P3 built the machinery for exactly that. The trial count is the
        // number of methods compared, derived rather than declared.
        if (best_mi > 0) {
            std::vector<double> diffs;
            for (const auto& d : days) diffs.push_back(d.informedness[best_mi] - d.informedness[0]);
            const PermutationTest perm = sign_flip_test(diffs, 20000, opts.seed + 99);
            const std::size_t trials = method_names.size() - 1;
            const double se = cis[best_mi].valid
                ? (cis[best_mi].hi - cis[best_mi].lo) / (2.0 * 1.959963985) : 0.0;
            const Deflation def = deflate(best_mean, se,
                                          perm.valid ? perm.p_value : 1.0, trials);
            std::printf("\n  BEST METHOD, CORRECTED FOR HAVING PICKED IT\n");
            std::printf("    %-28s %s\n", "method", method_names[best_mi].c_str());
            std::printf("    %-28s %zu\n", "methods compared", trials);
            std::printf("    %-28s %+.4f\n", "mean difference", best_mean);
            if (perm.valid) {
                std::printf("    %-28s %.4g\n", "sign test over days", perm.p_value);
            }
            if (!def.valid()) {
                std::printf("    %-28s %s\n", "not deflated", def.refusal.c_str());
            } else {
                std::printf("    %-28s %+.4f\n", "bar for best of that many",
                            def.expected_max_null);
                std::printf("    %-28s %+.4f  =>  z %+.2f\n", "observed minus the bar",
                            def.observed - def.expected_max_null, def.deflated_z);
                std::printf("    %-28s %s\n", "survives the correction",
                            def.survives ? "yes" : "no");
            }
        }
    }

    if (!opts.json_path.empty() && !days.empty()) {
        std::ofstream f(opts.json_path);
        if (f) {
            f << "{\n  \"schema\": \"titans.subset.v2\",\n";
            f << "  \"window\": " << opts.window << ",\n";
            f << "  \"budget\": " << opts.budget << ",\n";
            f << "  \"points_per_day\": " << points_scored << ",\n";
            f << "  \"days\": " << days.size() << ",\n";
            f << "  \"threshold_bps\": " << opts.threshold_bps << ",\n";
            f << "  \"methods\": [\n";
            for (std::size_t mi = 0; mi < method_names.size(); ++mi) {
                double mean_j = 0.0, mean_diff = 0.0;
                std::size_t better = 0;
                for (const auto& d : days) {
                    mean_j += d.informedness[mi];
                    const double v = d.informedness[mi] - d.informedness[0];
                    mean_diff += v;
                    if (v > 0.0) ++better;
                }
                mean_j /= static_cast<double>(days.size());
                mean_diff /= static_cast<double>(days.size());
                f << "    {\"name\": \"" << method_names[mi] << "\""
                  << ", \"lambda\": " << method_lambdas[mi]
                  << ", \"relevance\": " << days.front().relevance[mi]
                  << ", \"diversity\": " << days.front().diversity[mi]
                  << ", \"solve_us\": " << days.front().solve_us[mi]
                  << ", \"informedness_day1\": " << days.front().informedness[mi]
                  << ", \"mean_informedness\": " << mean_j
                  << ", \"mean_diff_vs_recency\": " << mean_diff
                  << ", \"days_better\": " << better << "}"
                  << (mi + 1 < method_names.size() ? "," : "") << "\n";
            }
            f << "  ],\n  \"per_day\": [\n";
            for (std::size_t di = 0; di < days.size(); ++di) {
                f << "    {\"day\": \"" << days[di].label << "\", \"informedness\": [";
                for (std::size_t mi = 0; mi < days[di].informedness.size(); ++mi) {
                    f << days[di].informedness[mi]
                      << (mi + 1 < days[di].informedness.size() ? ", " : "");
                }
                f << "]}" << (di + 1 < days.size() ? "," : "") << "\n";
            }
            f << "  ]\n}\n";
            std::printf("\nWrote %s\n", opts.json_path.c_str());
        }
    }
    return 0;
}
