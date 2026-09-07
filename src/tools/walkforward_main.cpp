/**
 * @file walkforward_main.cpp
 * @brief Out-of-sample evaluation of the slow-lane policy across many days.
 *
 * Usage:
 *   titans_walkforward data/raw/BTCUSDT-aggTrades-2024-01-*.csv [options]
 *
 * WHY THIS EXISTS
 * ---------------
 * Every market-data claim in this repository rested on BTCUSDT 2024-01-15, and
 * the policy threshold was calibrated on the first 5000 observations of the
 * same day it was then scored on. This runs the same policy over a series of
 * days, fitting the threshold on earlier days only, and reports what survives.
 *
 * It reports TWO arms per day, deliberately:
 *
 *   out-of-sample   threshold from days before this one   <- the honest number
 *   in-sample       threshold from this day itself        <- what single-day
 *                                                            tooling reports
 *
 * The gap between them is the optimism that a single-day evaluation cannot see.
 * Reporting only the first would hide how large that gap is; reporting only the
 * second is what this tool exists to stop.
 *
 * EXIT CODES
 *   0  ran and reported, whatever the verdict
 *   1  data error
 *   2  usage error
 *   3  protocol violation -- not a valid walk-forward, so no number is reported
 *   4  the policy is degenerate on every fold (constant), so informedness is
 *      zero by construction and there is nothing to interpret
 */

#include "titans/context/binance_dataset.hpp"
#include "titans/eval/bootstrap.hpp"
#include "titans/eval/deflated.hpp"
#include "titans/eval/metrics.hpp"
#include "titans/eval/walk_forward.hpp"
#include "titans/lanes/flow_policy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace titans;
using namespace titans::eval;
using namespace titans::context;

namespace {

struct Options {
    std::vector<std::string> paths;
    std::size_t window = 50;
    double quantile = 0.95;
    int64_t horizon_ms = 1000;
    double threshold_bps = 5.0;
    int64_t drift_window_ms = 0;
    std::size_t max_rows = 0;          ///< 0 = whole day. Truncation is unsound.
    bool contaminate = false;
    std::size_t resamples = 10000;
    std::uint64_t seed = 42;
    std::string json_path;
    /// Training samples within this many ms of a day's end are held back from
    /// the threshold used on the NEXT day, because they are adjacent in time to
    /// it. More than one value runs the whole protocol once per value.
    /// The FIRST value is the headline; the rest are sensitivity.
    std::vector<int64_t> embargo_ms{0};
    /// Configurations evaluated OUTSIDE this program that fed into choosing
    /// what it was asked to run. Added to the sweep this run performs.
    std::size_t declared_trials = 1;
};

/// AUC deviation past which a feature is treated as leaking the label.
/// Same limit as titans_dataset, on purpose: two tools disagreeing about what
/// counts as clean would be worse than either limit being slightly wrong.
constexpr double kLeakLimit = 0.10;

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--window" && i + 1 < argc)             o.window = std::atoll(argv[++i]);
        else if (a == "--quantile" && i + 1 < argc)      o.quantile = std::atof(argv[++i]);
        else if (a == "--horizon-ms" && i + 1 < argc)    o.horizon_ms = std::atoll(argv[++i]);
        else if (a == "--threshold-bps" && i + 1 < argc) o.threshold_bps = std::atof(argv[++i]);
        else if (a == "--drift-window-ms" && i + 1 < argc) o.drift_window_ms = std::atoll(argv[++i]);
        else if (a == "--max-rows" && i + 1 < argc)      o.max_rows = std::atoll(argv[++i]);
        else if (a == "--resamples" && i + 1 < argc)     o.resamples = std::atoll(argv[++i]);
        else if (a == "--seed" && i + 1 < argc)          o.seed = std::atoll(argv[++i]);
        else if (a == "--json" && i + 1 < argc)          o.json_path = argv[++i];
        else if (a == "--embargo-ms" && i + 1 < argc) {
            o.embargo_ms.clear();
            std::string list = argv[++i];
            std::size_t pos = 0;
            while (!list.empty()) {
                pos = list.find(',');
                const std::string tok = list.substr(0, pos);
                if (!tok.empty()) o.embargo_ms.push_back(std::atoll(tok.c_str()));
                if (pos == std::string::npos) break;
                list.erase(0, pos + 1);
            }
            if (o.embargo_ms.empty()) o.embargo_ms.push_back(0);
        }
        else if (a == "--trials" && i + 1 < argc)        o.declared_trials = std::atoll(argv[++i]);
        else if (a == "--contaminate")                   o.contaminate = true;
        else if (a == "--help" || a == "-h") {
            std::printf(
                "Usage: titans_walkforward <day1.csv> <day2.csv> ... [options]\n"
                "  --window N          trailing trades in the flow window (default 50)\n"
                "  --quantile Q        warn above this |net flow| quantile (default 0.95)\n"
                "  --horizon-ms N      toxic-flow horizon (default 1000)\n"
                "  --threshold-bps X   toxic-flow threshold (default 5)\n"
                "  --drift-window-ms N rolling drift estimate (default 0 = session mean)\n"
                "  --max-rows N        truncate each day (SMOKE ONLY -- see below)\n"
                "  --contaminate       corrupt the policy's view of order flow\n"
                "  --resamples N       bootstrap replicates (default 10000)\n"
                "  --seed N            deterministic seed (default 42)\n"
                "  --embargo-ms LIST   comma-separated embargo values; the first\n"
                "                      is the headline, the rest are sensitivity\n"
                "  --trials N          configurations already evaluated elsewhere\n"
                "  --json PATH         write the full result\n\n"
                "Days are sorted by their first trade timestamp, not by argument\n"
                "order, and every fold's train/test boundary is checked against\n"
                "timestamps before any number is reported.\n");
            std::exit(0);
        }
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            std::exit(2);
        }
        else o.paths.push_back(a);
    }
    return o;
}

