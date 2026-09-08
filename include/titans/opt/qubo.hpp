/**
 * @file qubo.hpp
 * @brief Subset selection as a QUBO, with the solvers to go with it.
 *
 * THE FORMULATION
 * ---------------
 * Taken unchanged from the workshop formulation this item comes from
 * (monitoring as combinatorial subset selection):
 *
 *     min_z  - sum_i r_i z_i  +  lambda sum_{i<j} S_ij z_i z_j
 *                             +  P (sum_i z_i - K)^2
 *
 * with `r` a per-item relevance, `S` a similarity kernel, `K` the budget and
 * `P` a penalty enforcing the cardinality constraint. lambda traces a frontier:
 * at 0 it is top-K by relevance, and as it grows the selection spreads out.
 * Greedy/MMR and simulated annealing are two points on the same frontier rather
 * than different algorithms for different problems.
 *
 * WHY THE PENALTY IS HERE AND NEVER BINDS
 * ---------------------------------------
 * The penalty is what makes this a QUBO -- an unconstrained binary quadratic
 * form, which is what an annealer or a QPU can accept. It is kept so `energy()`
 * reports the published objective and so the same matrix could be handed to
 * such a backend unchanged.
 *
 * The annealer here does not need it, because it moves by SWAPPING one selected
 * item for one unselected item, which preserves |z| = K exactly. That is both
 * faster to converge and free of the usual failure where a penalty weight is
 * tuned until the solution happens to be feasible, and the tuning is then
 * reported as a result. `test_penalty_never_binds_under_swap_moves` pins it.
 *
 * WHAT THIS IS FOR
 * ----------------
 * ROADMAP P5, and the roadmap is openly sceptical: a smarter slow lane is worth
 * nothing until the advice arrives in time. The thing that makes it worth
 * measuring here rather than anywhere else is that this repository scores
 * against EXTERNAL forward labels, not against the diversity term the objective
 * itself maximises -- and solve time is priced against a freshness curve that
 * was measured, not assumed. See docs/SUBSET.md.
 */

#pragma once

#include "titans/eval/bootstrap.hpp"

#include <chrono>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace titans {
namespace opt {

// ============================================================================
// The problem
// ============================================================================

struct SubsetProblem {
    /// Per-item relevance. Higher is more worth selecting.
    std::vector<double> relevance;
    /// Row-major n x n similarity, symmetric, zero diagonal, in [0, 1].
    std::vector<double> similarity;
    std::size_t k = 0;
    /// Weight on the redundancy term. 0 => top-K by relevance.
    double lambda = 0.0;
    /// Cardinality penalty. Never binds under swap moves; see the file comment.
    double penalty = 4.0;

    std::size_t n() const { return relevance.size(); }
    double sim(std::size_t i, std::size_t j) const { return similarity[i * n() + j]; }
};

/// @brief Dense symmetric QUBO. Linear terms live on the diagonal.
class Qubo {
public:
    explicit Qubo(std::size_t n) : n_(n), q_(n * n, 0.0) {}

    std::size_t n() const { return n_; }
    double& at(std::size_t i, std::size_t j) { return q_[i * n_ + j]; }
    double at(std::size_t i, std::size_t j) const { return q_[i * n_ + j]; }

