/**
 * @file bootstrap.hpp
 * @brief Bootstrap confidence intervals and an exact sign-flip permutation test.
 *
 * WHY NOT JUST QUOTE A MEDIAN AND A RANGE
 * ---------------------------------------
 * `titans_lanes` reports "median +0.0277, range [-0.0172, +0.0614], sd 0.0328"
 * across repeated replays and then asks whether the median clears two standard
 * deviations. That rule is a reasonable smell test, but it is not an interval:
 * it assumes a symmetric, roughly normal sampling distribution for a statistic
 * (informedness = TPR - FPR) computed from a few folds with a 1.6% positive
 * class. Both assumptions are doubtful here.
 *
 * A percentile bootstrap makes no distributional assumption, and a sign-flip
 * permutation test gives an exact p-value for "the per-fold effect is symmetric
 * about zero" when the number of folds is small enough to enumerate -- which,
 * for a walk-forward over a month of days, it is.
 *
 * DETERMINISM
 * -----------
 * `std::uniform_int_distribution` is not specified to produce the same values
 * across standard-library implementations, so a result file produced here would
 * not reproduce elsewhere. This file uses splitmix64 and Lemire's bounded
 * reduction directly, which are fully specified by the code below.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace titans {
namespace eval {

/// @brief splitmix64. Fully specified here so results reproduce anywhere.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    /// @brief Uniform on [0, bound). Lemire's multiply-shift with rejection,
    ///        so it is unbiased rather than modulo-skewed.
    std::uint64_t below(std::uint64_t bound) {
        if (bound == 0) return 0;
        const auto threshold = static_cast<std::uint64_t>(-bound) % bound;
        while (true) {
            const std::uint64_t r = next();
            using u128 = unsigned __int128;
            const auto m = static_cast<u128>(r) * static_cast<u128>(bound);
            if (static_cast<std::uint64_t>(m) >= threshold) {
                return static_cast<std::uint64_t>(m >> 64);
            }
        }
    }

private:
    std::uint64_t state_;
};

/**
 * @brief A percentile bootstrap interval, or an explicit refusal to give one.
 *
 * `valid` is false when the sample is too small for the interval to mean
 * anything. That is not defensiveness: with n observations the percentile
 * bootstrap's endpoints are order statistics of resample means, and for very
 * small n the interval is systematically too narrow -- it would understate
 * uncertainty exactly where uncertainty is largest.
 */
struct BootstrapCI {
    double point = 0.0;        ///< Statistic on the observed sample.
    double lo = 0.0;
    double hi = 0.0;
    double level = 0.95;
    std::size_t resamples = 0;
    std::size_t n = 0;
    bool valid = false;
    /// Set when n is small enough that the interval should be read as a hint.
    bool underpowered = false;
    std::string refusal;

    /// @brief True when the whole interval sits on one side of zero.
    bool excludes_zero() const { return valid && ((lo > 0.0) == (hi > 0.0)); }
};

/// Below this many observations no interval is reported at all.
inline constexpr std::size_t kMinSamplesForCI = 5;
/// Below this many the interval is reported but flagged.
inline constexpr std::size_t kUnderpoweredCI = 12;

/**
 * @brief Percentile bootstrap interval for the mean of `x`.
 *
 * @param level     Two-sided coverage, e.g. 0.95.
 * @param resamples Bootstrap replicates. 10000 puts Monte-Carlo error on the
 *                  2.5% endpoint well below the width of the interval itself.
 */
