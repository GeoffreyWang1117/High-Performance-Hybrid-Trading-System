/**
 * @file test_qubo.cpp
 * @brief The subset-selection objective, and the properties a frontier needs.
 *
 * A QUBO is easy to write down and easy to write down wrongly. The failure mode
 * is not a crash: it is a matrix whose energy disagrees with the formulation
 * everyone thinks is being solved, after which every lambda sweep and every
 * solver comparison is internally consistent and about something else.
 *
 * So the anchor test computes the published objective directly and requires the
 * matrix to reproduce it, and the solver tests are all against exhaustive
 * search rather than against each other.
 */

#include "titans/opt/qubo.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <vector>

using namespace titans::opt;
using titans::eval::Rng;

namespace {

/// @brief A reproducible instance with structure, not uniform noise: items
///        near each other in the index are similar, as they are on a tape.
SubsetProblem make_problem(std::size_t n, std::size_t k, double lambda,
                           std::uint64_t seed) {
    SubsetProblem p;
    p.k = k;
    p.lambda = lambda;
    p.relevance.resize(n);
    p.similarity.assign(n * n, 0.0);
    Rng rng(seed);
    std::vector<double> feat(n);
    for (std::size_t i = 0; i < n; ++i) {
        p.relevance[i] = static_cast<double>(rng.below(1000)) / 1000.0;
        feat[i] = static_cast<double>(i) / static_cast<double>(n - 1);
    }
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            const double d = feat[i] - feat[j];
            const double s = std::exp(-(d * d) / (2.0 * 0.25 * 0.25));
            p.similarity[i * n + j] = s;
            p.similarity[j * n + i] = s;
        }
    }
    return p;
}

/// @brief The published objective, written out, with no matrix involved.
double objective_directly(const SubsetProblem& p, const std::vector<std::uint8_t>& x) {
    double lin = 0.0, quad = 0.0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < p.n(); ++i) {
        if (!x[i]) continue;
        ++count;
        lin += p.relevance[i];
        for (std::size_t j = i + 1; j < p.n(); ++j) {
            if (x[j]) quad += p.sim(i, j);
        }
    }
    const double c = static_cast<double>(count) - static_cast<double>(p.k);
    return -lin + p.lambda * quad + p.penalty * c * c;
}

// ============================================================================
// The anchor
// ============================================================================

/**
 * @brief The matrix must reproduce the formulation, up to the dropped constant.
 *
 * `build_subset_qubo` drops the P*K^2 term because it shifts every energy
 * equally. That is fine and it is also exactly the kind of thing that turns
 * into a silent sign error, so the offset is asserted rather than assumed --
 * including on INFEASIBLE assignments, where the penalty is the only thing
 * keeping the objective honest.
 */
bool test_matrix_reproduces_the_written_objective() {
    Rng rng(9);
    for (int trial = 0; trial < 50; ++trial) {
        const std::size_t n = 8 + static_cast<std::size_t>(rng.below(8));
        const std::size_t k = 2 + static_cast<std::size_t>(rng.below(n - 3));
        const double lambda = static_cast<double>(rng.below(300)) / 100.0;
        const SubsetProblem p = make_problem(n, k, lambda, 100 + trial);
        const Qubo q = build_subset_qubo(p);
        const double offset = p.penalty * static_cast<double>(k) * static_cast<double>(k);

        // Random assignments, feasible and not.
        for (int rep = 0; rep < 20; ++rep) {
            std::vector<std::uint8_t> x(n, 0);
            for (std::size_t i = 0; i < n; ++i) x[i] = rng.below(2) ? 1 : 0;
            const double from_matrix = q.energy(x) + offset;
            const double direct = objective_directly(p, x);
            if (std::fabs(from_matrix - direct) > 1e-9 * std::max(1.0, std::fabs(direct))) {
                std::fprintf(stderr,
                    "FAIL: matrix says %.9f, the formulation says %.9f"
                    " (n=%zu k=%zu lambda=%.2f)\n",
                    from_matrix, direct, n, k, lambda);
                return false;
            }
        }
    }
    return true;
}

/**
 * @brief Every feasible assignment carries the same penalty, so it cannot rank.
 *
 * This is what licenses the swap-move annealer to ignore the penalty entirely.
 * If it ever stopped being true, the annealer would be optimising a different
 * objective from the one `energy()` reports and the two would disagree only in
 * the third decimal, which is the hardest kind of disagreement to notice.
 */
