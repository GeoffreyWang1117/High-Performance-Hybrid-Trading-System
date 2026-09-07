/**
 * @file change_point.hpp
 * @brief E-Divisive change-point detection, with a noise model taken from the
 *        series rather than from inside a single run.
 *
 * THE PROBLEM THIS SOLVES
 * -----------------------
 * CI can tell you a test failed. It cannot currently tell you a benchmark got
 * slower, because nothing compares today's number against yesterday's. The
 * naive fix -- fail when a metric moves more than X% -- is a false-positive
 * generator on shared hardware, where a runner's own variance routinely exceeds
 * the regression you are trying to catch.
 *
 * WHY E-DIVISIVE
 * --------------
 * Matteson & James (2014), "A Nonparametric Approach for Multiple Change Point
 * Analysis of Multivariate Data". It is what MongoDB deployed for exactly this
 * problem, and what the continuous-performance-engineering literature points at,
 * for three reasons that matter here:
 *
 *   - Non-parametric. Benchmark series are not normal; they are a level with
 *     occasional steps and a heavy right tail from interference.
 *   - It finds a LEVEL SHIFT, not an outlier. One slow run is not a regression.
 *     A run that is slow and stays slow is.
 *   - Significance comes from a permutation test on the series itself, so the
 *     threshold is inherited from the data instead of chosen.
 *
 * WHAT THIS ADDS
 * --------------
 * Statistical significance is not enough. With a long enough series, a shift of
 * a tenth of the noise is detectable and worth nobody's morning. So detection
 * is separated from the verdict: `find_change_points` says WHERE the level
 * moved, and `evaluate_gate` says whether the move is larger than the dispersion
 * the series itself exhibits when nothing is changing.
 *
 * That dispersion has to be estimated from the SERIES. The benchmark harness
 * reports a spread across repetitions inside one process, and that number is a
 * lower bound on what a gate faces: repetitions inside a process share a cache
 * state, a page mapping, a clock domain and a thermal state. See
 * docs/REGRESSION.md for the measured gap between the two on this repository's
 * own numbers.
 *
 * COST
 * ----
 * One split scan is O(n^2) via sorted prefix sums; the permutation test is that
 * times the shuffle count. A benchmark series is tens to hundreds of points, so
 * this runs in milliseconds and there is no reason to approximate.
 */

#pragma once

#include "titans/eval/bootstrap.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace titans {
namespace eval {

// ============================================================================
// Distance sums
// ============================================================================

/**
 * @brief Sum of |x_i - x_j| over i<j, for a SORTED array.
 *
 * The naive double loop is O(k^2) and is called O(n) times per scan and O(n*B)
 * times under permutation, which is where a change-point detector normally
 * becomes too slow to run in CI. Sorted, each element's contribution to the
 * pairwise sum is (i * x_i - sum of everything before it), so the whole thing
 * is one pass.
 */
inline double sorted_pair_sum(const std::vector<double>& s) {
    double total = 0.0;
    double prefix = 0.0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        total += static_cast<double>(i) * s[i] - prefix;
        prefix += s[i];
    }
    return total;
}

/// @brief Sum of |a_i - b_j| over all pairs, for two SORTED arrays.
inline double sorted_cross_sum(const std::vector<double>& a,
                               const std::vector<double>& b) {
    if (a.empty() || b.empty()) return 0.0;
    const double b_total = std::accumulate(b.begin(), b.end(), 0.0);
    double total = 0.0;
    double b_prefix = 0.0;
    std::size_t j = 0;
    for (const double x : a) {          // a is sorted, so j only moves forward
        while (j < b.size() && b[j] <= x) {
            b_prefix += b[j];
            ++j;
        }
        total += static_cast<double>(j) * x - b_prefix;
        total += (b_total - b_prefix) -
                 static_cast<double>(b.size() - j) * x;
    }
    return total;
}

/**
 * @brief The scaled E-statistic for a split between two sorted segments.
 *
 * Matteson & James eq. (2)-(4) with alpha = 1: the between-segment mean
 * distance, less each segment's own internal mean distance, scaled by
 * mn/(m+n). Zero when the two segments are drawn from the same distribution,
 * positive when they are not.
 */