inline BootstrapCI bootstrap_mean_ci(const std::vector<double>& x,
                                     double level = 0.95,
                                     std::size_t resamples = 10000,
                                     std::uint64_t seed = 42) {
    BootstrapCI out;
    out.level = level;
    out.n = x.size();
    out.resamples = resamples;

    if (x.size() < kMinSamplesForCI) {
        out.refusal = "only " + std::to_string(x.size()) +
                      " observations; a percentile bootstrap needs at least " +
                      std::to_string(kMinSamplesForCI) +
                      " before its endpoints mean anything";
        return out;
    }

    double sum = 0.0;
    for (double v : x) sum += v;
    out.point = sum / static_cast<double>(x.size());

    std::vector<double> means;
    means.reserve(resamples);
    Rng rng(seed);
    const std::uint64_t n = x.size();
    for (std::size_t r = 0; r < resamples; ++r) {
        double s = 0.0;
        for (std::uint64_t i = 0; i < n; ++i) s += x[rng.below(n)];
        means.push_back(s / static_cast<double>(n));
    }
    std::sort(means.begin(), means.end());

    const double alpha = (1.0 - level) / 2.0;
    auto at = [&](double p) {
        const auto idx = static_cast<std::size_t>(p * (means.size() - 1) + 0.5);
        return means[std::min(idx, means.size() - 1)];
    };
    out.lo = at(alpha);
    out.hi = at(1.0 - alpha);
    out.valid = true;
    out.underpowered = x.size() < kUnderpoweredCI;
    return out;
}

/**
 * @brief Result of a two-sided sign-flip permutation test.
 *
 * The null is that each per-fold value is equally likely to have carried the
 * opposite sign -- i.e. the effect is symmetric about zero. That is the right
 * null for "did this policy do better than nothing, across days", and unlike a
 * t-test it assumes nothing about the shape of the per-fold distribution.
 */
struct PermutationTest {
    double observed_mean = 0.0;
    double p_value = 1.0;
    std::size_t n = 0;
    /// Sign assignments considered. Equals 2^n when the test was exact.
    std::uint64_t assignments = 0;
    bool exact = false;
    bool valid = false;
    std::string refusal;
};

/// At or below this many folds every sign assignment is enumerated.
inline constexpr std::size_t kExactPermutationLimit = 20;

/**
 * @brief Two-sided sign-flip test on paired/per-fold values.
 *
 * For n <= 20 all 2^n sign assignments are enumerated and the p-value is exact.
 * Above that it is estimated from `samples` random assignments using the
 * add-one estimator (1 + #extreme) / (1 + samples), which cannot return zero.
 * A reported p of exactly 0 would claim more evidence than a finite number of
 * permutations can supply.
 */
inline PermutationTest sign_flip_test(const std::vector<double>& diffs,
                                      std::size_t samples = 20000,
                                      std::uint64_t seed = 42) {
    PermutationTest out;
    out.n = diffs.size();
    if (diffs.size() < 3) {
        out.refusal = "only " + std::to_string(diffs.size()) +
                      " values; the smallest two-sided p a sign-flip test can "
                      "produce with n is 2^-(n-1), so n<3 cannot reach any "
                      "useful level";
        return out;
    }

    double total = 0.0;
    for (double v : diffs) total += v;
    out.observed_mean = total / static_cast<double>(diffs.size());
    const double observed_abs = std::abs(total);

    if (diffs.size() <= kExactPermutationLimit) {
        const std::uint64_t combos = 1ULL << diffs.size();
        std::uint64_t extreme = 0;
        for (std::uint64_t mask = 0; mask < combos; ++mask) {
            double s = 0.0;
            for (std::size_t i = 0; i < diffs.size(); ++i) {
                s += (mask >> i) & 1ULL ? -diffs[i] : diffs[i];
            }
            // >= so the observed assignment counts itself, which is what makes
            // the test exact rather than anticonservative.
            if (std::abs(s) >= observed_abs) ++extreme;
        }
        out.p_value = static_cast<double>(extreme) / static_cast<double>(combos);
        out.assignments = combos;
        out.exact = true;
    } else {
        Rng rng(seed);
        std::uint64_t extreme = 0;
        for (std::size_t r = 0; r < samples; ++r) {
            double s = 0.0;
            std::uint64_t bits = rng.next();
            std::size_t used = 0;
            for (std::size_t i = 0; i < diffs.size(); ++i) {
                if (used == 64) { bits = rng.next(); used = 0; }
                s += (bits >> used) & 1ULL ? -diffs[i] : diffs[i];
                ++used;
            }
            if (std::abs(s) >= observed_abs) ++extreme;
        }
        out.p_value = static_cast<double>(1 + extreme) /
                      static_cast<double>(1 + samples);
        out.assignments = samples;
        out.exact = false;
    }
    out.valid = true;
    return out;
}

}  // namespace eval
}  // namespace titans
