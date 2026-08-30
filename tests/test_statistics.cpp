/**
 * @file test_statistics.cpp
 * @brief Validates the hand-rolled Student-t tail against known critical values.
 *
 * paired_t_test previously computed its p-value from a normal approximation.
 * These experiments run a handful of seeds, and at n=5 that approximation
 * reports p=0.005 where the correct value is p=0.050 -- an order of magnitude,
 * and on the wrong side of every conventional threshold.
 *
 * The replacement evaluates the regularized incomplete beta by continued
 * fraction. That is exactly the kind of numerical code that is easy to get
 * subtly wrong and impossible to eyeball, so it is checked here against
 * published critical values rather than trusted.
 */

#include "titans/context/experiment_persistence.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace titans::context;

namespace {

bool close(double a, double b, double tol) {
    return std::abs(a - b) <= tol;
}

/**
 * @brief Two-sided p at the published 0.05 and 0.01 critical values.
 *
 * Critical values from standard t tables; a correct implementation must return
 * the alpha that defines them.
 */
bool test_t_critical_values() {
    struct Case { double t; double df; double expected_p; const char* note; };
    const Case cases[] = {
        {12.706205,     1,  0.05,  "df=1  alpha=0.05"},
        { 4.302653,     2,  0.05,  "df=2  alpha=0.05"},
        { 3.182446,     3,  0.05,  "df=3  alpha=0.05"},
        { 2.776445,     4,  0.05,  "df=4  alpha=0.05"},
        { 2.228139,    10,  0.05,  "df=10 alpha=0.05"},
        { 2.042272,    30,  0.05,  "df=30 alpha=0.05"},
        { 4.604095,     4,  0.01,  "df=4  alpha=0.01"},
        { 3.169273,    10,  0.01,  "df=10 alpha=0.01"},
        { 2.750000,    30,  0.01,  "df=30 alpha~0.01"},
    };

    bool ok = true;
    for (const auto& c : cases) {
        const double p = StatisticalAnalyzer::student_t_two_sided_p(c.t, c.df);
        const bool good = close(p, c.expected_p, 1e-3);
        std::printf("      t=%9.6f df=%5.0f -> p=%.6f (expect %.4f) %s  [%s]\n",
                    c.t, c.df, p, c.expected_p, good ? "ok" : "MISMATCH", c.note);
        if (!good) ok = false;
    }
    return ok;
}

/// @brief Degenerate and boundary inputs must not produce nonsense.
bool test_t_edge_cases() {
    const double p0 = StatisticalAnalyzer::student_t_two_sided_p(0.0, 5);
    if (!close(p0, 1.0, 1e-9)) {
        std::fprintf(stderr, "FAIL: t=0 should give p=1, got %.9f\n", p0);
        return false;
    }

    // Large df must converge on the normal: |z|=1.959964 is the 0.05 two-sided
    // critical value of the standard normal.
    const double p_norm = StatisticalAnalyzer::student_t_two_sided_p(1.959964, 1e7);
    if (!close(p_norm, 0.05, 1e-3)) {
        std::fprintf(stderr,
                     "FAIL: t should approach the normal at large df; got "
                     "p=%.6f for z=1.96, expected 0.05\n", p_norm);
        return false;
    }

    // Monotone decreasing in t.
    double prev = 1.0;
    for (double t = 0.0; t <= 6.0; t += 0.25) {
        const double p = StatisticalAnalyzer::student_t_two_sided_p(t, 8);
        if (p > prev + 1e-12) {
            std::fprintf(stderr,
                         "FAIL: p-value is not monotone in t (t=%.2f gave %.6f "
                         "after %.6f)\n", t, p, prev);
            return false;
        }
        prev = p;
    }
    std::printf("      t=0 -> p=1; large df matches normal; monotone in t\n");
    return true;
}

/**
 * @brief The normal approximation this replaced was materially wrong at small n.
 *
 * Documents the size of the error that motivated the change, and fails if a
 * future edit quietly restores the approximation.
 */
bool test_small_n_differs_from_normal_approximation() {
    const double t = 2.776445;    // exactly the 0.05 critical value at df=4
    const double p_t = StatisticalAnalyzer::student_t_two_sided_p(t, 4);
    // Standard-normal two-sided tail, computed here rather than via the
    // class's private helper: the point is to compare against the formula the
    // implementation used to use, not to reach into it.
    const double p_norm = std::erfc(t / std::sqrt(2.0));

    std::printf("      t=%.6f, df=4:  Student-t p=%.4f  vs  normal p=%.4f "
                "(ratio %.1fx)\n", t, p_t, p_norm, p_t / p_norm);

    if (!close(p_t, 0.05, 1e-3)) {
        std::fprintf(stderr, "FAIL: Student-t p should be 0.05, got %.6f\n", p_t);
        return false;
    }
    if (p_norm > 0.02) {
        std::fprintf(stderr,
                     "FAIL: expected the normal approximation to understate p "
                     "badly at df=4, but it gave %.6f. Is paired_t_test back on "
                     "the normal approximation?\n", p_norm);
        return false;
    }
    return true;
}

/// @brief End-to-end: paired_t_test on data with a known answer.
bool test_paired_t_test_end_to_end() {
    // Treatment beats baseline by a constant 0.10 with small noise: clearly
    // significant.
    const std::vector<double> baseline  = {0.70, 0.72, 0.69, 0.71, 0.70};
    const std::vector<double> treatment = {0.80, 0.83, 0.79, 0.81, 0.81};

    const auto r = StatisticalAnalyzer::paired_t_test(baseline, treatment);
    std::printf("      mean diff %.4f, t=%.3f, p=%.6f -> %s\n",
                r.mean_diff, r.t_statistic, r.p_value, r.interpretation.c_str());

    if (!close(r.mean_diff, 0.104, 1e-3)) {
        std::fprintf(stderr, "FAIL: mean difference %.4f, expected 0.104\n", r.mean_diff);
        return false;
    }
    if (!r.significant_at_05) {
        std::fprintf(stderr, "FAIL: a consistent 0.10 gain over 5 pairs should "
                             "be significant; p=%.6f\n", r.p_value);
        return false;
    }

    // Identical arms: no effect, p must be 1.
    const auto null = StatisticalAnalyzer::paired_t_test(baseline, baseline);
    if (null.significant_at_05) {
        std::fprintf(stderr, "FAIL: identical samples reported as significant "
                             "(p=%.6f)\n", null.p_value);
        return false;
    }
    return true;
}

}  // namespace

bool run_statistics_tests() {
    struct Case { const char* name; bool (*fn)(); };
    const Case cases[] = {
        {"Student-t matches published critical values", test_t_critical_values},
        {"Student-t edge cases and monotonicity",       test_t_edge_cases},
        {"normal approximation was wrong at small n",   test_small_n_differs_from_normal_approximation},
        {"paired_t_test end to end",                    test_paired_t_test_end_to_end},
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
