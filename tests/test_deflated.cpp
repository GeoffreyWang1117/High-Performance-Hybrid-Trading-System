/**
 * @file test_deflated.cpp
 * @brief The multiple-testing correction, and the property it exists to enforce.
 *
 * The whole point of this machinery is a claim about numbers nobody measures:
 * that the best of N tries is not a draw from the null even when each try is.
 * So the central test does not check a formula against a constant -- it
 * SIMULATES the null, takes the best of N, and requires the correction to be
 * the thing that stops it being reported as a discovery.
 *
 * The rest pin the pieces that would otherwise fail silently:
 *
 *   - `normal_quantile` must invert `normal_cdf`. A wrong tail here makes every
 *     deflated p-value wrong in a direction nobody would notice.
 *   - One trial must deflate to nothing. If it did not, every existing result
 *     in the repository would move the moment this header was included.
 *   - A zero standard error must be refused. Treated as infinite precision it
 *     makes every result survive every correction -- the exact failure this
 *     code exists to prevent, arriving through the back door.
 */

#include "titans/eval/bootstrap.hpp"
#include "titans/eval/deflated.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace titans::eval;

namespace {

/// @brief Standard normal draw, Box-Muller on the repository's own splitmix64.
double gaussian(Rng& rng) {
    const double u1 = (static_cast<double>(rng.below(1u << 30)) + 1.0) / (1u << 30);
    const double u2 = static_cast<double>(rng.below(1u << 30)) / (1u << 30);
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2);
}

// ============================================================================
// The pieces
// ============================================================================

/**
 * @brief The far tail must not collapse to zero.
 *
 * `1 - Phi(z)` underflows to exactly 0 somewhere past z = 8, and a p-value of
 * 0 claims more evidence than any finite computation can supply. The
 * walk-forward's own deflated statistic reaches z = 10, so this is not a
 * hypothetical.
 */
bool test_the_far_tail_does_not_underflow_to_zero() {
    // The direct form stays positive everywhere the deflated statistic can go.
    for (const double z : {6.0, 8.0, 10.0, 20.0, 35.0}) {
        const double sf = normal_sf(z);
        if (!(sf > 0.0)) {
            std::fprintf(stderr, "FAIL: normal_sf(%.1f) = %g\n", z, sf);
            return false;
        }
    }
    // The cancelling form does not, and that is the whole reason for the
    // direct one. If this stops being true the test is no longer guarding
    // anything and should be deleted rather than quietly passing.
    if (1.0 - normal_cdf(20.0) != 0.0) {
        std::fprintf(stderr,
            "FAIL: 1 - cdf(20) = %g, expected it to have underflowed;\n"
            "      this test no longer demonstrates the problem it guards\n",
            1.0 - normal_cdf(20.0));
        return false;
    }
    // Where the cancelling form still carries digits, the two must agree.
    // Past about z = 5 it is quantised by the spacing of doubles near 1, so
    // the comparison is only meaningful below that.
    for (const double z : {0.0, 0.5, 1.96, 3.0, 5.0}) {
        const double a = normal_sf(z);
        const double b = 1.0 - normal_cdf(z);
        if (std::fabs(a - b) > 1e-12 * std::max(1.0, std::fabs(b))) {
            std::fprintf(stderr, "FAIL: tail forms disagree at z=%.2f (%g vs %g)\n",
                         z, a, b);
            return false;
        }
    }
    return true;
}

bool test_quantile_inverts_the_cdf() {
    const double probes[] = {1e-6, 1e-4, 0.001, 0.01, 0.02, 0.1, 0.25, 0.5,
                             0.75, 0.9, 0.975, 0.99, 0.999, 1.0 - 1e-6};
    for (const double p : probes) {
        const double z = normal_quantile(p);
        const double back = normal_cdf(z);
        if (std::fabs(back - p) > 1e-9 * std::max(1.0, p)) {
            std::fprintf(stderr,
                "FAIL: quantile(%.9g) = %.9g, cdf back = %.9g\n", p, z, back);
            return false;
        }
    }
    return true;
}