    /// @brief x^T Q x, with the diagonal contributing the linear part.
    double energy(const std::vector<std::uint8_t>& x) const {
        double e = 0.0;
        for (std::size_t i = 0; i < n_; ++i) {
            if (!x[i]) continue;
            e += at(i, i);
            for (std::size_t j = i + 1; j < n_; ++j) {
                if (x[j]) e += at(i, j) + at(j, i);
            }
        }
        return e;
    }

private:
    std::size_t n_;
    std::vector<double> q_;
};

/**
 * @brief Build the QUBO for a subset problem.
 *
 * Expanding P(sum z - K)^2 gives P(1 - 2K) on each diagonal and 2P on each
 * off-diagonal pair, plus the constant P*K^2 which is dropped -- it shifts
 * every energy by the same amount and changes no comparison.
 */
inline Qubo build_subset_qubo(const SubsetProblem& p) {
    const std::size_t n = p.n();
    Qubo q(n);
    const double kk = static_cast<double>(p.k);
    for (std::size_t i = 0; i < n; ++i) {
        q.at(i, i) = -p.relevance[i] + p.penalty * (1.0 - 2.0 * kk);
        for (std::size_t j = i + 1; j < n; ++j) {
            // Split each pair across the two off-diagonal entries so the matrix
            // stays symmetric and `energy` can read either half.
            const double v = 0.5 * (p.lambda * p.sim(i, j) + 2.0 * p.penalty);
            q.at(i, j) = v;
            q.at(j, i) = v;
        }
    }
    return q;
}

// ============================================================================
// Solutions
// ============================================================================

struct Solution {
    std::vector<std::uint8_t> x;
    double energy = 0.0;
    std::size_t selected = 0;
    std::size_t moves_accepted = 0;
    /// Solve time in microseconds. On the slow lane this is a freshness cost,
    /// so it is a first-class output rather than a footnote.
    double solve_us = 0.0;
    bool valid = false;
    std::string refusal;
};

/// @brief Mean pairwise dissimilarity over the selected items, in [0, 1].
inline double diversity_of(const SubsetProblem& p, const std::vector<std::uint8_t>& x) {
    std::vector<std::size_t> sel;
    for (std::size_t i = 0; i < p.n(); ++i) if (x[i]) sel.push_back(i);
    if (sel.size() < 2) return 0.0;
    double total = 0.0;
    std::size_t pairs = 0;
    for (std::size_t a = 0; a < sel.size(); ++a) {
        for (std::size_t b = a + 1; b < sel.size(); ++b) {
            total += 1.0 - p.sim(sel[a], sel[b]);
            ++pairs;
        }
    }
    return pairs ? total / static_cast<double>(pairs) : 0.0;
}

/// @brief Sum of the selected items' relevance.
inline double relevance_of(const SubsetProblem& p, const std::vector<std::uint8_t>& x) {
    double total = 0.0;
    for (std::size_t i = 0; i < p.n(); ++i) if (x[i]) total += p.relevance[i];
    return total;
}

// ============================================================================
// Solvers
// ============================================================================

/**
 * @brief Exhaustive over the feasible set, not over all 2^n assignments.
 *
 * Enumerating subsets of size exactly K is C(n, K) rather than 2^n, which is
 * what makes an exact reference affordable at the sizes a slow lane would
 * actually use. It is the ground truth every other solver here is checked
 * against, so it refuses rather than truncates when the count would be
 * unreasonable -- an "exact" answer that quietly gave up is worse than none.
 */
inline constexpr double kMaxExactCombinations = 5e7;

inline Solution solve_exact(const SubsetProblem& p) {
    Solution s;
    const std::size_t n = p.n();
    if (p.k == 0 || p.k > n) {
        s.refusal = "k must be in [1, n]";
        return s;
    }
    // C(n, k) without overflow, and refuse before doing any work.
    double combos = 1.0;
    for (std::size_t i = 0; i < p.k; ++i) {
        combos *= static_cast<double>(n - i) / static_cast<double>(i + 1);
        if (combos > kMaxExactCombinations) {
            s.refusal = "C(" + std::to_string(n) + ", " + std::to_string(p.k) +
                        ") exceeds the exact-search budget";
            return s;
        }
    }

    const Qubo q = build_subset_qubo(p);
    std::vector<std::size_t> idx(p.k);
    std::iota(idx.begin(), idx.end(), 0);
    std::vector<std::uint8_t> x(n, 0);
    double best = 0.0;
    bool have = false;

    const auto t0 = std::chrono::steady_clock::now();
    while (true) {
        std::fill(x.begin(), x.end(), 0);
        for (const std::size_t i : idx) x[i] = 1;
        const double e = q.energy(x);
        if (!have || e < best) { have = true; best = e; s.x = x; }

        // Next combination in lexicographic order.
        std::size_t i = p.k;
        while (i > 0 && idx[i - 1] == n - p.k + i - 1) --i;
        if (i == 0) break;
        ++idx[i - 1];
        for (std::size_t j = i; j < p.k; ++j) idx[j] = idx[j - 1] + 1;
    }
    const auto t1 = std::chrono::steady_clock::now();

    s.solve_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    s.energy = best;
    s.selected = p.k;
    s.valid = true;
    return s;
}

/**
 * @brief Greedy maximal-marginal-relevance: take the best marginal gain, K times.
 *
 * The standard baseline, and the one the source formulation reports beside
 * exact and annealing. At lambda = 0 it is exactly top-K by relevance, which is
 * a property worth testing rather than assuming.
 */
inline Solution solve_greedy(const SubsetProblem& p) {
    Solution s;
    const std::size_t n = p.n();
    if (p.k == 0 || p.k > n) {
        s.refusal = "k must be in [1, n]";
        return s;
    }
    const auto t0 = std::chrono::steady_clock::now();

    std::vector<std::uint8_t> x(n, 0);
    for (std::size_t picked = 0; picked < p.k; ++picked) {
        std::size_t best_i = n;
        double best_gain = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            if (x[i]) continue;
            double redundancy = 0.0;
            for (std::size_t j = 0; j < n; ++j) if (x[j]) redundancy += p.sim(i, j);
            const double gain = p.relevance[i] - p.lambda * redundancy;
            if (best_i == n || gain > best_gain) { best_i = i; best_gain = gain; }
        }
        if (best_i == n) break;
        x[best_i] = 1;
    }

    const auto t1 = std::chrono::steady_clock::now();
    s.solve_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    s.x = std::move(x);
    s.energy = build_subset_qubo(p).energy(s.x);
    s.selected = p.k;
    s.valid = true;
    return s;
}

struct AnnealConfig {
    std::size_t sweeps = 200;
    double t_start = 1.0;
    double t_end = 0.01;
    std::uint64_t seed = 42;
};