inline double e_statistic(const std::vector<double>& left_sorted,
                          const std::vector<double>& right_sorted) {
    const auto m = static_cast<double>(left_sorted.size());
    const auto n = static_cast<double>(right_sorted.size());
    if (m < 2.0 || n < 2.0) return 0.0;

    const double cross = sorted_cross_sum(left_sorted, right_sorted);
    const double within_left = sorted_pair_sum(left_sorted);
    const double within_right = sorted_pair_sum(right_sorted);

    const double e = 2.0 * cross / (m * n)
                   - within_left / (m * (m - 1.0) / 2.0)
                   - within_right / (n * (n - 1.0) / 2.0);
    return (m * n / (m + n)) * e;
}

// ============================================================================
// Detection
// ============================================================================

/// @brief A segment shorter than this cannot support a median or a dispersion.
inline constexpr std::size_t kMinSegment = 5;
/// @brief Below this many points the gate refuses rather than guesses.
inline constexpr std::size_t kMinSeriesForGate = 12;

struct ChangePointConfig {
    /// Minimum points on each side of a split.
    std::size_t min_segment = kMinSegment;
    /// Permutation shuffles. The p-value floor is 1/(shuffles+1).
    std::size_t permutations = 999;
    /// Significance required to accept a split.
    double alpha = 0.05;
    /// How many multiples of the series' own dispersion a shift must clear
    /// before it is worth a human's attention. Statistical significance alone
    /// is not a reason to page anyone.
    double effect_sigmas = 3.0;
    std::uint64_t seed = 42;
};

struct SplitScan {
    std::size_t index = 0;     ///< first index of the RIGHT segment
    double statistic = 0.0;
    bool valid = false;
};

/// @brief Best split of `s[lo, hi)`, by the E-statistic. No significance test.
inline SplitScan best_split(const std::vector<double>& s, std::size_t lo,
                            std::size_t hi, std::size_t min_segment) {
    SplitScan best;
    if (hi <= lo || hi - lo < 2 * min_segment) return best;

    // Left grows one element at a time; both sides are kept sorted so the
    // distance sums stay linear.
    std::vector<double> left, right;
    left.reserve(hi - lo);
    right.assign(s.begin() + static_cast<std::ptrdiff_t>(lo),
                 s.begin() + static_cast<std::ptrdiff_t>(hi));
    std::sort(right.begin(), right.end());

    for (std::size_t t = lo; t < hi; ++t) {
        const double v = s[t];
        left.insert(std::upper_bound(left.begin(), left.end(), v), v);
        const auto it = std::lower_bound(right.begin(), right.end(), v);
        right.erase(it);

        const std::size_t left_n = t - lo + 1;
        const std::size_t right_n = hi - t - 1;
        if (left_n < min_segment || right_n < min_segment) continue;

        const double q = e_statistic(left, right);
        if (!best.valid || q > best.statistic) {
            best.valid = true;
            best.statistic = q;
            best.index = t + 1;
        }
    }
    return best;
}

struct ChangePoint {
    std::size_t index = 0;      ///< first index of the segment AFTER the change
    double statistic = 0.0;
    double p_value = 1.0;
    std::size_t permutations = 0;
};

/**
 * @brief Significance of the best split in `s[lo, hi)`, by permutation.
 *
 * Under the null the ordering carries no information, so shuffling and
 * rescanning gives the distribution of the maximum statistic that arises by
 * chance. Scanning for the MAXIMUM under permutation, rather than testing one
 * fixed index, is what keeps the test honest about having searched every split.
 *
 * The add-one estimator, (1 + exceedances) / (1 + shuffles), so the result is
 * never 0 -- a p-value of exactly zero would claim more than a finite number of
 * shuffles can support.
 */
inline double permutation_p(const std::vector<double>& s, std::size_t lo,
                            std::size_t hi, double observed,
                            std::size_t shuffles, std::size_t min_segment,
                            std::uint64_t seed) {
    if (shuffles == 0) return 1.0;
    std::vector<double> shuffled(s.begin() + static_cast<std::ptrdiff_t>(lo),
                                 s.begin() + static_cast<std::ptrdiff_t>(hi));
    Rng rng(seed);
    std::size_t exceed = 0;
    for (std::size_t b = 0; b < shuffles; ++b) {
        for (std::size_t i = shuffled.size(); i > 1; --i) {
            const auto j = static_cast<std::size_t>(rng.below(i));
            std::swap(shuffled[i - 1], shuffled[j]);
        }
        const SplitScan sc = best_split(shuffled, 0, shuffled.size(), min_segment);
        if (sc.valid && sc.statistic >= observed) ++exceed;
    }
    return static_cast<double>(1 + exceed) / static_cast<double>(1 + shuffles);
}

