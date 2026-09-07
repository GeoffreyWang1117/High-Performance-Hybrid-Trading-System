/**
 * @file test_walk_forward.cpp
 * @brief The walk-forward protocol, and the inference it reports.
 *
 * A walk-forward is only worth running if it cannot cheat, so the properties
 * asserted here are the ones whose failure would be invisible in the output:
 *
 *   - The evaluator must decide on trade i from trades that closed BEFORE it.
 *     A version that observes trade i first still runs, still prints a table,
 *     and reports a much better number. That defect is checked with a fixture
 *     where the two versions give different answers by construction.
 *   - A fold whose training data overlaps its test day must be refused, not
 *     scored.
 *   - The interval must refuse small samples rather than emit a narrow one,
 *     and must actually cover at the rate it claims.
 *   - The permutation p must never be zero.
 *   - The rolling drift estimator must track a trend the session mean misses.
 *     This is the defect a 28-day walk-forward found in the labels, so it is
 *     pinned here rather than left to a tool run.
 */

#include "titans/context/binance_dataset.hpp"
#include "titans/eval/bootstrap.hpp"
#include "titans/eval/walk_forward.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace titans;
using namespace titans::eval;
using namespace titans::context;

namespace {

AggTrade trade(uint64_t id, double price, double qty, Timestamp t, bool buyer_maker) {
    AggTrade x;
    x.agg_trade_id = id;
    x.price = price;
    x.quantity = qty;
    x.transact_time_ms = t;
    x.is_buyer_maker = buyer_maker;
    return x;
}

// --------------------------------------------------------------------------

/**
 * @brief The decision scoring trade i must not have seen trade i.
 *
 * Fixture: 59 aggressive buys of 1.0, then one aggressive sell of 1000. With a
 * 50-trade window and a threshold of 40, the standing decision is always
 * "risk off, buy pressure", and it is scored against each next trade.
 *
 *   causal   the decision facing the final SELL was built from 50 buys, so its
 *            direction (+1) does not match the sell and it does not fire:
 *            9 sized down, 1 not.
 *   peeking  observing the -1000 sell first flips the window negative, the
 *            direction matches the sell, and it fires: 10 sized down, 0 not.
 *
 * One trade of difference, and it is the difference between a prediction and a
 * description.
 */
bool test_decision_precedes_its_trade() {
    std::vector<AggTrade> trades;
    for (uint64_t i = 0; i < 59; ++i) {
        trades.push_back(trade(i, 41000.0, 1.0, 1000 + i, /*buyer_maker=*/false));
    }
    trades.push_back(trade(59, 41000.0, 1000.0, 1059, /*buyer_maker=*/true));

    std::vector<int8_t> labels(trades.size(), 0);   // all benign

    lanes::FlowPolicy::Config cfg;
    cfg.window = 50;
    cfg.warn_above = 40.0;

    const auto run = run_policy_over_day(trades, labels, cfg, false);
    const auto& o = run.outcome;

    std::printf("      sized down %llu, full size %llu (causal expects 9 and 1)\n",
                static_cast<unsigned long long>(o.forgone_benign),
                static_cast<unsigned long long>(o.kept_benign));

    if (o.forgone_benign != 9 || o.kept_benign != 1) {
        std::fprintf(stderr,
            "FAIL: expected 9 sized down and 1 full size. Got %llu and %llu. "
            "10 and 0 means the evaluator observed the trade it was scoring.\n",
            static_cast<unsigned long long>(o.forgone_benign),
            static_cast<unsigned long long>(o.kept_benign));
        return false;
    }
    return true;
}

/// @brief A fold whose training data runs past the test day's first trade.
bool test_fold_guard_refuses_overlap() {
    Fold ok;
    ok.test_day = 1;
    ok.train_days = 1;
    ok.train_end_ms = 1000;
    ok.test_start_ms = 1001;
    ok.calibration_samples = 100;
    if (!check_fold(ok).empty()) {
        std::fprintf(stderr, "FAIL: a causal fold was refused: %s\n",
                     check_fold(ok).c_str());
        return false;
    }

    struct Case { const char* name; Fold f; };
    Fold overlap = ok;   overlap.train_end_ms = 1001;   // ends exactly at start
    Fold inverted = ok;  inverted.train_end_ms = 5000;  // ends after
    Fold notrain = ok;   notrain.train_days = 0;
    Fold nocalib = ok;   nocalib.calibration_samples = 0;

    const Case cases[] = {
        {"training ends exactly at the test day's first trade", overlap},
        {"training ends after the test day starts",             inverted},
        {"no training days",                                    notrain},
        {"no calibration samples",                              nocalib},
    };
    for (const auto& c : cases) {
        if (check_fold(c.f).empty()) {
            std::fprintf(stderr, "FAIL: guard accepted a fold where %s\n", c.name);
            return false;
        }
    }
    std::printf("      4 unsound fold shapes refused, 1 sound fold accepted\n");
    return true;
}

// --------------------------------------------------------------------------

bool test_bootstrap_refuses_small_samples() {
    for (std::size_t n = 0; n < kMinSamplesForCI; ++n) {
        std::vector<double> x(n, 0.5);
        const auto ci = bootstrap_mean_ci(x);
        if (ci.valid) {
            std::fprintf(stderr, "FAIL: reported an interval from %zu values\n", n);
            return false;
        }
        if (ci.refusal.empty()) {
            std::fprintf(stderr, "FAIL: refused without saying why at n=%zu\n", n);
            return false;
        }
    }
    const std::vector<double> enough(kMinSamplesForCI, 0.5);
    if (!bootstrap_mean_ci(enough).valid) {
        std::fprintf(stderr, "FAIL: refused at the documented minimum n=%zu\n",
                     kMinSamplesForCI);
        return false;
    }
    std::printf("      refuses below n=%zu, reports at n=%zu\n",
                kMinSamplesForCI, kMinSamplesForCI);
    return true;
}

/**
 * @brief The interval must cover the true mean about as often as it claims.
 *
 * An off-by-one in the percentile index, or resampling without replacement,
 * both produce intervals that look reasonable and cover at the wrong rate.
 * The only way to see that is to count.
 *
 * The percentile bootstrap is known to under-cover at small n, so the band
 * here is deliberately loose on the low side; what it rules out is an
 * implementation that covers at 60% or at 100%.
 */
bool test_bootstrap_coverage() {
    constexpr int kTrials = 300;
    constexpr std::size_t kN = 20;
    const double true_mean = 0.5;         // Uniform[0,1]

    Rng gen(20240115);
    int covered = 0;
    for (int t = 0; t < kTrials; ++t) {
        std::vector<double> sample;
        sample.reserve(kN);
        for (std::size_t i = 0; i < kN; ++i) {
            sample.push_back(static_cast<double>(gen.next() >> 11) /
                             static_cast<double>(1ULL << 53));
        }
        const auto ci = bootstrap_mean_ci(sample, 0.95, 2000,
                                          static_cast<std::uint64_t>(t) + 1);
        if (!ci.valid) {
            std::fprintf(stderr, "FAIL: refused a sample of %zu\n", kN);
            return false;
        }
        if (ci.lo <= true_mean && true_mean <= ci.hi) ++covered;
    }
    const double rate = static_cast<double>(covered) / kTrials;
    std::printf("      95%% interval covered the true mean %.1f%% of %d trials "
                "at n=%zu\n", rate * 100.0, kTrials, kN);
    if (rate < 0.85 || rate > 0.99) {
        std::fprintf(stderr,
            "FAIL: coverage %.3f is outside [0.85, 0.99] for a nominal 0.95 "
            "interval\n", rate);
        return false;
    }
    return true;
}

bool test_bootstrap_is_reproducible() {
    const std::vector<double> x{0.1, -0.4, 0.7, 0.2, 0.0, 0.55, -0.1, 0.33};
    const auto a = bootstrap_mean_ci(x, 0.95, 4000, 7);
    const auto b = bootstrap_mean_ci(x, 0.95, 4000, 7);
    if (a.lo != b.lo || a.hi != b.hi) {
        std::fprintf(stderr, "FAIL: same seed gave [%g, %g] then [%g, %g]\n",
                     a.lo, a.hi, b.lo, b.hi);
        return false;
    }
    const auto c = bootstrap_mean_ci(x, 0.95, 4000, 8);
    if (a.lo == c.lo && a.hi == c.hi) {
        std::fprintf(stderr, "FAIL: two seeds gave an identical interval; the "
                             "seed is not reaching the resampler\n");
        return false;
    }
    if (std::abs(a.point - b.point) > 0.0 || std::abs(a.point - c.point) > 0.0) {
        std::fprintf(stderr, "FAIL: the point estimate must not depend on the "
                             "seed\n");
        return false;
    }
    std::printf("      identical under one seed, different under another, "
                "point estimate fixed\n");
    return true;
}

// --------------------------------------------------------------------------

/**
 * @brief The exact branch must match a p-value that can be computed by hand.
 *
 * With n identical positive values the only assignments as extreme as the
 * observed one are all-plus and all-minus, so p = 2 / 2^n exactly.
 */
bool test_sign_flip_exact_values() {
    struct Case { std::size_t n; double expected; };
    const Case cases[] = {{3, 2.0 / 8}, {5, 2.0 / 32}, {8, 2.0 / 256},
                          {12, 2.0 / 4096}};
    for (const auto& c : cases) {
        const std::vector<double> x(c.n, 1.0);
        const auto r = sign_flip_test(x);
        if (!r.exact) {
            std::fprintf(stderr, "FAIL: n=%zu should be enumerated exactly\n", c.n);
            return false;
        }
        if (std::abs(r.p_value - c.expected) > 1e-12) {
            std::fprintf(stderr, "FAIL: n=%zu gave p=%.9f, expected %.9f\n",
                         c.n, r.p_value, c.expected);
            return false;
        }
    }

    // A sample symmetric about zero must not look significant.
    const std::vector<double> balanced{1.0, -1.0, 1.0, -1.0, 1.0, -1.0};
    const auto sym = sign_flip_test(balanced);
    if (sym.p_value < 0.5) {
        std::fprintf(stderr, "FAIL: a sample with mean 0 gave p=%.4f\n",
                     sym.p_value);
        return false;
    }
    std::printf("      p = 2/2^n on constant input for n=3,5,8,12; "
                "p=%.3f on a zero-mean sample\n", sym.p_value);
    return true;
}

/// @brief The sampled branch must never claim p = 0.
bool test_sign_flip_p_is_never_zero() {
    std::vector<double> x(25, 1.0);            // above the exact limit
    const auto r = sign_flip_test(x, 500, 1);
    if (r.exact) {
        std::fprintf(stderr, "FAIL: n=25 should not be enumerated\n");
        return false;
    }
    if (r.p_value <= 0.0) {
        std::fprintf(stderr, "FAIL: reported p = %.9f from %llu samples\n",
                     r.p_value, static_cast<unsigned long long>(r.assignments));
        return false;
    }
    const double floor_p = 1.0 / 501.0;
    if (std::abs(r.p_value - floor_p) > 1e-12) {
        std::fprintf(stderr, "FAIL: expected the add-one floor %.9f, got %.9f\n",
                     floor_p, r.p_value);
        return false;
    }
    std::printf("      500 sampled assignments floor p at %.5f rather than 0\n",
                r.p_value);
    return true;
}

// --------------------------------------------------------------------------

std::string write_csv(const std::string& name, const std::string& body) {
    const char* tmp = std::getenv("TMPDIR");
    const std::string path =
        std::string(tmp ? tmp : "/tmp") + "/titans_wf_" + name + ".csv";
    std::ofstream f(path);
    f << body;
    return path;
}

/**
 * @brief A session mean cannot remove a trend that reverses inside the day.
 *
 * The fixture rises at +10 bps per second for 100 s, then falls at -40 bps per
 * second for 25 s, so the SESSION mean forward return is zero by construction
 * while the local drift is large and of both signs. Trades alternate between
 * aggressive buys and sells, so neither side has an exposure advantage.
 *
 * With a single session mean the correction subtracts ~0 and the rise makes
 * every aggressive buy look toxic: the toxic set is one-sided. With a rolling
 * estimate the local trend is removed and the imbalance collapses.
 *
 * This is the defect the 28-day walk-forward surfaced, reduced to a case small
 * enough to reason about by hand.
 */
bool test_rolling_drift_tracks_a_reversing_trend() {
    constexpr int kUp = 1000;       // 100 ms apart -> 100 s
    constexpr int kDown = 250;      // 25 s
    constexpr double kP0 = 41000.0;

    std::string body;
    body.reserve(1 << 16);
    char buf[256];
    double last_price = kP0;
    for (int i = 0; i < kUp + kDown; ++i) {
        const long long t = static_cast<long long>(i) * 100;
        double price;
        if (i < kUp) {
            price = kP0 * (1.0 + 1e-6 * static_cast<double>(t));
            last_price = price;
        } else {
            const double dt = static_cast<double>(t - kUp * 100);
            price = last_price * (1.0 - 4e-6 * dt);
        }
        // Alternate the aggressor so neither side is over-represented.
        const bool buyer_maker = (i % 2) == 1;
        std::snprintf(buf, sizeof(buf), "%d,%.8f,1.00000000,%d,%d,%lld,%s,True\n",
                      i, price, i, i, t, buyer_maker ? "True" : "False");
        body += buf;
    }
    const std::string path = write_csv("reversing_trend", body);

    auto count_sides = [&](int64_t drift_window_ms, DatasetStats& stats,
                           size_t& toxic_buy, size_t& toxic_sell) {
        ToxicFlowLabelConfig cfg;
        cfg.horizon_ms = 1000;
        cfg.threshold_bps = 5.0;
        cfg.drift_adjust = true;
        cfg.drift_window_ms = drift_window_ms;
        BinanceToxicFlowDataset ds(cfg);
        if (!ds.load(path)) return false;
        const auto labels = ds.build_labels();
        const auto& trades = ds.trades();
        toxic_buy = toxic_sell = 0;
        for (size_t i = 0; i < trades.size(); ++i) {
            if (labels[i] <= 0) continue;
            if (trades[i].aggressor_sign() > 0) ++toxic_buy; else ++toxic_sell;
        }
        stats = ds.stats();
        return true;
    };

    DatasetStats s_session{}, s_rolling{};
    size_t sb = 0, ss = 0, rb = 0, rs = 0;
    if (!count_sides(0, s_session, sb, ss) ||
        !count_sides(20000, s_rolling, rb, rs)) {
        std::fprintf(stderr, "FAIL: could not label the fixture\n");
        return false;
    }

    std::printf("      session mean: drift %+.3f bps, spread %.3f, toxic "
                "%zu buy / %zu sell\n",
                s_session.drift_bps, s_session.drift_spread_bps, sb, ss);
    std::printf("      rolling 20 s: drift %+.3f bps, spread %.3f, toxic "
                "%zu buy / %zu sell\n",
                s_rolling.drift_bps, s_rolling.drift_spread_bps, rb, rs);

    if (s_session.drift_spread_bps != 0.0) {
        std::fprintf(stderr, "FAIL: a single session mean must have zero "
                             "spread, got %.6f\n", s_session.drift_spread_bps);
        return false;
    }
    if (s_rolling.drift_spread_bps < 10.0) {
        std::fprintf(stderr, "FAIL: the rolling estimate should span the +10 "
                             "and -40 bps regimes; spread was only %.3f\n",
                     s_rolling.drift_spread_bps);
        return false;
    }
    // Scale-free: what matters is how lopsided the toxic set is, not how many
    // trades the fixture happens to contain. The rising phase is four times
    // longer than the falling one, so the counts are not expected to be equal
    // even when the correction works -- the SHARE on one side is.
    auto lopsidedness = [](size_t buy, size_t sell) {
        const double total = static_cast<double>(buy + sell);
        return total == 0.0 ? 0.0
            : std::abs(static_cast<double>(buy) - static_cast<double>(sell)) / total;
    };
    const double session_skew = lopsidedness(sb, ss);
    const double rolling_skew = lopsidedness(rb, rs);
    std::printf("      one-sidedness of the toxic set: %.2f -> %.2f\n",
                session_skew, rolling_skew);

    if (session_skew < 0.5) {
        std::fprintf(stderr, "FAIL: the fixture should leave the session-mean "
                             "labels badly one-sided; skew was only %.2f\n",
                     session_skew);
        return false;
    }
    if (rolling_skew > 0.2) {
        std::fprintf(stderr,
            "FAIL: the rolling estimate did not remove the local trend: "
            "one-sidedness went from %.2f to %.2f\n",
            session_skew, rolling_skew);
        return false;
    }
    return true;
}

/// @brief The date parser feeding the fold labels.
bool test_day_label_from_path() {
    struct Case { const char* in; const char* want; };
    const Case cases[] = {
        {"data/raw/BTCUSDT-aggTrades-2024-01-15.csv", "2024-01-15"},
        {"/abs/path/ETHUSDT-aggTrades-2023-12-31.csv", "2023-12-31"},
        {"BTCUSDT-aggTrades-2024-02-04.csv", "2024-02-04"},
        {"something_else.csv", "something_else"},
    };
    for (const auto& c : cases) {
        const std::string got = day_label_from_path(c.in);
        if (got != c.want) {
            std::fprintf(stderr, "FAIL: %s -> %s, expected %s\n",
                         c.in, got.c_str(), c.want);
            return false;
        }
    }
    return true;
}

/**
 * @brief An embargo holds back exactly the samples adjacent to the next day.
 *
 * The split is on time, not on a count. A day whose last hour is quiet and
 * whose first hour is frantic would give a completely different embargo under a
 * count-based rule, and the sweep would be measuring trade density.
 */
bool test_embargo_splits_on_time_not_on_count() {
    // Ten samples an hour apart, the last at t = 10h.
    std::vector<double> v;
    std::vector<Timestamp> ms;
    for (int i = 1; i <= 10; ++i) {
        v.push_back(static_cast<double>(i));
        ms.push_back(static_cast<Timestamp>(i) * 3600000);
    }
    const Timestamp day_end = 10 * 3600000;

    // No embargo: everything is usable immediately.
    const auto none = split_by_embargo(v, ms, day_end, 0);
    if (none.body.size() != v.size() || !none.tail.empty()) {
        std::fprintf(stderr, "FAIL: a zero embargo withheld %zu samples\n",
                     none.tail.size());
        return false;
    }

    // Two and a half hours puts the cut at 7.5h, so the samples at 8h, 9h and
    // 10h are inside it. Spelling the boundary out matters: an off-by-one here
    // silently changes what "embargo" means and the sweep would be measuring
    // something other than what it says.
    const auto two_five = split_by_embargo(v, ms, day_end, 9000000);
    if (two_five.tail.size() != 3 || two_five.body.size() != 7) {
        std::fprintf(stderr, "FAIL: 2.5h embargo gave %zu held back, %zu usable;"
                             " expected 3 and 7\n",
                     two_five.tail.size(), two_five.body.size());
        return false;
    }
    if (two_five.tail[0] != 8.0 || two_five.tail[2] != 10.0) {
        std::fprintf(stderr, "FAIL: the wrong samples were held back (%g..%g)\n",
                     two_five.tail.front(), two_five.tail.back());
        return false;
    }
    // The sample exactly ON the boundary stays usable: the embargo excludes
    // what is strictly newer than the cut, and a half-open rule that drifts
    // would make two adjacent embargo values disagree by one sample.
    const auto exact = split_by_embargo(v, ms, day_end, 7200000);   // cut at 8h
    if (exact.tail.size() != 2 || exact.body.back() != 8.0) {
        std::fprintf(stderr, "FAIL: the boundary sample was not kept (%zu held)\n",
                     exact.tail.size());
        return false;
    }

    // The two halves must partition: nothing invented, nothing lost.
    if (two_five.body.size() + two_five.tail.size() != v.size()) {
        std::fprintf(stderr, "FAIL: split lost or duplicated samples\n");
        return false;
    }

    // An embargo longer than the day withholds all of it, rather than
    // silently clamping to something that still looks like a training set.
    const auto whole = split_by_embargo(v, ms, day_end, 100 * 3600000);
    if (!whole.body.empty() || whole.tail.size() != v.size()) {
        std::fprintf(stderr, "FAIL: an over-long embargo left %zu usable\n",
                     whole.body.size());
        return false;
    }
    return true;
}

/**
 * @brief Flow samples must carry the timestamp of the trade that produced them.
 *
 * The mapping from sample index to trade index is not the identity -- samples
 * begin only once the flow window is warm -- so a caller reconstructing it by
 * arithmetic would be off by the window length and the embargo would silently
 * cut in the wrong place.
 */
bool test_flow_samples_carry_their_own_timestamps() {
    std::vector<context::AggTrade> trades;
    std::vector<int8_t> labels;
    for (int i = 0; i < 300; ++i) {
        context::AggTrade t;
        t.agg_trade_id = static_cast<uint64_t>(i);
        t.price = 100.0;
        t.quantity = 1.0;
        t.transact_time_ms = 1000 + i;
        t.is_buyer_maker = (i % 3 == 0);
        trades.push_back(t);
        labels.push_back(0);
    }
    lanes::FlowPolicy::Config cfg;
    cfg.window = 50;
    const auto run = run_policy_over_day(trades, labels, cfg, true);

    if (run.abs_net.size() != run.abs_net_ms.size()) {
        std::fprintf(stderr, "FAIL: %zu samples but %zu timestamps\n",
                     run.abs_net.size(), run.abs_net_ms.size());
        return false;
    }
    if (run.abs_net.empty()) {
        std::fprintf(stderr, "FAIL: no samples collected at all\n");
        return false;
    }
    // The first sample cannot predate the window filling.
    if (run.abs_net_ms.front() < trades[cfg.window - 1].transact_time_ms) {
        std::fprintf(stderr,
            "FAIL: first sample stamped %lld, before the window was warm at %lld\n",
            static_cast<long long>(run.abs_net_ms.front()),
            static_cast<long long>(trades[cfg.window - 1].transact_time_ms));
        return false;
    }
    for (std::size_t i = 1; i < run.abs_net_ms.size(); ++i) {
        if (run.abs_net_ms[i] < run.abs_net_ms[i - 1]) {
            std::fprintf(stderr, "FAIL: sample timestamps went backwards at %zu\n", i);
            return false;
        }
    }
    return true;
}

}  // namespace

