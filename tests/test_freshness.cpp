/**
 * @file test_freshness.cpp
 * @brief The freshness contract, and the histogram it is measured with.
 *
 * These pin the properties whose failure would be invisible in a report:
 *
 *   - The histogram's bucket layout must bracket every value it is given, or
 *     every percentile in the repository is quietly wrong by an unknown amount.
 *   - The coordinated-omission correction must actually move a percentile that
 *     a stall would otherwise hide. A no-op correction still compiles, still
 *     runs, and still prints a reassuring p99.
 *   - Offered age must be recorded BEFORE the expiry check. The first version
 *     of this code recorded it after, which truncated the distribution at the
 *     TTL and made the SLO report MET by discarding exactly the samples that
 *     would have failed it.
 *   - A slow lane that publishes too rarely must be reported as breaking its
 *     contract even when nothing expires, tears, or is dropped.
 */

#include "titans/bench/histogram.hpp"
#include "titans/lanes/advisory.hpp"
#include "titans/lanes/freshness.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

using namespace titans;
using namespace titans::lanes;
using titans::bench::Histogram;

namespace {

/// @brief Every value must land in a bucket that contains it, monotonically.
bool test_histogram_buckets_bracket_every_value() {
    std::size_t prev = 0;
    for (uint64_t v = 0; v < (1ULL << 20); ++v) {
        const std::size_t i = Histogram::index_of(v);
        if (i >= Histogram::kCounts) {
            std::fprintf(stderr, "FAIL: value %llu maps to index %zu, table is %zu\n",
                         static_cast<unsigned long long>(v), i, Histogram::kCounts);
            return false;
        }
        if (i < prev) {
            std::fprintf(stderr, "FAIL: index went backwards at %llu\n",
                         static_cast<unsigned long long>(v));
            return false;
        }
        if (Histogram::lowest_equivalent(i) > v || v > Histogram::highest_equivalent(i)) {
            std::fprintf(stderr,
                "FAIL: %llu landed in bucket [%llu, %llu]\n",
                static_cast<unsigned long long>(v),
                static_cast<unsigned long long>(Histogram::lowest_equivalent(i)),
                static_cast<unsigned long long>(Histogram::highest_equivalent(i)));
            return false;
        }
        prev = i;
    }
    // Sampled across the remaining octaves, up to nanosecond values far beyond
    // any latency this system will see.
    for (unsigned b = 20; b < 63; ++b) {
        for (uint64_t d = 0; d < 4; ++d) {
            const uint64_t v = (1ULL << b) + d * ((1ULL << b) / 4);
            const std::size_t i = Histogram::index_of(v);
            if (i >= Histogram::kCounts ||
                Histogram::lowest_equivalent(i) > v ||
                v > Histogram::highest_equivalent(i)) {
                std::fprintf(stderr, "FAIL: octave %u value %llu\n", b,
                             static_cast<unsigned long long>(v));
                return false;
            }
        }
    }
    std::printf("      2^20 values exhaustively + 4 per octave to 2^62, all "
                "bracketed; %.2f%% worst-case error, %zu counters\n",
                Histogram::relative_error() * 100.0, Histogram::kCounts);
    return true;
}

/// @brief Percentiles must be within the layout's own stated error.
bool test_histogram_percentiles_are_within_stated_error() {
    Histogram h;
    for (int i = 1; i <= 10000; ++i) h.record(static_cast<uint64_t>(i));

    struct Case { double p; double truth; };
    const Case cases[] = {{50, 5000}, {90, 9000}, {99, 9900}, {99.9, 9990}};
    for (const auto& c : cases) {
        const double got = static_cast<double>(h.percentile(c.p));
        // The layout rounds up to the bucket's top, so a value below truth is
        // a real defect while a value slightly above it is the stated cost.
        const double upper = c.truth * (1.0 + Histogram::relative_error()) + 1.0;
        if (got < c.truth - 1.0 || got > upper) {
            std::fprintf(stderr,
                "FAIL: p%.1f reported %.0f, expected within [%.0f, %.0f]\n",
                c.p, got, c.truth - 1.0, upper);
            return false;
        }
    }
    if (h.percentile(100) != h.max()) {
        std::fprintf(stderr, "FAIL: p100 %llu != max %llu\n",
                     static_cast<unsigned long long>(h.percentile(100)),
                     static_cast<unsigned long long>(h.max()));
        return false;
    }
    std::printf("      p50/p90/p99/p99.9 over 1..10000 all within %.2f%%\n",
                Histogram::relative_error() * 100.0);
    return true;
}

/**
 * @brief A stall must show up in the corrected percentile and not in the raw one.
 *
 * This is Gil Tene's coordinated omission, reduced to its smallest form: 1000
 * samples at the expected 10-unit interval, then one 500-unit freeze. The
 * uncorrected p99 says the system is fine. It is not.
 */
bool test_coordinated_omission_correction_moves_the_tail() {
    Histogram raw, corrected;
    for (int i = 0; i < 1000; ++i) {
        raw.record(10);
        corrected.record_corrected(10, 10);
    }
    raw.record(500);
    corrected.record_corrected(500, 10);

    const uint64_t raw_p99 = raw.percentile(99);
    const uint64_t cor_p99 = corrected.percentile(99);
    std::printf("      500-unit stall in a 10-unit stream: raw p99 %llu, "
                "corrected p99 %llu\n",
                static_cast<unsigned long long>(raw_p99),
                static_cast<unsigned long long>(cor_p99));

    if (raw_p99 > 20) {
        std::fprintf(stderr, "FAIL: the uncorrected p99 should hide the stall, "
                             "got %llu\n", static_cast<unsigned long long>(raw_p99));
        return false;
    }
    if (cor_p99 < 300) {
        std::fprintf(stderr,
            "FAIL: the correction did not reconstruct the omitted samples; "
            "p99 %llu\n", static_cast<unsigned long long>(cor_p99));
        return false;
    }
    // Zero interval must disable the correction entirely.
    Histogram off;
    for (int i = 0; i < 1000; ++i) off.record_corrected(10, 0);
    off.record_corrected(500, 0);
    if (off.percentile(99) != raw_p99) {
        std::fprintf(stderr, "FAIL: expected_interval=0 must behave like "
                             "record()\n");
        return false;
    }
    return true;
}

// --------------------------------------------------------------------------

Advisory make_advisory(Timestamp issued, Timestamp ttl_ns, Timestamp horizon_ns) {
    Advisory a;
    a.stance = AdvisoryStance::RiskOff;
    a.size_multiplier = 0.25f;
    a.risk_direction = 1;
    a.issued_at = issued;
    a.valid_until = issued + ttl_ns;
    a.signal_horizon_ns = horizon_ns;
    a.context_generation = 1;
    return a;
}

/**
 * @brief The SLO must not be satisfiable by throwing away the failing samples.
 *
 * A TTL tighter than the horizon truncates the age distribution. If offered age
 * is recorded after the expiry check, every sample that would breach the SLO is
 * discarded before it is counted and the contract reports MET on a lane that is
 * plainly failing. That is what the first version of this code did.
 */
bool test_offered_age_is_recorded_before_expiry() {
    constexpr Timestamp kHorizon = 1000LL * 1000000;   // 1000 ms
    constexpr Timestamp kTtl     =  200LL * 1000000;   //  200 ms, tighter

    AdvisorySlot slot;
    FreshnessPolicy policy;                 // gate off, SLO at 1.0 x horizon
    AdvisoryView view(slot, AdvisoryStance::RiskOn, policy);

    slot.publish(make_advisory(/*issued=*/0, kTtl, kHorizon));

    // Read at ages well past both the TTL and the horizon.
    const Timestamp ages_ms[] = {10, 50, 150, 900, 1500, 3000, 4000};
    for (Timestamp ms : ages_ms) view.current(ms * 1000000, 1);

    const auto& fm = view.freshness();
    if (fm.offered().count() != 7) {
        std::fprintf(stderr,
            "FAIL: %llu of 7 reads recorded an offered age. Ages past the TTL "
            "are being discarded before they are counted.\n",
            static_cast<unsigned long long>(fm.offered().count()));
        return false;
    }
    if (fm.slo_met(policy, kHorizon)) {
        std::fprintf(stderr,
            "FAIL: SLO reported MET with an offered-age p99 of %llu ms against "
            "a 1000 ms horizon\n",
            static_cast<unsigned long long>(fm.offered().percentile(99) / 1000000));
        return false;
    }
    std::printf("      7 reads recorded past a 200 ms TTL; offered p99 %llu ms "
                "vs 1000 ms horizon -> BREACHED\n",
                static_cast<unsigned long long>(fm.offered().percentile(99) / 1000000));
    return true;
}

/// @brief The gate must reject on age while leaving the offered record intact.
bool test_gate_rejects_by_age_without_hiding_it() {
    constexpr Timestamp kHorizon = 1000LL * 1000000;
    constexpr Timestamp kTtl     = 60000LL * 1000000;   // wide: nothing expires

    AdvisorySlot slot;
    FreshnessPolicy policy;
    policy.max_age_fraction = 0.25;         // reject past 250 ms
    AdvisoryView view(slot, AdvisoryStance::RiskOn, policy);
    slot.publish(make_advisory(0, kTtl, kHorizon));

    int acted = 0;
    for (Timestamp ms = 50; ms <= 950; ms += 100) {
        const Advisory a = view.current(ms * 1000000, 1);
        if (a.stance == AdvisoryStance::RiskOff) ++acted;
    }
    // Ages 50,150,250 pass (<= 250 ms); 350..950 do not.
    if (acted != 3) {
        std::fprintf(stderr, "FAIL: %d advisories acted on, expected 3\n", acted);
        return false;
    }
    if (view.rejected() != 0) {
        std::fprintf(stderr, "FAIL: %llu expiry rejections with a 60 s TTL; the "
                             "age gate is being counted as expiry\n",
                     static_cast<unsigned long long>(view.rejected()));
        return false;
    }
    if (view.rejected_stale() != 7) {
        std::fprintf(stderr, "FAIL: %llu age rejections, expected 7\n",
                     static_cast<unsigned long long>(view.rejected_stale()));
        return false;
    }
    const auto& fm = view.freshness();
    if (fm.offered().count() != 10 || fm.acted().count() != 3) {
        std::fprintf(stderr,
            "FAIL: offered %llu (want 10), acted %llu (want 3). The gate must "
            "not be able to improve the distribution the SLO is declared on.\n",
            static_cast<unsigned long long>(fm.offered().count()),
            static_cast<unsigned long long>(fm.acted().count()));
        return false;
    }
    std::printf("      gate at 0.25 x H: 3 acted, 7 rejected by age, 0 by "
                "expiry; offered still records all 10\n");
    return true;
}

/// @brief An advisory with no declared horizon must not be gated to nothing.
bool test_undeclared_horizon_disables_the_gate() {
    AdvisorySlot slot;
    FreshnessPolicy policy;
    policy.max_age_fraction = 0.25;
    AdvisoryView view(slot, AdvisoryStance::RiskOn, policy);

    slot.publish(make_advisory(0, 60000LL * 1000000, /*horizon=*/0));
    const Advisory a = view.current(5000LL * 1000000, 1);
    if (a.stance != AdvisoryStance::RiskOff) {
        std::fprintf(stderr, "FAIL: a producer that declared no horizon had "
                             "every advisory rejected\n");
        return false;
    }
    std::printf("      horizon 0 passes the gate rather than rejecting "
                "everything\n");
    return true;
}

/**
 * @brief Publishing too rarely is a contract breach with nothing else wrong.
 *
 * The lane in this test expires nothing, tears nothing and drops nothing. It is
 * still failing, and before CadenceBudget existed no report said so.
 */
bool test_slow_publisher_breaches_cadence_with_nothing_expired() {
    constexpr Timestamp kHorizon = 1000LL * 1000000;
    CadenceBudget budget(kHorizon / 4);      // must publish every 250 ms

    // Publishes 1 s apart in market time: four times slower than the budget.
    for (int i = 0; i < 10; ++i) budget.observe_publish(i * 1000LL * 1000000);

    if (budget.passed()) {
        std::fprintf(stderr, "FAIL: 1 s publish intervals passed a 250 ms "
                             "budget\n");
        return false;
    }
    if (budget.violations() != 9) {
        std::fprintf(stderr, "FAIL: %llu violations, expected 9\n",
                     static_cast<unsigned long long>(budget.violations()));
        return false;
    }

    CadenceBudget ok(kHorizon / 4);
    for (int i = 0; i < 10; ++i) ok.observe_publish(i * 100LL * 1000000);
    if (!ok.passed()) {
        std::fprintf(stderr, "FAIL: 100 ms intervals failed a 250 ms budget\n");
        return false;
    }
    std::printf("      1 s publishes breach a 250 ms budget 9 times; 100 ms "
                "publishes pass\n");
    return true;
}

}  // namespace