bool test_one_trial_deflates_to_nothing() {
    if (expected_max_z(1) != 0.0 || expected_max_z(0) != 0.0) {
        std::fprintf(stderr, "FAIL: a single trial has a non-zero null maximum\n");
        return false;
    }
    const Deflation d = deflate(0.20, 0.05, 0.001, 1);
    if (!d.valid() || d.expected_max_null != 0.0) {
        std::fprintf(stderr, "FAIL: one trial moved the bar to %+.6f\n",
                     d.expected_max_null);
        return false;
    }
    if (std::fabs(d.adjusted_p - 0.001) > 1e-12) {
        std::fprintf(stderr, "FAIL: one trial changed p from 0.001 to %.6g\n",
                     d.adjusted_p);
        return false;
    }
    return true;
}

bool test_the_bar_rises_with_trials() {
    double prev = -1.0;
    for (const std::size_t n : {2u, 5u, 10u, 100u, 1000u}) {
        const double z = expected_max_z(n);
        if (z <= prev) {
            std::fprintf(stderr,
                "FAIL: expected max did not rise at N=%zu (%.4f after %.4f)\n",
                n, z, prev);
            return false;
        }
        prev = z;
    }
    // A sanity anchor a reader can check against the DSR literature: with ten
    // trials the best result under a pure null already sits above 1.5 sigma,
    // which is the whole reason a sweep's winner needs a higher bar.
    const double z10 = expected_max_z(10);
    if (z10 < 1.3 || z10 > 1.8) {
        std::fprintf(stderr, "FAIL: E[max of 10] = %.4f, expected ~1.54\n", z10);
        return false;
    }
    return true;
}

bool test_zero_standard_error_is_refused() {
    for (const double se : {0.0, -1.0}) {
        const Deflation d = deflate(0.20, se, 0.001, 5);
        if (d.valid() || d.survives) {
            std::fprintf(stderr,
                "FAIL: se = %.1f produced a verdict instead of a refusal\n", se);
            return false;
        }
    }
    return true;
}

bool test_sidak_is_monotone_and_bounded() {
    if (std::fabs(sidak_p(0.04, 1) - 0.04) > 1e-12) {
        std::fprintf(stderr, "FAIL: one trial changed the p-value\n");
        return false;
    }
    double prev = 0.04;
    for (const std::size_t n : {2u, 4u, 16u, 64u}) {
        const double q = sidak_p(0.04, n);
        if (q < prev || q > 1.0) {
            std::fprintf(stderr, "FAIL: sidak_p(0.04, %zu) = %.6f\n", n, q);
            return false;
        }
        prev = q;
    }
    // And the interval level moves the other way: more trials, wider interval.
    if (!(sidak_level(0.95, 5) > 0.95) || sidak_level(0.95, 5) >= 1.0) {
        std::fprintf(stderr, "FAIL: sidak_level(0.95, 5) = %.6f\n",
                     sidak_level(0.95, 5));
        return false;
    }
    return true;
}

// ============================================================================
// The property the whole header exists for
// ============================================================================

/**
 * @brief Simulate the thing being corrected for, and require the correction to
 *        catch it.
 *
 * Draw N independent samples from a null with no effect, keep the best, and ask
 * whether it looks significant. Uncorrected it does, often -- that is the
 * garden of forking paths, reproduced. Corrected it should not, at roughly the
 * nominal rate.
 *
 * Both halves are asserted. A correction that rejects everything would pass the
 * second half alone and is useless.
 */
