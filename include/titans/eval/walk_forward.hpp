/**
 * @file walk_forward.hpp
 * @brief Expanding-window out-of-sample evaluation over a series of days.
 *
 * WHAT THIS FIXES
 * ---------------
 * Every market-data number in this repository came from one day, BTCUSDT
 * 2024-01-15, and the slow lane's warn threshold was calibrated from the first
 * 5000 observations of that same day. Both are defensible on their own terms --
 * the prefix is causal, and one day is a real day -- and together they still
 * cannot answer the only question that matters for a policy: does it work on a
 * day it has never seen.
 *
 * A walk-forward answers it. For each day t in a series:
 *
 *     fit on days [0, t)   ->   test on day t
 *
 * The threshold that decides day t is computed from earlier days only. Day t
 * contributes nothing to its own decision rule, and the guard below refuses the
 * run if that is ever violated -- if the files arrive out of order, or if two
 * days overlap in time.
 *
 * WHY THE POLICY IS EVALUATED WITHOUT THREADS
 * -------------------------------------------
 * `titans_lanes` scores the same policy under a real-time two-thread replay,
 * and its verdict is NOT RESOLVED: the run-to-run spread swamps the effect,
 * because whether an advisory happens to be fresh when a toxic trade arrives
 * depends on when the slow lane last woke. That is an honest measurement of the
 * ARCHITECTURE and a useless instrument for the POLICY.
 *
 * So this evaluates the policy deterministically: same rule, same window, no
 * threads, no advisory expiry. Two consequences worth being explicit about.
 * It removes scheduling noise, which is the point. It also removes staleness,
 * so the numbers here are an UPPER BOUND on what the live lane can achieve --
 * the live lane additionally has to get the advice there in time. A policy that
 * shows nothing here cannot show anything there.
 *
 * WHAT IS STILL ALLOWED TO SEE THE FUTURE
 * ---------------------------------------
 * The label. A trade is toxic if the price moved against the maker within the
 * horizon, and the drift correction subtracts the day's own mean forward
 * return. Both use the whole day. That is legitimate and is not what leakage
 * means here: the constraint is that the PREDICTOR must not see the future, not
 * that the TARGET must avoid it. The predictor here sees a trailing window of
 * trades that all closed before the trade being scored.
 */

#pragma once

#include "../context/binance_dataset.hpp"
#include "../lanes/flow_policy.hpp"
#include "metrics.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace titans {
namespace eval {

/// @brief One day of data, after loading and labelling.
struct DaySummary {
    std::string path;
    std::string label;              ///< "2024-01-15", parsed from the filename.
    std::size_t trades = 0;
    std::size_t labelled = 0;
    std::size_t toxic = 0;
    double toxic_rate = 0.0;
    double drift_bps = 0.0;
    Timestamp first_ms = 0;
    Timestamp last_ms = 0;
    /// Leakage check, per day. The aggressor side must stay near chance.
    double aggressor_auc = 0.5;
    /// Learnability check, per day. Trailing signed flow must beat chance.
    double flow_auc = 0.5;
};

/// @brief One expanding-window fold: train on [0, test_day), test on test_day.
struct Fold {
    std::size_t test_day = 0;
    std::size_t train_days = 0;
    Timestamp train_end_ms = 0;
    Timestamp test_start_ms = 0;
    std::size_t calibration_samples = 0;

    /// Threshold from earlier days only. This is the honest arm.
    double warn_above = 0.0;
    PolicyOutcome out_of_sample;

    /// Threshold fitted on the test day itself, kept as the counterfactual:
    /// it is what the single-day tooling reports, so the gap between the two
    /// is the size of the optimism that walk-forward removes.
    double in_sample_warn_above = 0.0;
    PolicyOutcome in_sample;

