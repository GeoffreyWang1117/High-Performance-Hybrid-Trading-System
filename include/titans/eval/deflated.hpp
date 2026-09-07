/**
 * @file deflated.hpp
 * @brief Correcting a reported statistic for the number of trials behind it.
 *
 * THE PROBLEM
 * -----------
 * Every interval and p-value in this repository is computed as though the
 * configuration that produced it was the only one ever considered. Run a sweep
 * over five settings, report the best, and the interval is optimistic by an
 * amount nobody has quantified -- because the best of five draws from a null
 * distribution is not a draw from that null distribution.
 *
 * The walk-forward has so far avoided this by never selecting: sweeps were run,
 * every point was reported, and the headline came from a fixed configuration.
 * That is a discipline, not a mechanism, and it survives exactly as long as
 * whoever is holding it remembers. This header is the mechanism.
 *
 * WHAT IS HERE
 * ------------
 *   - `expected_max_z(N)`: what the best of N trials reaches under the null.
 *     This is the term the Deflated Sharpe Ratio (Bailey & Lopez de Prado,
 *     2014) subtracts, and the reason a sweep's winner needs a higher bar than
 *     a single pre-registered test.
 *   - `sidak_p` / `sidak_level`: family-wise adjustment. Exact under
 *     independence and less punitive than Bonferroni, which matters when the
 *     trials are a parameter sweep whose points are strongly correlated -- both
 *     are then conservative, and the less conservative one is the honest choice.
 *   - `deflate`: puts them together and says whether the result survives.
 *
 * WHAT IS NOT HERE
 * ----------------
 * A correction for correlated trials. Sidak assumes independence; a sweep over
 * an embargo parameter produces trials that are almost perfectly correlated, so
 * the adjustment is more conservative than necessary. Being too harsh on your
 * own result is the safe direction, and the trial count is an explicit argument
 * so that a caller claiming fewer effective trials has to say so in the open
 * rather than by passing 1 and moving on.
 */

#pragma once

#include <cmath>
#include <cstddef>
#include <limits>
#include <string>