bool test_penalty_never_binds_under_swap_moves() {
    const SubsetProblem p = make_problem(14, 5, 0.7, 21);
    const Qubo q = build_subset_qubo(p);
    const double offset = p.penalty * 25.0;

    double reference = 0.0;
    bool have = false;
    Rng rng(31);
    for (int rep = 0; rep < 200; ++rep) {
        std::vector<std::uint8_t> x(p.n(), 0);
        std::size_t count = 0;
        while (count < p.k) {
            const std::size_t i = static_cast<std::size_t>(rng.below(p.n()));
            if (!x[i]) { x[i] = 1; ++count; }
        }
        // Penalty contribution = energy - (the parts that are not the penalty).
        double lin = 0.0, quad = 0.0;
        for (std::size_t i = 0; i < p.n(); ++i) {
            if (!x[i]) continue;
            lin += p.relevance[i];
            for (std::size_t j = i + 1; j < p.n(); ++j) if (x[j]) quad += p.sim(i, j);
        }
        const double penalty_part = q.energy(x) - (-lin + p.lambda * quad);
        if (!have) { reference = penalty_part; have = true; }
        else if (std::fabs(penalty_part - reference) > 1e-9) {
            std::fprintf(stderr,
                "FAIL: the penalty differs between two feasible assignments"
                " (%.9f vs %.9f)\n", penalty_part, reference);
            return false;
        }
    }
    if (std::fabs(reference + offset) > 1e-9) {
        std::fprintf(stderr, "FAIL: penalty on a feasible point is %.9f, expected %.9f\n",
                     reference, -offset);
        return false;
    }
    return true;
}

// ============================================================================
// Solvers, all against exhaustive search
// ============================================================================

bool test_annealing_matches_exhaustive_search() {
    int matched = 0, total = 0;
    double worst_gap = 0.0;
    for (int trial = 0; trial < 30; ++trial) {
        const SubsetProblem p = make_problem(18, 6, 0.1 * (trial % 5), 400 + trial);
        const Solution ex = solve_exact(p);
        if (!ex.valid) {
            std::fprintf(stderr, "FAIL: exact refused a 18-choose-6 instance: %s\n",
                         ex.refusal.c_str());
            return false;
        }
        AnnealConfig cfg;
        cfg.sweeps = 300;
        cfg.seed = 900 + trial;
        const Solution sa = solve_annealing(p, cfg);
        ++total;
        if (sa.energy <= ex.energy + 1e-9) ++matched;
        // A solver that beats exhaustive search is not a better solver; it is
        // an energy function that disagrees with itself.
        if (sa.energy < ex.energy - 1e-9) {
            std::fprintf(stderr,
                "FAIL: annealing found %.9f, below the exhaustive optimum %.9f\n",
                sa.energy, ex.energy);
            return false;
        }
        worst_gap = std::max(worst_gap, (sa.energy - ex.energy) / std::fabs(ex.energy));
        if (sa.selected != p.k) {
            std::fprintf(stderr, "FAIL: annealing returned %zu items, wanted %zu\n",
                         sa.selected, p.k);
            return false;
        }
    }
    if (matched < total * 3 / 4) {
        std::fprintf(stderr, "FAIL: annealing matched the optimum on %d of %d\n",
                     matched, total);
        return false;
    }
    if (worst_gap > 0.01) {
        std::fprintf(stderr, "FAIL: worst optimality gap %.4f\n", worst_gap);
        return false;
    }
    return true;
}

bool test_greedy_never_beats_exhaustive_search() {
    for (int trial = 0; trial < 30; ++trial) {
        const SubsetProblem p = make_problem(16, 5, 0.2 * (trial % 6), 700 + trial);
        const Solution ex = solve_exact(p);
        const Solution gr = solve_greedy(p);
        if (!ex.valid || !gr.valid) {
            std::fprintf(stderr, "FAIL: a solver refused a small instance\n");
            return false;
        }
        if (gr.energy < ex.energy - 1e-9) {
            std::fprintf(stderr,
                "FAIL: greedy found %.9f below the exhaustive optimum %.9f\n",
                gr.energy, ex.energy);
            return false;
        }
        if (gr.selected != p.k) {
            std::fprintf(stderr, "FAIL: greedy returned %zu items\n", gr.selected);
            return false;
        }
    }
    return true;
}

/// @brief The left endpoint of the frontier is top-K by relevance, exactly.
bool test_lambda_zero_is_top_k_by_relevance() {
    for (int trial = 0; trial < 20; ++trial) {
        SubsetProblem p = make_problem(20, 7, 0.0, 1300 + trial);
        const Solution gr = solve_greedy(p);

        std::vector<std::size_t> order(p.n());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(),
                         [&](std::size_t a, std::size_t b) {
                             return p.relevance[a] > p.relevance[b];
                         });
        std::vector<std::uint8_t> want(p.n(), 0);
        for (std::size_t i = 0; i < p.k; ++i) want[order[i]] = 1;

        if (gr.x != want) {
            std::fprintf(stderr,
                "FAIL: at lambda = 0 greedy did not select the top %zu by relevance\n",
                p.k);
            return false;
        }
    }
    return true;
}

/**
 * @brief Raising lambda must not reduce diversity. That is the frontier.
 *
 * Checked on EXACT solutions, so this is a property of the objective rather
 * than of a heuristic that happens to behave.
 */