    /// @brief Every training trade closed before the first test trade.
    bool causal() const { return train_end_ms < test_start_ms; }
};

/// @brief A reason the run is not a valid walk-forward. Any of these is fatal.
struct ProtocolViolation {
    std::size_t fold = 0;
    std::string what;
};

/**
 * @brief Run the policy over one day, strictly causally.
 *
 * The decision for trade `i` is taken from the window of trades that closed
 * before it; trade `i` itself enters the window only after it has been scored.
 * That mirrors the live lane, where the advisory the fast path reads was
 * published from strictly earlier observations, and it is what makes the number
 * a prediction rather than a description.
 *
 * @param collect_abs_net  When set, records |net| at each decision point. Those
 *                         are the samples a later fold's threshold is drawn
 *                         from, and they are sampled at exactly the points the
 *                         decision uses -- calibrating on a different quantity
 *                         than the one being thresholded is a real way to get a
 *                         threshold that means nothing.
 */
struct DayRun {
    PolicyOutcome outcome;
    std::vector<double> abs_net;
    /// Trade timestamp for each `abs_net` entry, so a caller can apply an
    /// embargo without having to re-derive which trade a sample came from.
    /// The mapping is not the identity: samples only begin once the flow
    /// window is warm, and a caller reconstructing the offset by hand would
    /// be off by `window - 1` and never know.
    std::vector<Timestamp> abs_net_ms;
};

/**
 * @brief Split a day's flow samples at an embargo boundary.
 *
 * Samples within @p embargo_ms of @p day_end_ms are adjacent in time to the
 * day that will be TESTED next, so they are held back from the threshold that
 * day is judged with. Everything else joins the training set immediately.
 *
 * The cut is on the sample's own timestamp and not on a count of samples,
 * because trade density varies by two orders of magnitude across a session:
 * "the last 10000 trades" is a different embargo on a quiet morning and a
 * volatile afternoon, and a parameter whose meaning moves with the data cannot
 * be swept.
 *
 * An embargo of 0 puts everything in `body`, which is what makes the sweep's
 * first point exactly the protocol that was published without one.
 */
struct EmbargoSplit {
    std::vector<double> body;   ///< usable for the next day's threshold
    std::vector<double> tail;   ///< held back one more day
};

inline EmbargoSplit split_by_embargo(const std::vector<double>& abs_net,
                                     const std::vector<Timestamp>& abs_net_ms,
                                     Timestamp day_end_ms,
                                     Timestamp embargo_ms) {
    EmbargoSplit out;
    const std::size_t n = std::min(abs_net.size(), abs_net_ms.size());
    out.body.reserve(n);
    if (embargo_ms <= 0) {
        out.body.assign(abs_net.begin(), abs_net.begin() + static_cast<std::ptrdiff_t>(n));
        return out;
    }
    const Timestamp cut = day_end_ms - embargo_ms;
    for (std::size_t i = 0; i < n; ++i) {
        if (abs_net_ms[i] > cut) out.tail.push_back(abs_net[i]);
        else                     out.body.push_back(abs_net[i]);
    }
    return out;
}

inline DayRun run_policy_over_day(const std::vector<context::AggTrade>& trades,
                                  const std::vector<int8_t>& labels,
                                  lanes::FlowPolicy::Config cfg,
                                  bool collect_abs_net) {
    DayRun run;
    lanes::FlowPolicy policy(cfg);

    lanes::FlowPolicy::Decision pending;
    bool have_pending = false;

    for (std::size_t i = 0; i < trades.size(); ++i) {
        const auto& t = trades[i];

        // Score the decision that was standing BEFORE this trade existed.
        if (have_pending && labels[i] >= 0) {
            const bool sized_down =
                pending.risk_off &&
                pending.direction == static_cast<std::int8_t>(t.aggressor_sign());
            const bool toxic = labels[i] > 0;
            if (sized_down) {
                if (toxic) ++run.outcome.avoided_toxic;
                else       ++run.outcome.forgone_benign;
            } else {
                if (toxic) ++run.outcome.missed_toxic;
                else       ++run.outcome.kept_benign;
            }
        }

        policy.observe(t.aggressor_sign() * t.quantity);
        if (policy.warm()) {
            pending = policy.decide();
            have_pending = true;
            if (collect_abs_net) {
                run.abs_net.push_back(std::abs(pending.net));
                run.abs_net_ms.push_back(t.transact_time_ms);
            }
        }
    }
    return run;
}

/**
 * @brief Check one fold's train/test boundary.
 *
 * Returns an empty string when the fold is sound. The check is on timestamps
 * rather than on filenames, because a filename is a claim and a timestamp is
 * evidence -- a mis-sorted argument list or a re-published archive would
 * otherwise silently turn an expanding window into one that trains on its own
 * test day.
 */
inline std::string check_fold(const Fold& f) {
    if (f.train_days == 0) {
        return "no training days";
    }
    if (!f.causal()) {
        return "training data ends at " + std::to_string(f.train_end_ms) +
               " ms but the test day starts at " +
               std::to_string(f.test_start_ms) +
               " ms; the threshold would be fitted on the day it is scored on";
    }
    if (f.calibration_samples == 0) {
        return "no calibration samples from the training days";
    }
    return "";
}

/// @brief Pull "2024-01-15" out of ".../BTCUSDT-aggTrades-2024-01-15.csv".
inline std::string day_label_from_path(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    // Last 10 characters, when they look like a date.
    if (name.size() >= 10) {
        const std::string tail = name.substr(name.size() - 10);
        if (tail[4] == '-' && tail[7] == '-') return tail;
    }
    return name;
}

}  // namespace eval
}  // namespace titans