/**
 * @brief E-Divisive: split, test, recurse into both halves.
 *
 * Returns change-point indices in increasing order. Each is the first index of
 * the segment that begins at the change.
 */
inline std::vector<ChangePoint> find_change_points(const std::vector<double>& s,
                                                   ChangePointConfig cfg = {}) {
    std::vector<ChangePoint> found;
    if (s.size() < 2 * cfg.min_segment) return found;

    // Explicit stack rather than recursion: a pathological series should not be
    // able to overflow one.
    std::vector<std::pair<std::size_t, std::size_t>> pending{{0, s.size()}};
    std::uint64_t seed = cfg.seed;
    while (!pending.empty()) {
        const auto [lo, hi] = pending.back();
        pending.pop_back();
        if (hi - lo < 2 * cfg.min_segment) continue;

        const SplitScan sc = best_split(s, lo, hi, cfg.min_segment);
        if (!sc.valid) continue;
        const double p = permutation_p(s, lo, hi, sc.statistic, cfg.permutations,
                                       cfg.min_segment, seed);
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        if (p > cfg.alpha) continue;

        ChangePoint cp;
        cp.index = sc.index;
        cp.statistic = sc.statistic;
        cp.p_value = p;
        cp.permutations = cfg.permutations;
        found.push_back(cp);

        pending.push_back({lo, sc.index});
        pending.push_back({sc.index, hi});
    }
    std::sort(found.begin(), found.end(),
              [](const ChangePoint& a, const ChangePoint& b) { return a.index < b.index; });
    return found;
}

// ============================================================================
// Dispersion
// ============================================================================

inline double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    const double hi = v[mid];
    if (v.size() % 2 == 1) return hi;
    const auto lo_it = std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
    return 0.5 * (*lo_it + hi);
}

/// @brief 1.4826 * MAD, so it is on the same scale as a standard deviation for
///        normal data but does not chase a single interference spike.
inline double robust_sigma(const std::vector<double>& residuals) {
    if (residuals.size() < 2) return 0.0;
    const double med = median_of(residuals);
    std::vector<double> dev;
    dev.reserve(residuals.size());
    for (const double r : residuals) dev.push_back(std::fabs(r - med));
    return 1.4826 * median_of(dev);
}

/**
 * @brief Dispersion of the series with the level shifts taken out.
 *
 * This is the number the gate compares against: what the metric does run to run
 * when nothing changed. Taking the change points out first matters -- pooling
 * across a step would fold the regression into the noise estimate and hide it.
 */
inline double residual_sigma(const std::vector<double>& s,
                             const std::vector<ChangePoint>& cps) {
    std::vector<double> residuals;
    residuals.reserve(s.size());
    std::size_t lo = 0;
    std::vector<std::size_t> bounds;
    for (const auto& cp : cps) bounds.push_back(cp.index);
    bounds.push_back(s.size());
    for (const std::size_t hi : bounds) {
        if (hi <= lo) continue;
        const std::vector<double> seg(s.begin() + static_cast<std::ptrdiff_t>(lo),
                                      s.begin() + static_cast<std::ptrdiff_t>(hi));
        const double med = median_of(seg);
        for (const double v : seg) residuals.push_back(v - med);
        lo = hi;
    }
    return robust_sigma(residuals);
}

// ============================================================================
// Modality
// ============================================================================

