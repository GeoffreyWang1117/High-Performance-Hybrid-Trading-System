/**
 * @file test_change_point.cpp
 * @brief The properties a regression gate is worse than useless without.
 *
 * A gate that never fires and a gate that always fires are both green builds.
 * Neither is visible without asserting both directions, so both are here:
 *
 *   - It must catch a planted step.
 *   - It must stay quiet on a stationary series, at about the rate its own
 *     alpha promises, over many independent draws rather than one lucky run.
 *   - It must NOT fire on a single slow run. One bad sample is interference;
 *     a regression is a level that moved and stayed moved.
 *   - It must not confuse a step with two interleaved modes, in EITHER
 *     direction. Getting this wrong the first way makes the gate blind: a
 *     planted +20% step in update_level was reported as "two modes 20.6%
 *     apart, cannot gate" and passed the build.
 *   - It must hold back a shift that is statistically significant but smaller
 *     than the noise. On the real 40-run series a 2% injection reaches
 *     p = 0.005; firing there is how a gate gets switched off by its owners.
 *   - The prefix-sum distance formulas must equal the definition they optimise.
 *     Everything above is computed from them.
 */

#include "titans/eval/change_point.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace titans::eval;

namespace {

/// @brief Deliberately not Gaussian. The method is non-parametric and the data
///        it will see is a level plus a heavy right tail from interference.
double noisy(Rng& rng, double level, double spread) {
    const double u = static_cast<double>(rng.below(1000000)) / 1000000.0;
    const double tail = (u > 0.9) ? 3.0 * (u - 0.9) * 10.0 : 0.0;
    return level * (1.0 + spread * (u - 0.5) + spread * tail);
}

std::vector<double> stationary(Rng& rng, std::size_t n, double level, double spread) {
    std::vector<double> s;
    s.reserve(n);
    for (std::size_t i = 0; i < n; ++i) s.push_back(noisy(rng, level, spread));
    return s;
}

std::vector<double> with_step(Rng& rng, std::size_t n, std::size_t at,
                              double level, double step_pct, double spread) {
    std::vector<double> s;
    s.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double l = (i < at) ? level : level * (1.0 + step_pct / 100.0);
        s.push_back(noisy(rng, l, spread));
    }
    return s;
}

// ============================================================================
// The statistic
// ============================================================================

/// @brief The prefix-sum forms must agree with the definitions they replace.
bool test_distance_sums_match_the_definition() {
    Rng rng(7);
    for (int trial = 0; trial < 40; ++trial) {
        const std::size_t m = 2 + static_cast<std::size_t>(rng.below(20));
        const std::size_t n = 2 + static_cast<std::size_t>(rng.below(20));
        std::vector<double> a = stationary(rng, m, 100.0, 0.4);
        std::vector<double> b = stationary(rng, n, 130.0, 0.4);
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());

        double naive_within = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i)
            for (std::size_t j = i + 1; j < a.size(); ++j)
                naive_within += std::fabs(a[i] - a[j]);
        double naive_cross = 0.0;
        for (const double x : a)
            for (const double y : b) naive_cross += std::fabs(x - y);

        const double fast_within = sorted_pair_sum(a);
        const double fast_cross = sorted_cross_sum(a, b);
        const double tol = 1e-9 * std::max(1.0, std::fabs(naive_cross));
        if (std::fabs(fast_within - naive_within) > tol ||
            std::fabs(fast_cross - naive_cross) > tol) {
            std::fprintf(stderr,
                "FAIL: within %.9f vs %.9f, cross %.9f vs %.9f\n",
                fast_within, naive_within, fast_cross, naive_cross);
            return false;
        }
    }
    return true;
}

// ============================================================================
// Detection, both directions
// ============================================================================

bool test_detects_a_planted_step() {
    Rng rng(11);
    ChangePointConfig cfg;
    cfg.permutations = 299;
    const std::vector<double> s = with_step(rng, 40, 24, 100.0, 20.0, 0.05);

    const GateResult g = evaluate_gate(s, cfg);
    if (!g.valid || !g.regressed) {
        std::fprintf(stderr,
            "FAIL: a +20%% step was not reported as a regression"
            " (valid=%d change=%d effect=%.1f sd p=%.4f ungated=%d)\n",
            g.valid, g.has_change, g.effect_sigmas, g.p_value, g.ungated);
        return false;
    }
    // Within a couple of points of where it was planted.
    const auto found = static_cast<long long>(g.change_index);
    if (std::llabs(found - 24) > 3) {
        std::fprintf(stderr, "FAIL: step planted at 24, found at %lld\n", found);
        return false;
    }
    return true;
}