bool run_walk_forward_tests() {
    struct Case { const char* name; bool (*fn)(); };
    const Case cases[] = {
        {"the decision scoring a trade never saw it", test_decision_precedes_its_trade},
        {"folds that overlap their test day are refused", test_fold_guard_refuses_overlap},
        {"bootstrap refuses samples too small to interval", test_bootstrap_refuses_small_samples},
        {"bootstrap covers at the rate it claims",       test_bootstrap_coverage},
        {"bootstrap is reproducible from its seed",      test_bootstrap_is_reproducible},
        {"sign-flip test matches hand-computed p",       test_sign_flip_exact_values},
        {"sampled permutation p is never zero",          test_sign_flip_p_is_never_zero},
        {"rolling drift tracks a reversing trend",       test_rolling_drift_tracks_a_reversing_trend},
        {"day labels parse out of file paths",           test_day_label_from_path},
        {"an embargo splits on time, not on count",      test_embargo_splits_on_time_not_on_count},
        {"flow samples carry their own timestamps",      test_flow_samples_carry_their_own_timestamps},
    };
    bool all = true;
    for (const auto& c : cases) {
        std::printf("  [ RUN ] %s\n", c.name);
        const bool ok = c.fn();
        std::printf("  [ %s ] %s\n", ok ? "OK  " : "FAIL", c.name);
        all = all && ok;
    }
    return all;
}