struct LoadedDay {
    DaySummary summary;
    std::vector<AggTrade> trades;
    std::vector<int8_t> labels;
};

/**
 * @brief Signed order flow over the `window` trades STRICTLY BEFORE each trade.
 *
 * Trade `i` is excluded from its own feature. That is not pedantry: the policy
 * decides how to size before the fill, so a feature that includes the trade
 * being scored describes information the policy does not have, and the audit
 * would then certify a signal the policy cannot use. Entries before the window
 * is full are marked unusable rather than computed from a short sum.
 */
std::vector<double> trailing_flow(const std::vector<AggTrade>& trades,
                                  std::size_t window,
                                  std::vector<char>& usable) {
    std::vector<double> out(trades.size(), 0.0);
    usable.assign(trades.size(), 0);
    double net = 0.0;
    for (std::size_t i = 0; i < trades.size(); ++i) {
        out[i] = net;                       // window is [i-window, i-1]
        usable[i] = i >= window ? 1 : 0;
        net += trades[i].aggressor_sign() * trades[i].quantity;
        if (i >= window) {
            net -= trades[i - window].aggressor_sign() * trades[i - window].quantity;
        }
    }
    return out;
}

/**
 * @brief First trade timestamp of a file, without loading it.
 *
 * Days are ordered by evidence rather than by argument order, but loading a
 * month of trades just to sort them would hold ~2 GB for no reason and would
 * stop working on a longer series. One line per file is enough.
 */
bool peek_first_timestamp(const std::string& path, Timestamp& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (line.find("agg_trade_id") != std::string::npos) continue;  // header
        // Field 5 (0-based) is transact_time in milliseconds.
        std::size_t pos = 0;
        for (int field = 0; field < 5; ++field) {
            pos = line.find(',', pos);
            if (pos == std::string::npos) return false;
            ++pos;
        }
        try {
            out = static_cast<Timestamp>(std::stoull(line.substr(pos)));
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }
    return false;
}

bool load_day(const std::string& path, const Options& o, LoadedDay& out) {
    ToxicFlowLabelConfig lab;
    lab.horizon_ms = o.horizon_ms;
    lab.threshold_bps = o.threshold_bps;
    lab.drift_window_ms = o.drift_window_ms;

    BinanceToxicFlowDataset ds(lab);
    if (!ds.load(path, o.max_rows)) {
        std::fprintf(stderr, "ERROR: %s\n", ds.error().c_str());
        return false;
    }
    out.labels = ds.build_labels();
    out.trades = ds.trades();

    const auto& s = ds.stats();
    out.summary.path = path;
    out.summary.label = day_label_from_path(path);
    out.summary.trades = s.total_trades;
    out.summary.labelled = s.labelled;
    out.summary.toxic = s.toxic;
    out.summary.toxic_rate = s.toxic_rate;
    out.summary.drift_bps = s.drift_bps;
    out.summary.first_ms = s.first_time_ms;
    out.summary.last_ms = s.last_time_ms;

    // Per-day leakage audit. On one day this passed; whether it passes on all
    // of them is a different claim, and it is the one this tool can check.
    std::vector<char> usable;
    const auto flow = trailing_flow(out.trades, o.window, usable);
    std::vector<double> f_side, f_flow;
    std::vector<bool> lbl, lbl_flow;
    f_side.reserve(s.labelled);
    f_flow.reserve(s.labelled);
    lbl.reserve(s.labelled);
    lbl_flow.reserve(s.labelled);
    for (std::size_t i = 0; i < out.trades.size(); ++i) {
        if (out.labels[i] < 0) continue;
        f_side.push_back(out.trades[i].aggressor_sign());
        lbl.push_back(out.labels[i] > 0);
        if (!usable[i]) continue;
        // Signed by the aggressor, so the feature means "flow agreeing with
        // this trade's direction" rather than "flow in some absolute
        // direction"; an unsigned version cancels out across the two sides.
        f_flow.push_back(out.trades[i].aggressor_sign() * flow[i]);
        lbl_flow.push_back(out.labels[i] > 0);
    }
    out.summary.aggressor_auc = auc(f_side, lbl);
    out.summary.flow_auc = auc(f_flow, lbl_flow);
    return true;
}