/**
 * @brief The false-positive half, over many independent series rather than one.
 *
 * "Stayed quiet on a no-op build" is one sample. The rate is the property.
 */
bool test_quiet_on_stationary_series() {
    ChangePointConfig cfg;
    cfg.permutations = 199;
    const int trials = 120;
    int fired = 0;
    for (int t = 0; t < trials; ++t) {
        Rng rng(1000 + static_cast<std::uint64_t>(t));
        const std::vector<double> s = stationary(rng, 40, 100.0, 0.05);
        cfg.seed = 5000 + static_cast<std::uint64_t>(t);
        const GateResult g = evaluate_gate(s, cfg);
        if (g.regressed || g.improved) ++fired;
    }
    // The effect bar makes the realised rate far below alpha; allow generous
    // slack so this does not become a flaky test about its own seeds, but
    // still fail a gate that has started firing on noise.
    const double rate = static_cast<double>(fired) / trials;
    if (rate > 0.05) {
        std::fprintf(stderr,
            "FAIL: fired on %d of %d stationary series (%.1f%%)\n",
            fired, trials, 100.0 * rate);
        return false;
    }
    return true;
}

/// @brief One slow run is interference. A gate that fires on it is a pager.
bool test_a_single_outlier_is_not_a_regression() {
    Rng rng(23);
    ChangePointConfig cfg;
    cfg.permutations = 299;
    std::vector<double> s = stationary(rng, 40, 100.0, 0.03);
    s[30] = 400.0;                        // one catastrophic run

    const GateResult g = evaluate_gate(s, cfg);
    if (g.regressed) {
        std::fprintf(stderr,
            "FAIL: a single 4x outlier was reported as a regression"
            " (shift %+.1f%%, %.1f sd)\n", g.delta_pct, g.effect_sigmas);
        return false;
    }
    return true;
}

/**
 * @brief Significant is not the same as worth waking someone up.
 *
 * A long series makes an arbitrarily small shift detectable. The effect bar is
 * what stops the gate becoming noise with a p-value attached.
 */
bool test_effect_bar_holds_back_a_tiny_significant_shift() {
    Rng rng(31);
    ChangePointConfig cfg;
    cfg.permutations = 299;
    // Shift far smaller than the spread, over enough points to be detectable.
    const std::vector<double> s = with_step(rng, 80, 40, 100.0, 0.5, 0.08);

    const GateResult g = evaluate_gate(s, cfg);
    if (g.regressed) {
        std::fprintf(stderr,
            "FAIL: a %.1f sd shift cleared a %.1f sd bar\n",
            g.effect_sigmas, cfg.effect_sigmas);
        return false;
    }
    return true;
}

// ============================================================================
// Steps versus modes
// ============================================================================

/**
 * @brief The bug this test exists for.
 *
 * A step is two clusters. So are two modes. Told apart only by arrangement:
 * a step's labels are one block then the other, two modes' labels interleave.
 * A modality test that sorts the data first cannot see the difference, and the
 * first version of `assess_modality` did exactly that -- it marked a planted
 * +20% regression "bimodal, cannot gate" and the build passed.
 */
bool test_a_step_is_not_mistaken_for_two_modes() {
    Rng rng(41);
    const std::vector<double> s = with_step(rng, 40, 20, 100.0, 20.0, 0.03);
    const Modality m = assess_modality(s);
    if (m.bimodal) {
        std::fprintf(stderr,
            "FAIL: a step was called bimodal (gap %.1f%%, runs %zu, z %.2f)\n",
            m.gap_pct, m.runs, m.order_z);
        return false;
    }
    if (!m.time_ordered) {
        std::fprintf(stderr,
            "FAIL: a step's labels were not detected as time-ordered"
            " (runs %zu, z %.2f)\n", m.runs, m.order_z);
        return false;
    }
    return true;
}