bool run_freshness_tests() {
    struct Case { const char* name; bool (*fn)(); };
    const Case cases[] = {
        {"histogram buckets bracket every value",   test_histogram_buckets_bracket_every_value},
        {"percentiles stay within the stated error", test_histogram_percentiles_are_within_stated_error},
        {"coordinated-omission correction moves the tail", test_coordinated_omission_correction_moves_the_tail},
        {"offered age is recorded before expiry",   test_offered_age_is_recorded_before_expiry},
        {"age gate rejects without hiding the age", test_gate_rejects_by_age_without_hiding_it},
        {"an undeclared horizon disables the gate", test_undeclared_horizon_disables_the_gate},
        {"publishing too rarely breaches cadence",  test_slow_publisher_breaches_cadence_with_nothing_expired},
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

// ---------------------------------------------------------------------------
// Appended: the design claim behind Advisory::parameter.
// ---------------------------------------------------------------------------

namespace {

/**
 * @brief A delayed DECISION misaligns; a delayed PARAMETER does not.
 *
 * This is the claim that made `Advisory::parameter` exist, reduced to a case
 * with no threads in it. A threshold-crossing rule is only correct at the
 * instant it is evaluated: ship the answer and k trades of delay moves it onto
 * the wrong trades, ship the threshold and the consumer re-evaluates against
 * state that is current by construction.
 *
 * Measured in the lane on real trades: shipping decisions agreed with the
 * ideal on 49.1% of its actions, shipping the parameter on 89.8%, at the same
 * threshold to four significant figures.
 */
bool test_delayed_decision_misaligns_but_delayed_parameter_does_not() {
    constexpr int kN = 4000;
    constexpr std::size_t kWindow = 50;
    constexpr int kDelay = 3;          // the median staleness measured in the lane
    constexpr double kWarn = 6.0;

    // A tape with bursts: long calm stretches punctuated by one-sided pushes,
    // which is the shape that makes a threshold crossing a moment rather than
    // a state.
    std::vector<double> flow(kN);
    uint64_t rng = 12345;
    auto next = [&] {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        return rng;
    };
    for (int i = 0; i < kN; ++i) {
        const bool burst = ((i / 40) % 7) == 0;
        const double mag = burst ? 1.0 : 0.15;
        flow[i] = (next() % 2 ? 1.0 : -1.0) * mag * (0.5 + (next() % 100) / 100.0);
    }

    auto net_at = [&](int i) {          // window ending at trade i-1
        double s = 0.0;
        for (int j = std::max(0, i - static_cast<int>(kWindow)); j < i; ++j) s += flow[j];
        return s;
    };
    auto decide = [&](int i) {
        const double net = net_at(i);
        return std::pair<bool, int>(std::abs(net) > kWarn, net > 0 ? 1 : -1);
    };
    // Aggressor side of trade i, which the rule must match to fire.
    auto side = [&](int i) { return flow[i] > 0 ? 1 : -1; };

    int ideal_on = 0, dec_agree = 0, dec_on = 0, par_agree = 0, par_on = 0;
    for (int i = static_cast<int>(kWindow) + kDelay; i < kN; ++i) {
        const auto ideal = decide(i);
        const bool ideal_fire = ideal.first && ideal.second == side(i);
        if (ideal_fire) ++ideal_on;

        // Shipped decision: computed kDelay trades ago, applied now.
        const auto stale = decide(i - kDelay);
        const bool dec_fire = stale.first && stale.second == side(i);
        if (dec_fire) ++dec_on;
        if (dec_fire && ideal_fire) ++dec_agree;

        // Shipped parameter: the threshold is kDelay trades old, the decision
        // is taken now. Here the threshold is constant, which is the point --
        // it is the slow-moving quantity.
        const auto fresh = decide(i);
        const bool par_fire = fresh.first && fresh.second == side(i);
        if (par_fire) ++par_on;
        if (par_fire && ideal_fire) ++par_agree;
    }

    const double dec_rate = ideal_on ? static_cast<double>(dec_agree) / ideal_on : 0.0;
    const double par_rate = ideal_on ? static_cast<double>(par_agree) / ideal_on : 0.0;
    std::printf("      %d ideal actions; shipped decision agrees %.1f%%, "
                "shipped parameter agrees %.1f%% at %d trades of delay\n",
                ideal_on, dec_rate * 100.0, par_rate * 100.0, kDelay);

    if (ideal_on < 50) {
        std::fprintf(stderr, "FAIL: fixture produced only %d actions; it cannot "
                             "discriminate\n", ideal_on);
        return false;
    }
    if (par_rate < 0.999) {
        std::fprintf(stderr, "FAIL: a delayed PARAMETER must not move the "
                             "decision at all; agreement %.3f\n", par_rate);
        return false;
    }
    if (dec_rate > 0.85) {
        std::fprintf(stderr,
            "FAIL: a decision delayed by %d trades should land on materially "
            "different trades; agreement was %.3f. If this ever passes, the "
            "fixture stopped being burst-like and the test stopped testing "
            "anything.\n", kDelay, dec_rate);
        return false;
    }
    return true;
}

}  // namespace

bool run_advisory_kind_tests() {
    std::printf("  [ RUN ] a delayed decision misaligns, a delayed parameter does not\n");
    const bool ok = test_delayed_decision_misaligns_but_delayed_parameter_does_not();
    std::printf("  [ %s ] a delayed decision misaligns, a delayed parameter does not\n",
                ok ? "OK  " : "FAIL");
    return ok;
}