/**
 * @brief Whether a series has two separated modes rather than one noisy level.
 *
 * This is not a refinement. On this repository's own hardware
 * `SPSCQueue::try_push` takes one of two values, ~1.02 ns or ~1.48 ns, decided
 * once at process start and stable for that whole run -- 46% apart, while the
 * harness reports a within-run spread of 1.1%. Alignment, page colouring and
 * which physical core the pin lands on are all fixed for a process and vary
 * between them.
 *
 * A level test survives this: the modes alternate at random, so no segment is
 * systematically different and E-Divisive correctly finds nothing. The EFFECT
 * SIZE does not survive it. A robust sigma computed across a bimodal series
 * describes neither mode, so a run of luck in the mode mix would look like an
 * enormous shift. The honest response is to say the metric cannot be gated this
 * way, not to emit a verdict about it.
 *
 * Detection is 1-D 2-means -- for sorted data the optimal split is a prefix, so
 * one scan finds it -- plus three guards. The minority cluster must be large
 * enough to be a mode rather than an outlier; the clusters must be separated by
 * more than @p min_separation_sigmas of their own pooled spread; and, the guard
 * that matters most, the two clusters must be INTERLEAVED IN TIME.
 *
 * That last one is not a refinement either. A regression is also two clusters
 * -- fast before, slow after -- so a modality test that ignores order marks
 * every genuine step as "bimodal, cannot gate" and the gate silently stops
 * working. The first version of this function did exactly that: handed a
 * planted +20% step in `update_level` it reported two modes 20.6% apart and
 * passed the build. What separates the two cases is arrangement, not spread.
 *
 * So the labels are tested with Wald-Wolfowitz runs. Under interleaved modes
 * the label sequence looks like a coin; under a step it is one block of zeros
 * followed by one block of ones, which is two runs against an expectation of
 * about n/2. A z-score below @p max_order_z means the split is explained by
 * time, and the series is a shift rather than a mode mix.
 */
struct Modality {
    bool bimodal = false;
    double low = 0.0;                ///< mean of the faster cluster
    double high = 0.0;               ///< mean of the slower cluster
    double minority_fraction = 0.0;
    double separation_sigmas = 0.0;
    double gap_pct = 0.0;            ///< (high - low) / low
    /// Wald-Wolfowitz z on the label sequence. Near 0 => interleaved, so two
    /// modes. Strongly negative => the labels are blocked in time, so a step.
    double order_z = 0.0;
    std::size_t runs = 0;
    bool time_ordered = false;
};

inline Modality assess_modality(const std::vector<double>& s,
                                double min_separation_sigmas = 6.0,
                                double min_minority_fraction = 0.15,
                                double max_order_z = -2.0) {
    Modality m;
    if (s.size() < 2 * kMinSegment) return m;

    std::vector<double> sorted = s;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t n = sorted.size();

    std::vector<double> prefix(n + 1, 0.0);
    for (std::size_t i = 0; i < n; ++i) prefix[i + 1] = prefix[i] + sorted[i];

    double best_cost = 0.0;
    std::size_t best_k = 0;
    for (std::size_t k = 1; k < n; ++k) {
        const double ma = prefix[k] / static_cast<double>(k);
        const double mb = (prefix[n] - prefix[k]) / static_cast<double>(n - k);
        double cost = 0.0;
        for (std::size_t i = 0; i < k; ++i) cost += (sorted[i] - ma) * (sorted[i] - ma);
        for (std::size_t i = k; i < n; ++i) cost += (sorted[i] - mb) * (sorted[i] - mb);
        if (best_k == 0 || cost < best_cost) { best_cost = cost; best_k = k; }
    }
    if (best_k == 0) return m;

    m.low = prefix[best_k] / static_cast<double>(best_k);
    m.high = (prefix[n] - prefix[best_k]) / static_cast<double>(n - best_k);
    m.minority_fraction =
        static_cast<double>(std::min(best_k, n - best_k)) / static_cast<double>(n);
    m.gap_pct = m.low != 0.0 ? 100.0 * (m.high - m.low) / m.low : 0.0;

    std::vector<double> residuals;
    residuals.reserve(n);
    for (std::size_t i = 0; i < best_k; ++i) residuals.push_back(sorted[i] - m.low);
    for (std::size_t i = best_k; i < n; ++i) residuals.push_back(sorted[i] - m.high);
    const double pooled = robust_sigma(residuals);

    m.separation_sigmas = pooled > 0.0 ? (m.high - m.low) / pooled : 0.0;

    // Label the series IN ITS ORIGINAL ORDER, then ask whether the labels are
    // arranged by time. The cut sits between the two clusters.
    const double cut = 0.5 * (sorted[best_k - 1] + sorted[best_k]);
    std::size_t n_low = 0, n_high = 0, runs = 1;
    bool prev_high = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const bool is_high = s[i] > cut;
        if (is_high) ++n_high; else ++n_low;
        if (i > 0 && is_high != prev_high) ++runs;
        prev_high = is_high;
    }
    m.runs = runs;

    if (n_low > 0 && n_high > 0 && s.size() > 1) {
        const double a = static_cast<double>(n_low);
        const double b = static_cast<double>(n_high);
        const double nn = a + b;
        const double mu = 2.0 * a * b / nn + 1.0;
        const double var = 2.0 * a * b * (2.0 * a * b - nn) / (nn * nn * (nn - 1.0));
        m.order_z = var > 0.0 ? (static_cast<double>(runs) - mu) / std::sqrt(var) : 0.0;
    }
    m.time_ordered = m.order_z <= max_order_z;

    m.bimodal = m.minority_fraction >= min_minority_fraction &&
                (pooled == 0.0 || m.separation_sigmas >= min_separation_sigmas) &&
                !m.time_ordered;
    return m;
}