/// @brief And the other direction: interleaved modes must not be gated.
bool test_interleaved_modes_are_not_gated() {
    Rng rng(43);
    std::vector<double> s;
    s.reserve(40);
    for (int i = 0; i < 40; ++i) {
        const bool slow = rng.below(2) == 1;      // mode chosen per run, at random
        s.push_back(noisy(rng, slow ? 148.0 : 102.0, 0.01));
    }
    const Modality m = assess_modality(s);
    if (!m.bimodal) {
        std::fprintf(stderr,
            "FAIL: interleaved modes not detected (gap %.1f%%, sep %.1f sd,"
            " minority %.2f, z %.2f)\n",
            m.gap_pct, m.separation_sigmas, m.minority_fraction, m.order_z);
        return false;
    }
    ChangePointConfig cfg;
    cfg.permutations = 299;
    const GateResult g = evaluate_gate(s, cfg);
    if (g.regressed) {
        std::fprintf(stderr, "FAIL: a bimodal metric produced a regression verdict\n");
        return false;
    }
    if (!g.ungated) {
        std::fprintf(stderr, "FAIL: a bimodal metric was not marked ungated\n");
        return false;
    }
    return true;
}

// ============================================================================
// Refusals and determinism
// ============================================================================

bool test_short_series_is_refused_not_passed() {
    Rng rng(53);
    const std::vector<double> s = stationary(rng, kMinSeriesForGate - 1, 100.0, 0.05);
    const GateResult g = evaluate_gate(s);
    if (g.valid) {
        std::fprintf(stderr, "FAIL: a %zu-point series was judged rather than refused\n",
                     s.size());
        return false;
    }
    if (g.refusal.empty()) {
        std::fprintf(stderr, "FAIL: refusal carried no reason; a silent pass and a\n"
                             "      refusal look identical on a green build\n");
        return false;
    }
    return true;
}

bool test_same_seed_same_verdict() {
    Rng rng(61);
    const std::vector<double> s = with_step(rng, 40, 20, 100.0, 12.0, 0.05);
    ChangePointConfig cfg;
    cfg.permutations = 199;
    cfg.seed = 9876;
    const GateResult a = evaluate_gate(s, cfg);
    const GateResult b = evaluate_gate(s, cfg);
    if (a.p_value != b.p_value || a.change_index != b.change_index ||
        a.regressed != b.regressed) {
        std::fprintf(stderr,
            "FAIL: same input and seed gave different verdicts"
            " (p %.6f vs %.6f, idx %zu vs %zu)\n",
            a.p_value, b.p_value, a.change_index, b.change_index);
        return false;
    }
    // And the p-value floor is honest about how many shuffles backed it.
    if (a.has_change && a.p_value < 1.0 / static_cast<double>(cfg.permutations + 1)) {
        std::fprintf(stderr,
            "FAIL: p = %.6f is below the 1/(B+1) floor for %zu shuffles\n",
            a.p_value, cfg.permutations);
        return false;
    }
    return true;
}

struct Case {
    const char* name;
    bool (*fn)();
};

const Case kCases[] = {
    {"distance sums match the definition", test_distance_sums_match_the_definition},
    {"detects a planted step", test_detects_a_planted_step},
    {"quiet on stationary series", test_quiet_on_stationary_series},
    {"a single outlier is not a regression", test_a_single_outlier_is_not_a_regression},
    {"effect bar holds back a tiny significant shift", test_effect_bar_holds_back_a_tiny_significant_shift},
    {"a step is not mistaken for two modes", test_a_step_is_not_mistaken_for_two_modes},
    {"interleaved modes are not gated", test_interleaved_modes_are_not_gated},
    {"a short series is refused, not passed", test_short_series_is_refused_not_passed},
    {"same seed, same verdict", test_same_seed_same_verdict},
};

}  // namespace

bool run_change_point_tests() {
    bool all = true;
    for (const auto& c : kCases) {
        const bool ok = c.fn();
        std::printf("  [%s] %s\n", ok ? "pass" : "FAIL", c.name);
        if (!ok) all = false;
    }
    return all;
}