bool test_diversity_is_monotone_in_lambda() {
    const double lambdas[] = {0.0, 0.25, 0.5, 1.0, 2.0, 4.0};
    for (int trial = 0; trial < 12; ++trial) {
        double prev = -1.0;
        for (const double lam : lambdas) {
            const SubsetProblem p = make_problem(16, 6, lam, 2100 + trial);
            const Solution ex = solve_exact(p);
            if (!ex.valid) return false;
            const double d = diversity_of(p, ex.x);
            if (d < prev - 1e-9) {
                std::fprintf(stderr,
                    "FAIL: diversity fell from %.6f to %.6f when lambda rose to %.2f\n",
                    prev, d, lam);
                return false;
            }
            prev = d;
        }
    }
    return true;
}

// ============================================================================
// Warm starting, refusals, determinism
// ============================================================================

/// @brief Started from the optimum, the annealer must return the optimum.
bool test_warm_start_from_the_optimum_keeps_it() {
    for (int trial = 0; trial < 20; ++trial) {
        const SubsetProblem p = make_problem(18, 6, 0.5, 3300 + trial);
        const Solution ex = solve_exact(p);
        if (!ex.valid) return false;
        AnnealConfig cfg;
        cfg.sweeps = 200;
        cfg.seed = 4400 + trial;
        const Solution warm = solve_annealing(p, cfg, &ex.x);
        if (warm.energy > ex.energy + 1e-9) {
            std::fprintf(stderr,
                "FAIL: started at the optimum %.9f and returned %.9f;\n"
                "      the best-seen solution is not being kept\n",
                ex.energy, warm.energy);
            return false;
        }
    }
    return true;
}

/// @brief A warm start of the wrong size is repaired, not trusted.
bool test_warm_start_of_the_wrong_size_is_repaired() {
    const SubsetProblem p = make_problem(20, 6, 0.5, 5500);
    AnnealConfig cfg;
    cfg.sweeps = 50;
    cfg.seed = 77;
    for (const std::size_t seeded : {std::size_t{0}, std::size_t{3}, std::size_t{15}}) {
        std::vector<std::uint8_t> start(p.n(), 0);
        for (std::size_t i = 0; i < seeded; ++i) start[i] = 1;
        const Solution s = solve_annealing(p, cfg, &start);
        std::size_t count = 0;
        for (const auto v : s.x) count += v;
        if (count != p.k) {
            std::fprintf(stderr,
                "FAIL: a warm start with %zu items produced %zu, wanted %zu\n",
                seeded, count, p.k);
            return false;
        }
    }
    return true;
}

/// @brief An exhaustive search too large to run is refused, not truncated.
bool test_exact_refuses_an_unreasonable_search() {
    const SubsetProblem p = make_problem(200, 40, 0.5, 6600);
    const Solution s = solve_exact(p);
    if (s.valid) {
        std::fprintf(stderr, "FAIL: exact claimed to enumerate C(200, 40)\n");
        return false;
    }
    if (s.refusal.empty()) {
        std::fprintf(stderr, "FAIL: the refusal carried no reason\n");
        return false;
    }
    return true;
}

bool test_same_seed_same_solution() {
    const SubsetProblem p = make_problem(40, 12, 0.8, 7700);
    AnnealConfig cfg;
    cfg.sweeps = 150;
    cfg.seed = 8800;
    const Solution a = solve_annealing(p, cfg);
    const Solution b = solve_annealing(p, cfg);
    if (a.x != b.x || std::fabs(a.energy - b.energy) > 1e-12) {
        std::fprintf(stderr, "FAIL: the same seed gave two different answers\n");
        return false;
    }
    return true;
}

struct Case {
    const char* name;
    bool (*fn)();
};

const Case kCases[] = {
    {"the matrix reproduces the written objective", test_matrix_reproduces_the_written_objective},
    {"the penalty never binds under swap moves", test_penalty_never_binds_under_swap_moves},
    {"annealing matches exhaustive search", test_annealing_matches_exhaustive_search},
    {"greedy never beats exhaustive search", test_greedy_never_beats_exhaustive_search},
    {"lambda zero is top-k by relevance", test_lambda_zero_is_top_k_by_relevance},
    {"diversity is monotone in lambda", test_diversity_is_monotone_in_lambda},
    {"a warm start from the optimum keeps it", test_warm_start_from_the_optimum_keeps_it},
    {"a warm start of the wrong size is repaired", test_warm_start_of_the_wrong_size_is_repaired},
    {"an unreasonable exhaustive search is refused", test_exact_refuses_an_unreasonable_search},
    {"same seed, same solution", test_same_seed_same_solution},
};

}  // namespace

bool run_qubo_tests() {
    bool all = true;
    for (const auto& c : kCases) {
        const bool ok = c.fn();
        std::printf("  [%s] %s\n", ok ? "pass" : "FAIL", c.name);
        if (!ok) all = false;
    }
    return all;
}