namespace titans {
namespace eval {

/// @brief Standard normal CDF.
inline double normal_cdf(double z) {
    return 0.5 * std::erfc(-z * 0.70710678118654752440);
}

/**
 * @brief Upper tail, 1 - Phi(z), computed without cancelling.
 *
 * `1.0 - normal_cdf(z)` is exactly 0 for z beyond about 8, because the CDF has
 * already rounded to 1. A deflated p-value printed as 0 claims infinite
 * evidence, which is the same failure the sign-flip test avoids by printing a
 * floor instead of zero. `erfc` computes the tail directly and stays accurate
 * to the edge of double precision.
 */
inline double normal_sf(double z) {
    return 0.5 * std::erfc(z * 0.70710678118654752440);
}

/**
 * @brief Inverse standard normal CDF.
 *
 * Acklam's rational approximation, relative error under 1.15e-9 across the open
 * unit interval, refined by one Halley step. Written out rather than pulled in
 * because the alternative is a dependency for one function.
 */
inline double normal_quantile(double p) {
    if (p <= 0.0) return -std::numeric_limits<double>::infinity();
    if (p >= 1.0) return std::numeric_limits<double>::infinity();

    static const double a[6] = {-3.969683028665376e+01, 2.209460984245205e+02,
                                -2.759285104469687e+02, 1.383577518672690e+02,
                                -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[5] = {-5.447609879822406e+01, 1.615858368580409e+02,
                                -1.556989798598866e+02, 6.680131188771972e+01,
                                -1.328068155288572e+01};
    static const double c[6] = {-7.784894002430293e-03, -3.223964580411365e-01,
                                -2.400758277161838e+00, -2.549732539343734e+00,
                                 4.374664141464968e+00,  2.938163982698783e+00};
    static const double d[4] = {7.784695709041462e-03, 3.224671290700398e-01,
                                2.445134137142996e+00, 3.754408661907416e+00};
    const double p_low = 0.02425, p_high = 1.0 - p_low;
    double x;
    if (p < p_low) {
        const double q = std::sqrt(-2.0 * std::log(p));
        x = (((((c[0]*q + c[1])*q + c[2])*q + c[3])*q + c[4])*q + c[5]) /
            ((((d[0]*q + d[1])*q + d[2])*q + d[3])*q + 1.0);
    } else if (p <= p_high) {
        const double q = p - 0.5;
        const double r = q * q;
        x = (((((a[0]*r + a[1])*r + a[2])*r + a[3])*r + a[4])*r + a[5]) * q /
            (((((b[0]*r + b[1])*r + b[2])*r + b[3])*r + b[4])*r + 1.0);
    } else {
        const double q = std::sqrt(-2.0 * std::log(1.0 - p));
        x = -(((((c[0]*q + c[1])*q + c[2])*q + c[3])*q + c[4])*q + c[5]) /
             ((((d[0]*q + d[1])*q + d[2])*q + d[3])*q + 1.0);
    }
    // One Halley refinement against the CDF, which removes the approximation's
    // last few digits of error.
    const double e = normal_cdf(x) - p;
    const double u = e * std::sqrt(2.0 * M_PI) * std::exp(x * x / 2.0);
    return x - u / (1.0 + x * u / 2.0);
}

/**
 * @brief Expected maximum of @p trials independent standard normal draws.
 *
 * The Gumbel-based form used by the Deflated Sharpe Ratio. This is the bar a
 * sweep's winner has to clear before it is evidence of anything: with 10 trials
 * the best result under a pure null is already +1.54 standard errors, which is
 * comfortably "significant" at the usual threshold.
 *
 * One trial returns 0: the expected value of a single draw from a mean-zero
 * null is zero, and there is nothing to deflate.
 */
inline double expected_max_z(std::size_t trials) {
    if (trials <= 1) return 0.0;
    constexpr double kEuler = 0.5772156649015329;
    const auto n = static_cast<double>(trials);
    return (1.0 - kEuler) * normal_quantile(1.0 - 1.0 / n) +
           kEuler * normal_quantile(1.0 - 1.0 / (n * M_E));
}

/// @brief Family-wise p-value for the best of @p trials. Exact when independent.
inline double sidak_p(double p, std::size_t trials) {
    if (trials <= 1) return p;
    const double q = 1.0 - std::pow(1.0 - p, static_cast<double>(trials));
    return q > 1.0 ? 1.0 : q;
}

/**
 * @brief Per-interval confidence level giving family-wise @p level over N.
 *
 * For 95% family-wise coverage over 5 trials each interval must be built at
 * 98.98%, which is why a deflated interval is visibly wider than the raw one
 * rather than a rounding difference.
 */
inline double sidak_level(double level, std::size_t trials) {
    if (trials <= 1) return level;
    return std::pow(level, 1.0 / static_cast<double>(trials));
}

/**
 * @brief What a result looks like once the search behind it is accounted for.
 */
struct Deflation {
    std::size_t trials = 1;
    double observed = 0.0;
    double standard_error = 0.0;
    /// The expected best-of-N under the null, in the statistic's own units.
    double expected_max_null = 0.0;
    /// Observed minus that bar, in standard errors.
    double deflated_z = 0.0;
    /// One-sided p for the deflated statistic.
    double deflated_p = 1.0;
    /// The raw p, adjusted family-wise. Reported beside the deflated one
    /// because they answer different questions and can disagree.
    double adjusted_p = 1.0;
    bool survives = false;
    /// Present when the inputs cannot support a deflation at all.
    std::string refusal;

    bool valid() const { return refusal.empty(); }
};

/**
 * @brief Deflate an observed statistic for the number of configurations tried.
 *
 * @param observed  point estimate, in its own units
 * @param standard_error  its standard error, same units
 * @param raw_p     the p-value as computed for a single pre-registered test
 * @param trials    configurations evaluated to produce @p observed
 * @param alpha     significance the result is asked to survive
 *
 * A zero or negative standard error is refused rather than treated as
 * infinitely precise: an interval of width zero would make every result
 * survive every correction, which is the failure mode this exists to prevent.
 */
inline Deflation deflate(double observed, double standard_error, double raw_p,
                         std::size_t trials, double alpha = 0.05) {
    Deflation d;
    d.trials = trials == 0 ? 1 : trials;
    d.observed = observed;
    d.standard_error = standard_error;
    if (!(standard_error > 0.0)) {
        d.refusal = "standard error is not positive; nothing to deflate against";
        return d;
    }
    d.expected_max_null = standard_error * expected_max_z(d.trials);
    d.deflated_z = (observed - d.expected_max_null) / standard_error;
    d.deflated_p = normal_sf(d.deflated_z);
    d.adjusted_p = sidak_p(raw_p, d.trials);
    d.survives = d.deflated_p < alpha && d.adjusted_p < alpha;
    return d;
}

}  // namespace eval
}  // namespace titans