bool test_correction_controls_the_best_of_n_false_discovery_rate() {
    constexpr std::size_t kTrials = 20;      // configurations swept
    constexpr int kExperiments = 400;        // independent repetitions
    constexpr std::size_t kFolds = 27;       // as in the 28-day walk-forward
    constexpr double kAlpha = 0.05;

    int naive_fired = 0, deflated_fired = 0;
    for (int e = 0; e < kExperiments; ++e) {
        Rng rng(90000 + static_cast<std::uint64_t>(e));
        double best_mean = -1e300, best_se = 0.0;
        for (std::size_t t = 0; t < kTrials; ++t) {
            // A configuration's folds: pure noise, no effect whatsoever.
            double sum = 0.0, sumsq = 0.0;
            for (std::size_t f = 0; f < kFolds; ++f) {
                const double x = gaussian(rng);
                sum += x;
                sumsq += x * x;
            }
            const double n = static_cast<double>(kFolds);
            const double mean = sum / n;
            const double var = (sumsq - n * mean * mean) / (n - 1.0);
            const double se = std::sqrt(var / n);
            if (mean > best_mean) { best_mean = mean; best_se = se; }
        }
        const double z = best_mean / best_se;
        const double raw_p = normal_sf(z);
        if (raw_p < kAlpha) ++naive_fired;

        const Deflation d = deflate(best_mean, best_se, raw_p, kTrials, kAlpha);
        if (d.survives) ++deflated_fired;
    }

    const double naive_rate = static_cast<double>(naive_fired) / kExperiments;
    const double deflated_rate = static_cast<double>(deflated_fired) / kExperiments;

    std::printf("         best-of-%zu on a pure null: %.1f%% called significant "
                "uncorrected, %.1f%% after deflation\n",
                kTrials, 100.0 * naive_rate, 100.0 * deflated_rate);

    // Uncorrected, taking the best of 20 turns a 5% test into a wildly
    // optimistic one. If this half stops holding, the test has stopped
    // simulating the problem.
    if (naive_rate < 0.30) {
        std::fprintf(stderr,
            "FAIL: best-of-%zu fired only %.1f%% of the time uncorrected;\n"
            "      the simulation is no longer reproducing the problem\n",
            kTrials, 100.0 * naive_rate);
        return false;
    }
    // Corrected, it must come back to about nominal.
    if (deflated_rate > 0.10) {
        std::fprintf(stderr,
            "FAIL: corrected rate %.1f%% against a %.0f%% target"
            " (uncorrected %.1f%%)\n",
            100.0 * deflated_rate, 100.0 * kAlpha, 100.0 * naive_rate);
        return false;
    }
    return true;
}

/// @brief And it must not reject a real effect that is large enough.
bool test_a_real_effect_still_survives() {
    constexpr std::size_t kTrials = 20;
    const Deflation d = deflate(0.1676, 0.0148, 5e-5, kTrials);
    if (!d.valid()) {
        std::fprintf(stderr, "FAIL: refused a well-formed input: %s\n",
                     d.refusal.c_str());
        return false;
    }
    if (!d.survives) {
        std::fprintf(stderr,
            "FAIL: the walk-forward's own effect (+0.1676 +/- 0.0148) did not\n"
            "      survive %zu trials: bar %+.4f, z %+.2f, p %.4g\n",
            kTrials, d.expected_max_null, d.deflated_z, d.deflated_p);
        return false;
    }
    return true;
}

struct Case {
    const char* name;
    bool (*fn)();
};

const Case kCases[] = {
    {"quantile inverts the cdf", test_quantile_inverts_the_cdf},
    {"the far tail does not underflow to zero", test_the_far_tail_does_not_underflow_to_zero},
    {"one trial deflates to nothing", test_one_trial_deflates_to_nothing},
    {"the bar rises with trials", test_the_bar_rises_with_trials},
    {"a zero standard error is refused", test_zero_standard_error_is_refused},
    {"sidak is monotone and bounded", test_sidak_is_monotone_and_bounded},
    {"correction controls the best-of-N rate", test_correction_controls_the_best_of_n_false_discovery_rate},
    {"a real effect still survives", test_a_real_effect_still_survives},
};

}  // namespace

bool run_deflated_tests() {
    bool all = true;
    for (const auto& c : kCases) {
        const bool ok = c.fn();
        std::printf("  [%s] %s\n", ok ? "pass" : "FAIL", c.name);
        if (!ok) all = false;
    }
    return all;
}