/// One row of the embargo sweep, as the result file records it.
struct ArmRecord {
    int64_t embargo_ms = 0;
    std::size_t folds = 0;
    BootstrapCI ci;
};

void write_json(const Options& o,
                const std::vector<DaySummary>& days,
                const std::vector<Fold>& folds,
                const BootstrapCI& oos_ci,
                const BootstrapCI& gap_ci,
                const PermutationTest& perm,
                std::size_t trials,
                const std::vector<ArmRecord>& arms,
                const Deflation& deflation) {
    std::ofstream f(o.json_path);
    if (!f) {
        std::fprintf(stderr, "WARNING: cannot write %s\n", o.json_path.c_str());
        return;
    }
    f << "{\n  \"schema\": \"titans.walkforward.v1\",\n";
    f << "  \"protocol\": \"expanding window; threshold fitted on days strictly "
         "before the test day; predictor sees only trades that closed before "
         "the trade being scored\",\n";
    f << "  \"config\": {\"window\": " << o.window
      << ", \"quantile\": " << o.quantile
      << ", \"horizon_ms\": " << o.horizon_ms
      << ", \"threshold_bps\": " << o.threshold_bps
      << ", \"drift_window_ms\": " << o.drift_window_ms
      << ", \"contaminate\": " << (o.contaminate ? "true" : "false")
      << ", \"seed\": " << o.seed
      << ", \"max_rows_per_day\": " << o.max_rows << "},\n";

    // How many configurations produced this file. A result whose trial count
    // is not recorded cannot be corrected later by anyone reading it, and the
    // number nobody wrote down is always 1.
    f << "  \"trials\": {\"total\": " << trials
      << ", \"embargo_values\": " << arms.size()
      << ", \"declared_elsewhere\": " << o.declared_trials
      << ", \"headline_embargo_ms\": "
      << (arms.empty() ? 0 : arms.front().embargo_ms) << "},\n";

    f << "  \"embargo_sweep\": [\n";
    for (std::size_t i = 0; i < arms.size(); ++i) {
        const auto& a = arms[i];
        f << "    {\"embargo_ms\": " << a.embargo_ms
          << ", \"folds\": " << a.folds
          << ", \"valid\": " << (a.ci.valid ? "true" : "false")
          << ", \"informedness_oos\": " << a.ci.point
          << ", \"ci_lo\": " << a.ci.lo
          << ", \"ci_hi\": " << a.ci.hi << "}"
          << (i + 1 < arms.size() ? "," : "") << "\n";
    }
    f << "  ],\n";

    f << "  \"deflation\": {\"applied\": "
      << (trials > 1 && deflation.valid() ? "true" : "false")
      << ", \"trials\": " << deflation.trials
      << ", \"expected_max_null\": " << deflation.expected_max_null
      << ", \"deflated_z\": " << deflation.deflated_z
      << ", \"deflated_p\": " << deflation.deflated_p
      << ", \"family_wise_p\": " << deflation.adjusted_p
      << ", \"survives\": " << (deflation.survives ? "true" : "false")
      << ", \"refusal\": \"" << deflation.refusal << "\"},\n";

    f << "  \"days\": [\n";
    for (std::size_t i = 0; i < days.size(); ++i) {
        const auto& d = days[i];
        f << "    {\"date\": \"" << d.label << "\", \"trades\": " << d.trades
          << ", \"labelled\": " << d.labelled << ", \"toxic\": " << d.toxic
          << ", \"toxic_rate\": " << d.toxic_rate
          << ", \"drift_bps\": " << d.drift_bps
          << ", \"aggressor_auc\": " << d.aggressor_auc
          << ", \"flow_auc\": " << d.flow_auc << "}"
          << (i + 1 < days.size() ? "," : "") << "\n";
    }
    f << "  ],\n  \"folds\": [\n";
    for (std::size_t i = 0; i < folds.size(); ++i) {
        const auto& fd = folds[i];
        f << "    {\"test_date\": \"" << days[fd.test_day].label
          << "\", \"train_days\": " << fd.train_days
          << ", \"calibration_samples\": " << fd.calibration_samples
          << ", \"warn_above_oos\": " << fd.warn_above
          << ", \"warn_above_in_sample\": " << fd.in_sample_warn_above
          << ", \"informedness_oos\": " << fd.out_of_sample.informedness()
          << ", \"informedness_in_sample\": " << fd.in_sample.informedness()
          << ", \"tpr_oos\": " << fd.out_of_sample.tpr()
          << ", \"fpr_oos\": " << fd.out_of_sample.fpr()
          << ", \"action_rate_oos\": " << fd.out_of_sample.action_rate()
          << ", \"decisions\": " << fd.out_of_sample.decisions() << "}"
          << (i + 1 < folds.size() ? "," : "") << "\n";
    }
    f << "  ],\n";
    f << "  \"out_of_sample\": {\"mean\": " << oos_ci.point
      << ", \"ci_lo\": " << oos_ci.lo << ", \"ci_hi\": " << oos_ci.hi
      << ", \"level\": " << oos_ci.level
      << ", \"valid\": " << (oos_ci.valid ? "true" : "false")
      << ", \"excludes_zero\": " << (oos_ci.excludes_zero() ? "true" : "false")
      << "},\n";
    f << "  \"in_sample_minus_out_of_sample\": {\"mean\": " << gap_ci.point
      << ", \"ci_lo\": " << gap_ci.lo << ", \"ci_hi\": " << gap_ci.hi
      << ", \"valid\": " << (gap_ci.valid ? "true" : "false") << "},\n";
    f << "  \"sign_flip_test\": {\"p_value\": " << perm.p_value
      << ", \"exact\": " << (perm.exact ? "true" : "false")
      << ", \"assignments\": " << perm.assignments
      << ", \"n\": " << perm.n << "}\n}\n";
    std::printf("\nWrote %s\n", o.json_path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    const Options opts = parse(argc, argv);

    if (opts.paths.size() < 3) {
        std::fprintf(stderr,
            "usage: titans_walkforward <day1.csv> <day2.csv> ... [options]\n\n"
            "At least 3 days are required: the first is training-only, leaving\n"
            "2 folds, and nothing statistical can be said from fewer.\n\n"
            "Get a month with:\n"
            "  python python/data/fetch_binance.py --symbol BTCUSDT \\\n"
            "      --dates 2024-01-08:2024-02-04\n");
        return 2;
    }
    if (opts.max_rows) {
        std::printf(
            "WARNING: --max-rows %zu truncates every day.\n"
            "  The label's drift correction is a sample-mean statistic, so on a\n"
            "  prefix it removes the prefix's mean and leaves the local trend\n"
            "  standing. Measured on one day, the aggressor side goes from AUC\n"
            "  0.5012 over the full session to 0.6979 over the first 20k trades.\n"
            "  Use this for smoke tests, not for a result.\n\n",
            opts.max_rows);
    }

    std::printf("WALK-FORWARD: %zu days, expanding window\n", opts.paths.size());
    std::printf("  policy: trailing signed order flow, window %zu, warn above "
                "the q=%.2f quantile of |net|%s\n",
                opts.window, opts.quantile,
                opts.contaminate ? ", CONTAMINATED" : "");

    // ----------------------------------------------------------------------
    // Order the days by their first trade's timestamp. Argument order is a
    // claim; the timestamp is evidence. Only one line per file is read here --
    // the days themselves are loaded one at a time inside the fold loop, so
    // peak memory is one day rather than the whole series.
    // ----------------------------------------------------------------------
    std::vector<std::pair<Timestamp, std::string>> ordered;
    ordered.reserve(opts.paths.size());
    for (const auto& p : opts.paths) {
        Timestamp first = 0;
        if (!peek_first_timestamp(p, first)) {
            std::fprintf(stderr, "ERROR: cannot read a first timestamp from %s\n",
                         p.c_str());
            return 1;
        }
        ordered.emplace_back(first, p);
    }
    std::sort(ordered.begin(), ordered.end());
    for (std::size_t i = 1; i < ordered.size(); ++i) {
        if (ordered[i].first == ordered[i - 1].first) {
            std::fprintf(stderr,
                "ERROR: %s and %s start at the same millisecond; the same day "
                "listed twice would train on its own test data\n",
                ordered[i - 1].second.c_str(), ordered[i].second.c_str());
            return 3;
        }
    }

    std::vector<DaySummary> days(ordered.size());

    // ----------------------------------------------------------------------
    // Folds
    // ----------------------------------------------------------------------
    // One arm per embargo value. They share the expensive part -- loading a
    // day and running the policy over it once to collect the flow samples --
    // and differ only in which of those samples are allowed into the threshold
    // the NEXT day is judged with. Running the sweep as separate passes would
    // re-read 2.8 GB per value for no additional information.
    struct Arm {
        int64_t embargo_ms = 0;
        lanes::FlowCalibrator calib;
        std::vector<Fold> folds;
        /// Samples inside `embargo_ms` of the previous day's end. Held back
        /// from the threshold applied to the day immediately after them, and
        /// released once one more day has passed and they are no longer
        /// adjacent to anything being tested.
        std::vector<double> deferred;
    };
    std::vector<Arm> arms(opts.embargo_ms.size());
    for (std::size_t a = 0; a < arms.size(); ++a) arms[a].embargo_ms = opts.embargo_ms[a];

    std::vector<ProtocolViolation> violations;
    Timestamp train_end = 0;

    if (arms.size() > 1) {
        std::printf("\nEMBARGO SWEEP: %zu values, headline is the first (%lld ms)\n",
                    arms.size(), static_cast<long long>(arms[0].embargo_ms));
    }
    std::printf("\nFolds (threshold from earlier days only, embargo %lld ms)\n",
                static_cast<long long>(arms[0].embargo_ms));
    std::printf("  %-11s %5s %9s %8s %8s %8s %10s %10s %7s\n",
                "day", "train", "warn>", "toxic%", "sideAUC", "flowAUC",
                "OOS J", "in-samp J", "acted");
    std::fflush(stdout);

    for (std::size_t t = 0; t < ordered.size(); ++t) {
        LoadedDay day;
        if (!load_day(ordered[t].second, opts, day)) return 1;
        days[t] = day.summary;

        lanes::FlowPolicy::Config base_cfg;
        base_cfg.window = opts.window;
        base_cfg.contaminate = opts.contaminate;

        // The flow samples do not depend on the threshold, so one pass over the
        // day produces them for every arm. Only the DECISIONS differ.
        const auto warm = run_policy_over_day(day.trades, day.labels, base_cfg, true);

        // The counterfactual arm fits the threshold on this very day, so it is
        // untouched by an embargo on the training set and is computed once.
        PolicyOutcome in_sample{};
        double in_sample_warn = 0.0;
        if (t > 0) {
            lanes::FlowCalibrator self;
            for (double v : warm.abs_net) self.add(v);
            self.commit();
            in_sample_warn = self.quantile(opts.quantile);
            lanes::FlowPolicy::Config cfg = base_cfg;
            cfg.warn_above = in_sample_warn;
            in_sample = run_policy_over_day(day.trades, day.labels, cfg, false).outcome;
        }

        for (std::size_t a = 0; a < arms.size(); ++a) {
            Arm& arm = arms[a];
            if (t > 0) {
                Fold fd;
                fd.test_day = t;
                fd.train_days = t;
                fd.train_end_ms = train_end;
                fd.test_start_ms = day.summary.first_ms;
                fd.calibration_samples = arm.calib.size();
                fd.warn_above = arm.calib.quantile(opts.quantile);

                const std::string bad = check_fold(fd);
                if (!bad.empty()) {
                    if (a == 0) violations.push_back({t, days[t].label + ": " + bad});
                } else {
                    lanes::FlowPolicy::Config cfg = base_cfg;
                    cfg.warn_above = fd.warn_above;
                    fd.out_of_sample =
                        run_policy_over_day(day.trades, day.labels, cfg, false).outcome;
                    fd.in_sample_warn_above = in_sample_warn;
                    fd.in_sample = in_sample;

                    if (a == 0) {
                        std::printf("  %-11s %5zu %9.4f %7.2f%% %8.4f %8.4f %+10.4f "
                                    "%+10.4f %6.2f%%\n",
                                    days[t].label.c_str(), fd.train_days, fd.warn_above,
                                    days[t].toxic_rate * 100.0, days[t].aggressor_auc,
                                    days[t].flow_auc,
                                    fd.out_of_sample.informedness(),
                                    fd.in_sample.informedness(),
                                    fd.out_of_sample.action_rate() * 100.0);
                        std::fflush(stdout);
                    }
                    arm.folds.push_back(fd);
                }
            }

            // Yesterday's held-back tail is now one full day away from anything
            // being tested, so it rejoins the training set.
            for (double v : arm.deferred) arm.calib.add(v);
            arm.deferred.clear();

            // Split today's samples at the embargo boundary. See
            // eval/walk_forward.hpp for why the cut is on time and not on count.
            EmbargoSplit split = split_by_embargo(
                warm.abs_net, warm.abs_net_ms, day.summary.last_ms, arm.embargo_ms);
            for (double v : split.body) arm.calib.add(v);
            arm.deferred = std::move(split.tail);
            arm.calib.commit();
        }

        if (t == 0) {
            std::printf("  %-11s %5s %9s %7.2f%% %8.4f %8.4f %10s %10s %7s"
                        "   (training only)\n",
                        days[0].label.c_str(), "-", "-",
                        days[0].toxic_rate * 100.0, days[0].aggressor_auc,
                        days[0].flow_auc, "-", "-", "-");
            std::fflush(stdout);
        }

        train_end = std::max(train_end, day.summary.last_ms);
    }

    const std::vector<Fold>& folds = arms[0].folds;

    if (!violations.empty()) {
        std::printf("\nPROTOCOL VIOLATIONS -- no number is reported\n");
        for (const auto& v : violations) {
            std::printf("  fold %zu: %s\n", v.fold, v.what.c_str());
        }
        std::printf("\n  A walk-forward whose folds are not causal is not a\n"
                    "  walk-forward. Check that the day files do not overlap.\n");
        return 3;
    }
    if (folds.size() < 2) {
        std::fprintf(stderr, "\nERROR: %zu usable folds\n", folds.size());
        return 1;
    }

    // ----------------------------------------------------------------------
    // Label quality across days
    // ----------------------------------------------------------------------
    std::size_t leaky = 0;
    for (const auto& d : days) {
        if (std::abs(d.aggressor_auc - 0.5) > kLeakLimit) ++leaky;
    }
    std::printf("\nLABEL AUDIT ACROSS DAYS\n");
    {
        double worst = 0.0, best_flow = 0.5;
        std::string worst_day, worst_flow_day;
        double worst_flow = 1.0;
        for (const auto& d : days) {
            const double dev = std::abs(d.aggressor_auc - 0.5);
            if (dev > worst) { worst = dev; worst_day = d.label; }
            if (d.flow_auc > best_flow) best_flow = d.flow_auc;
            if (d.flow_auc < worst_flow) { worst_flow = d.flow_auc; worst_flow_day = d.label; }
        }
        std::printf("  aggressor side: worst deviation from chance %.4f on %s "
                    "(limit %.2f)\n", worst, worst_day.c_str(), kLeakLimit);
        std::printf("  trailing flow:  best %.4f, worst %.4f on %s\n",
                    best_flow, worst_flow, worst_flow_day.c_str());
        if (leaky) {
            std::printf("  [LEAK] %zu of %zu days exceed the limit. Those days' "
                        "folds are kept in the aggregate and named above rather "
                        "than dropped:\n"
                        "         removing days by a criterion computed from the "
                        "same data is how a result gets selected into existence.\n",
                        leaky, days.size());
        } else {
            std::printf("  [pass] no day's aggressor side clears the leakage "
                        "limit\n");
        }
    }

    // ----------------------------------------------------------------------
    // Aggregate
    // ----------------------------------------------------------------------
    std::vector<double> oos_j, ins_j, gap;
    std::size_t degenerate = 0;
    for (const auto& f : folds) {
        oos_j.push_back(f.out_of_sample.informedness());
        ins_j.push_back(f.in_sample.informedness());
        gap.push_back(f.in_sample.informedness() - f.out_of_sample.informedness());
        if (f.out_of_sample.degenerate()) ++degenerate;
    }

    // Sensitivity: the same aggregate over the days whose labels passed the
    // leakage audit. Reported BELOW the headline and never in place of it --
    // this is a subset chosen after seeing the data, and a number computed on
    // a post-hoc subset is a sensitivity check, not a result.
    std::vector<double> oos_clean;
    for (const auto& f : folds) {
        if (std::abs(days[f.test_day].aggressor_auc - 0.5) <= kLeakLimit) {
            oos_clean.push_back(f.out_of_sample.informedness());
        }
    }

    const auto oos_ci = bootstrap_mean_ci(oos_j, 0.95, opts.resamples, opts.seed);
    const auto ins_ci = bootstrap_mean_ci(ins_j, 0.95, opts.resamples, opts.seed + 1);
    const auto gap_ci = bootstrap_mean_ci(gap, 0.95, opts.resamples, opts.seed + 2);
    const auto perm = sign_flip_test(oos_j, 20000, opts.seed + 3);

    auto report_ci = [](const char* name, const BootstrapCI& c) {
        if (!c.valid) {
            std::printf("  %-26s NOT REPORTED -- %s\n", name, c.refusal.c_str());
            return;
        }
        std::printf("  %-26s %+.4f  95%% CI [%+.4f, %+.4f]%s\n", name, c.point,
                    c.lo, c.hi, c.underpowered ? "   (few folds; wide)" : "");
    };

    std::printf("\nINFORMEDNESS (TPR - FPR) ACROSS %zu FOLDS\n", folds.size());
    report_ci("out-of-sample", oos_ci);
    report_ci("in-sample (counterfactual)", ins_ci);
    report_ci("in-sample minus OOS", gap_ci);
    if (perm.valid) {
        const double floor_p = perm.exact
            ? 1.0 / static_cast<double>(perm.assignments)
            : 1.0 / static_cast<double>(1 + perm.assignments);
        if (perm.p_value <= floor_p) {
            // Printing 0.0000 would claim more evidence than this many sign
            // assignments can supply, whatever the true p is.
            std::printf("  %-26s p < %.2g -- the smallest value %llu %s "
                        "assignments can resolve\n",
                        "sign-flip test vs zero", floor_p,
                        static_cast<unsigned long long>(perm.assignments),
                        perm.exact ? "exhaustive" : "sampled");
        } else {
            std::printf("  %-26s p = %.4g (%s, %llu sign assignments)\n",
                        "sign-flip test vs zero", perm.p_value,
                        perm.exact ? "exact" : "sampled",
                        static_cast<unsigned long long>(perm.assignments));
        }
    } else {
        std::printf("  %-26s NOT REPORTED -- %s\n", "sign-flip test vs zero",
                    perm.refusal.c_str());
    }

    if (leaky && oos_clean.size() >= kMinSamplesForCI &&
        oos_clean.size() < folds.size()) {
        const auto clean_ci =
            bootstrap_mean_ci(oos_clean, 0.95, opts.resamples, opts.seed + 4);
        std::printf("\n  SENSITIVITY (not the headline): dropping the %zu folds "
                    "whose labels\n  failed the audit leaves %zu folds at "
                    "%+.4f, 95%% CI [%+.4f, %+.4f].\n"
                    "  That subset was chosen after seeing the data, so it "
                    "bounds how much\n  of the effect could be riding on the "
                    "leaky days -- it does not replace\n  the all-days number "
                    "above.\n",
                    folds.size() - oos_clean.size(), oos_clean.size(),
                    clean_ci.point, clean_ci.lo, clean_ci.hi);
    }

    if (degenerate == folds.size()) {
        std::printf("\nDEGENERATE\n"
                    "  The policy took the same action on every trade of every "
                    "fold, so\n  informedness is 0 by construction and no delta "
                    "above is interpretable.\n  Check --quantile: a threshold "
                    "at or beyond the observed range of\n  |net flow| never "
                    "fires.\n");
        return 4;
    }
    if (degenerate) {
        std::printf("\n  NOTE: %zu of %zu folds were degenerate (the policy never "
                    "changed its\n  action) and contribute a structural zero to "
                    "the mean above.\n", degenerate, folds.size());
    }

    // ----------------------------------------------------------------------
    // Embargo sensitivity, and the multiple-testing bill for having run it
    // ----------------------------------------------------------------------
    std::vector<ArmRecord> arm_summaries;
    for (const auto& arm : arms) {
        std::vector<double> j;
        for (const auto& f : arm.folds) j.push_back(f.out_of_sample.informedness());
        arm_summaries.push_back(ArmRecord{arm.embargo_ms, arm.folds.size(),
                                bootstrap_mean_ci(j, 0.95, opts.resamples, opts.seed)});
    }

    // Trials: every embargo value this run evaluated, times whatever the caller
    // declared was already tried elsewhere. Counting the sweep automatically is
    // the point -- a trial count a human has to remember to increment is a
    // trial count that stays at 1.
    const std::size_t trials = arms.size() * (opts.declared_trials == 0 ? 1 : opts.declared_trials);

    if (arms.size() > 1) {
        std::printf("\nEMBARGO SENSITIVITY\n");
        std::printf("  Training samples within the embargo of a day's end are held\n"
                    "  back from the threshold applied to the next day. The label\n"
                    "  looks %lld ms ahead, so anything below that is not an embargo.\n\n",
                    static_cast<long long>(opts.horizon_ms));
        std::printf("  %12s %7s %10s %22s %s\n",
                    "embargo", "folds", "OOS J", "95% CI", "");
        std::printf("  %s\n", std::string(66, '-').c_str());
        for (std::size_t a = 0; a < arm_summaries.size(); ++a) {
            const auto& as = arm_summaries[a];
            if (!as.ci.valid) {
                std::printf("  %10lld ms %7zu %10s %22s %s\n",
                            static_cast<long long>(as.embargo_ms), as.folds,
                            "-", "NOT REPORTED", as.ci.refusal.c_str());
                continue;
            }
            std::printf("  %10lld ms %7zu %+10.4f    [%+.4f, %+.4f] %s\n",
                        static_cast<long long>(as.embargo_ms), as.folds,
                        as.ci.point, as.ci.lo, as.ci.hi,
                        a == 0 ? "<-- headline" : "");
        }

        // The best point, named as such. Reporting it without this label is the
        // thing the correction below exists to make impossible.
        std::size_t best = arm_summaries.size();
        for (std::size_t a = 0; a < arm_summaries.size(); ++a) {
            if (!arm_summaries[a].ci.valid) continue;
            if (best == arm_summaries.size() ||
                arm_summaries[a].ci.point > arm_summaries[best].ci.point) best = a;
        }
        if (best == arm_summaries.size() || !arm_summaries[0].ci.valid) {
            std::printf("\n  No point in the sweep produced a reportable interval, so\n"
                        "  there is no best point to name and nothing to deflate.\n");
        } else {
            std::printf("\n  The headline is the FIRST value, not the best one. The best\n"
                        "  is %lld ms at %+.4f; it is %+.4f above the headline, and the\n"
                        "  deflation below is what it would have to survive to be\n"
                        "  reported as the result.\n",
                        static_cast<long long>(arm_summaries[best].embargo_ms),
                        arm_summaries[best].ci.point,
                        arm_summaries[best].ci.point - arm_summaries[0].ci.point);
        }
    }

    Deflation deflation;
    std::printf("\nMULTIPLE TESTING\n");
    std::printf("  %-30s %zu  (%zu embargo value%s x %zu declared elsewhere)\n",
                "configurations evaluated", trials, arms.size(),
                arms.size() == 1 ? "" : "s", opts.declared_trials);
    if (trials <= 1) {
        std::printf("  No correction applied, and none is needed only because\n"
                    "  nothing was selected: one configuration was run and its\n"
                    "  number reported. Pass --trials N if this run's parameters\n"
                    "  were themselves chosen by looking at earlier runs.\n");
    } else if (!oos_ci.valid) {
        std::printf("  NOT DEFLATED. The interval a correction would widen was\n"
                    "  itself refused: %s\n"
                    "  A missing correction on a missing number is not a pass.\n",
                    oos_ci.refusal.c_str());
    } else {
        // The bootstrap interval's half-width over 1.96 is the standard error
        // the deflation needs; taking it from the interval rather than from a
        // normal assumption keeps it consistent with what is printed above.
        const double se = (oos_ci.hi - oos_ci.lo) / (2.0 * 1.959963985);
        const double raw_p = perm.valid ? perm.p_value : 1.0;
        deflation = deflate(oos_ci.point, se, raw_p, trials);
        const Deflation& def = deflation;
        const double lvl = sidak_level(0.95, trials);
        const auto wide = bootstrap_mean_ci(oos_j, lvl, opts.resamples, opts.seed + 7);

        if (!def.valid()) {
            std::printf("  NOT DEFLATED -- %s\n", def.refusal.c_str());
        } else {
            std::printf("  %-30s %.4f  =>  %.2f%% interval\n",
                        "Sidak per-interval level", lvl, 100.0 * lvl);
            if (wide.valid) {
                std::printf("  %-30s [%+.4f, %+.4f]   (raw 95%%: [%+.4f, %+.4f])\n",
                            "deflated interval", wide.lo, wide.hi, oos_ci.lo, oos_ci.hi);
            }
            std::printf("  %-30s %+.4f  (best of %zu under the null)\n",
                        "bar the best point must clear", def.expected_max_null, trials);
            if (def.deflated_p > 0.0) {
                std::printf("  %-30s %+.4f  =>  z %+.2f, p %.3g\n",
                            "observed minus that bar",
                            def.observed - def.expected_max_null, def.deflated_z,
                            def.deflated_p);
            } else {
                // Below the smallest double the tail can represent. Printing 0
                // would claim infinite evidence; the sign-flip test above
                // prints a floor for the same reason.
                std::printf("  %-30s %+.4f  =>  z %+.2f, p < 1e-300\n",
                            "observed minus that bar",
                            def.observed - def.expected_max_null, def.deflated_z);
            }
            std::printf("  %-30s %.3g  (raw %.3g)\n",
                        "sign-flip p, family-wise", def.adjusted_p, raw_p);
            std::printf("\n  %s\n", def.survives
                ? "The result survives being the best of this many trials."
                : "The result does NOT survive the correction for this many trials.");
        }
    }

    // ----------------------------------------------------------------------
    // Verdict
    // ----------------------------------------------------------------------
    std::printf("\nVERDICT\n");
    if (!oos_ci.valid) {
        std::printf("  NO INTERVAL. %s\n", oos_ci.refusal.c_str());
    } else if (oos_ci.excludes_zero()) {
        std::printf("  RESOLVED. The out-of-sample 95%% interval [%+.4f, %+.4f]\n"
                    "  excludes zero across %zu days the threshold never saw.\n",
                    oos_ci.lo, oos_ci.hi, folds.size());
    } else {
        std::printf("  NOT RESOLVED. The out-of-sample 95%% interval\n"
                    "  [%+.4f, %+.4f] contains zero, so across %zu unseen days\n"
                    "  this policy is not distinguishable from quoting the same\n"
                    "  size at every trade.\n", oos_ci.lo, oos_ci.hi, folds.size());
    }
    if (gap_ci.valid) {
        std::printf("\n  Fitting the threshold on the test day itself moves\n"
                    "  informedness by %+.4f on average (95%% CI [%+.4f, %+.4f]).\n"
                    "  That is the margin a single-day evaluation cannot see, and\n"
                    "  it is why the number in the README came from one day.\n",
                    gap_ci.point, gap_ci.lo, gap_ci.hi);
    }

    if (!opts.json_path.empty()) {
        write_json(opts, days, folds, oos_ci, gap_ci, perm,
                   trials, arm_summaries, deflation);
    }
    return 0;
}