// ============================================================================
// The gate
// ============================================================================

/// @brief What the gate decided, and everything needed to argue with it.
struct GateResult {
    bool valid = false;             ///< false => refused, see `refusal`
    std::string refusal;

    bool regressed = false;         ///< a slowdown that cleared both bars
    bool improved = false;          ///< same, in the other direction

    std::size_t change_index = 0;   ///< most recent change point, if any
    bool has_change = false;
    double p_value = 1.0;

    double before = 0.0;            ///< segment medians either side
    double after = 0.0;
    double delta_pct = 0.0;
    double sigma = 0.0;             ///< series dispersion, level shifts removed
    double effect_sigmas = 0.0;     ///< |delta| in units of that dispersion

    /// Two separated modes make the effect-size bar meaningless, so a bimodal
    /// metric is reported and NOT failed on. See `assess_modality`.
    Modality modality;
    bool ungated = false;

    std::vector<ChangePoint> change_points;
};

/**
 * @brief Detect, then decide.
 *
 * Two bars, and a change has to clear both:
 *
 *   1. The level shift is significant under permutation. One slow run is not a
 *      regression; a step is.
 *   2. The shift is larger than `effect_sigmas` times the dispersion the series
 *      shows with its own steps removed. Without this bar a long enough series
 *      makes arbitrarily small shifts significant, and the gate becomes noise
 *      with a p-value attached.
 *
 * A series shorter than `kMinSeriesForGate` is refused rather than judged. The
 * refusal is a string so the caller can print WHY nothing was checked, instead
 * of a silent pass that looks exactly like a clean run.
 */
inline GateResult evaluate_gate(const std::vector<double>& s,
                                ChangePointConfig cfg = {}) {
    GateResult g;
    if (s.size() < kMinSeriesForGate) {
        g.refusal = "series has " + std::to_string(s.size()) + " points, need " +
                    std::to_string(kMinSeriesForGate);
        return g;
    }
    g.valid = true;
    g.change_points = find_change_points(s, cfg);
    g.sigma = residual_sigma(s, g.change_points);
    g.modality = assess_modality(s);
    g.ungated = g.modality.bimodal;
    if (g.ungated) {
        g.refusal = "two modes " + std::to_string(g.modality.gap_pct) +
                    "% apart; a single dispersion describes neither";
    }

    if (g.change_points.empty()) return g;

    const ChangePoint& last = g.change_points.back();
    g.has_change = true;
    g.change_index = last.index;
    g.p_value = last.p_value;

    const std::size_t prev_start =
        g.change_points.size() >= 2
            ? g.change_points[g.change_points.size() - 2].index
            : 0;
    const std::vector<double> before(s.begin() + static_cast<std::ptrdiff_t>(prev_start),
                                     s.begin() + static_cast<std::ptrdiff_t>(last.index));
    const std::vector<double> after(s.begin() + static_cast<std::ptrdiff_t>(last.index),
                                    s.end());
    g.before = median_of(before);
    g.after = median_of(after);
    const double delta = g.after - g.before;
    g.delta_pct = g.before != 0.0 ? 100.0 * delta / g.before : 0.0;
    g.effect_sigmas = g.sigma > 0.0 ? std::fabs(delta) / g.sigma : 0.0;

    const bool big_enough = g.effect_sigmas >= cfg.effect_sigmas;
    // Higher is slower for every metric this repository gates on (nanoseconds
    // per operation). A caller measuring throughput must negate before calling.
    //
    // `ungated` suppresses the verdict, not the detection: the change point and
    // its effect size are still reported, so a bimodal metric shows what it did
    // rather than vanishing from the table.
    if (big_enough && delta > 0.0 && !g.ungated) g.regressed = true;
    if (big_enough && delta < 0.0 && !g.ungated) g.improved = true;
    return g;
}

}  // namespace eval
}  // namespace titans