/**
 * @brief Simulated annealing over SWAPS, so the cardinality never breaks.
 *
 * @param warm  optional starting assignment. Passing the previous window's
 *              solution is the warm start the source formulation leaves open;
 *              `titans_subset` measures whether it buys anything.
 *
 * A swap's energy change is computed from the two rows involved rather than by
 * re-evaluating the objective, so a sweep is O(k) per move rather than O(n^2).
 */
inline Solution solve_annealing(const SubsetProblem& p, AnnealConfig cfg,
                                const std::vector<std::uint8_t>* warm = nullptr) {
    Solution s;
    const std::size_t n = p.n();
    if (p.k == 0 || p.k > n) {
        s.refusal = "k must be in [1, n]";
        return s;
    }
    const Qubo q = build_subset_qubo(p);
    eval::Rng rng(cfg.seed);

    const auto t0 = std::chrono::steady_clock::now();

    std::vector<std::uint8_t> x(n, 0);
    if (warm && warm->size() == n) {
        std::size_t count = 0;
        for (std::size_t i = 0; i < n; ++i) { x[i] = (*warm)[i]; count += x[i]; }
        // A warm start of the wrong size is repaired rather than trusted: a
        // window that slid may have dropped a selected item off the front.
        while (count > p.k) {
            const std::size_t i = static_cast<std::size_t>(rng.below(n));
            if (x[i]) { x[i] = 0; --count; }
        }
        while (count < p.k) {
            const std::size_t i = static_cast<std::size_t>(rng.below(n));
            if (!x[i]) { x[i] = 1; ++count; }
        }
    } else {
        std::size_t count = 0;
        while (count < p.k) {
            const std::size_t i = static_cast<std::size_t>(rng.below(n));
            if (!x[i]) { x[i] = 1; ++count; }
        }
    }

    std::vector<std::size_t> in, out;
    auto rebuild = [&]() {
        in.clear(); out.clear();
        for (std::size_t i = 0; i < n; ++i) (x[i] ? in : out).push_back(i);
    };
    rebuild();

    double energy = q.energy(x);
    std::vector<std::uint8_t> best_x = x;
    double best_e = energy;

    const double log_ratio = std::log(cfg.t_end / cfg.t_start);
    for (std::size_t sweep = 0; sweep < cfg.sweeps; ++sweep) {
        const double frac = cfg.sweeps > 1
            ? static_cast<double>(sweep) / static_cast<double>(cfg.sweeps - 1) : 1.0;
        const double temp = cfg.t_start * std::exp(log_ratio * frac);

        for (std::size_t attempt = 0; attempt < p.k; ++attempt) {
            if (in.empty() || out.empty()) break;
            const std::size_t a = static_cast<std::size_t>(rng.below(in.size()));
            const std::size_t b = static_cast<std::size_t>(rng.below(out.size()));
            const std::size_t drop = in[a], add = out[b];

            // Energy change from removing `drop` and adding `add`. Both rows
            // exclude each other and themselves.
            double d = q.at(add, add) - q.at(drop, drop);
            for (const std::size_t j : in) {
                if (j == drop) continue;
                d += q.at(add, j) + q.at(j, add);
                d -= q.at(drop, j) + q.at(j, drop);
            }

            if (d <= 0.0 ||
                static_cast<double>(rng.below(1000000)) / 1e6 < std::exp(-d / temp)) {
                x[drop] = 0; x[add] = 1;
                in[a] = add; out[b] = drop;
                energy += d;
                ++s.moves_accepted;
                if (energy < best_e) { best_e = energy; best_x = x; }
            }
        }
    }

    const auto t1 = std::chrono::steady_clock::now();
    s.solve_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    s.x = std::move(best_x);
    s.energy = best_e;
    s.selected = p.k;
    s.valid = true;
    return s;
}

/// @brief The K most recent items, when items are in time order. The baseline
///        the slow lane already uses, and the one selection has to beat.
inline Solution select_recent(const SubsetProblem& p) {
    Solution s;
    if (p.k == 0 || p.k > p.n()) { s.refusal = "k must be in [1, n]"; return s; }
    s.x.assign(p.n(), 0);
    for (std::size_t i = p.n() - p.k; i < p.n(); ++i) s.x[i] = 1;
    s.energy = build_subset_qubo(p).energy(s.x);
    s.selected = p.k;
    s.valid = true;
    return s;
}

/// @brief K items chosen uniformly. The control that says whether any of the
///        others are doing anything at all.
inline Solution select_random(const SubsetProblem& p, std::uint64_t seed) {
    Solution s;
    if (p.k == 0 || p.k > p.n()) { s.refusal = "k must be in [1, n]"; return s; }
    eval::Rng rng(seed);
    s.x.assign(p.n(), 0);
    std::size_t count = 0;
    while (count < p.k) {
        const std::size_t i = static_cast<std::size_t>(rng.below(p.n()));
        if (!s.x[i]) { s.x[i] = 1; ++count; }
    }
    s.energy = build_subset_qubo(p).energy(s.x);
    s.selected = p.k;
    s.valid = true;
    return s;
}

}  // namespace opt
}  // namespace titans
