/**
 * @file metrics.hpp
 * @brief Scoring primitives shared by the dataset audit and the walk-forward.
 *
 * These were previously local to `dataset_main.cpp`. The walk-forward needs the
 * same AUC, and a second implementation of a ranking metric is exactly the kind
 * of duplication that lets two tools quietly disagree about whether a day's
 * labels are clean.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

namespace titans {
namespace eval {

/**
 * @brief Area under the ROC curve.
 *
 * Computed via the rank-sum identity (Mann-Whitney U), which is exact and needs
 * no threshold sweep. 0.5 is chance; a feature that predicts the label
 * backwards shows up below 0.5, and |AUC - 0.5| is the quantity that matters
 * for a leakage audit -- a feature that is reliably wrong is just as much of a
 * leak as one that is reliably right.
 *
 * Ties take the average rank, so a constant score returns exactly 0.5 rather
 * than an artefact of sort order.
 */
inline double auc(const std::vector<double>& scores,
                  const std::vector<bool>& labels) {
    const std::size_t n = scores.size();
    if (n == 0 || n != labels.size()) return 0.5;

    std::vector<std::size_t> idx(n);
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(),
              [&](std::size_t a, std::size_t b) { return scores[a] < scores[b]; });

    std::vector<double> rank(n);
    std::size_t i = 0;
    while (i < n) {
        std::size_t j = i;
        while (j + 1 < n && scores[idx[j + 1]] == scores[idx[i]]) ++j;
        const double avg = 0.5 * (static_cast<double>(i) + static_cast<double>(j)) + 1.0;
        for (std::size_t k = i; k <= j; ++k) rank[idx[k]] = avg;
        i = j + 1;
    }

    double sum_pos_rank = 0.0;
    std::size_t n_pos = 0;
    for (std::size_t k = 0; k < n; ++k) {
        if (labels[k]) { sum_pos_rank += rank[k]; ++n_pos; }
    }
    const std::size_t n_neg = n - n_pos;
    if (n_pos == 0 || n_neg == 0) return 0.5;

    return (sum_pos_rank - n_pos * (n_pos + 1) / 2.0) /
           (static_cast<double>(n_pos) * static_cast<double>(n_neg));
}

/**
 * @brief What a sizing policy did, in the units a market maker cares about.
 *
 * Not "accuracy". The policy's job is to quote smaller into informed flow, so
 * the two costs are asymmetric and both are counted: adverse selection avoided,
 * versus benign flow needlessly declined.
 */
struct PolicyOutcome {
    std::uint64_t avoided_toxic = 0;   ///< toxic, sized down  (true positive)
    std::uint64_t missed_toxic = 0;    ///< toxic, full size   (false negative)
    std::uint64_t forgone_benign = 0;  ///< benign, sized down (false positive)
    std::uint64_t kept_benign = 0;     ///< benign, full size  (true negative)

    std::uint64_t toxic() const { return avoided_toxic + missed_toxic; }
    std::uint64_t benign() const { return forgone_benign + kept_benign; }
    std::uint64_t decisions() const { return toxic() + benign(); }

    double tpr() const {
        return toxic() ? static_cast<double>(avoided_toxic) / toxic() : 0.0;
    }
    double fpr() const {
        return benign() ? static_cast<double>(forgone_benign) / benign() : 0.0;
    }

    /**
     * @brief Youden's J. Zero for any policy that ignores the label, including
     *        one that sizes down on everything or on nothing.
     *
     * Accuracy would not have this property: with a 1.6% toxic rate, a policy
     * that never sizes down scores 98.4% accurate and is worth nothing.
     */
    double informedness() const { return tpr() - fpr(); }

    /// @brief Share of decisions where the policy sized down. A value at 0 or 1
    ///        means the policy is constant and informedness is 0 by
    ///        construction, not by measurement.
    double action_rate() const {
        return decisions()
            ? static_cast<double>(avoided_toxic + forgone_benign) / decisions()
            : 0.0;
    }
    bool degenerate() const {
        return decisions() == 0 || action_rate() == 0.0 || action_rate() == 1.0;
    }
};

}  // namespace eval
}  // namespace titans
