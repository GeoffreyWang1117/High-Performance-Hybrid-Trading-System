/**
 * @file latency_budget.hpp
 * @brief Declared, enforced, and measured latency budgets per pipeline stage.
 *
 * A "low-latency system" that does not state its budget cannot be said to meet
 * one. Each stage on the fast path declares a budget in nanoseconds; the stage
 * records its own cost with the same rdtsc timing the benchmarks use, and
 * counts the times it exceeded budget. Violations are surfaced, never averaged
 * away -- a p99 that hides 1000 breaches per second is not a passing grade.
 *
 * Overhead: two rdtsc reads (~20 ns on a 5950X) per guarded stage. That is
 * material relative to a 1 ns queue push, so budget tracking is compiled out
 * entirely in TITANS_FASTPATH_NO_INSTRUMENTATION builds and the shipped fast
 * path guards whole stages, never individual primitives.
 */

#pragma once

#include "titans/bench/cycle_timer.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace titans {
namespace lanes {

/**
 * @brief One stage's declared budget and observed behaviour.
 *
 * Counters are relaxed atomics: they are read from a reporting thread but must
 * not introduce ordering costs on the fast path.
 */
class LatencyBudget {
public:
    LatencyBudget(std::string stage_name, uint64_t budget_ns)
        : name_(std::move(stage_name)), budget_ns_(budget_ns) {}

    const std::string& name() const { return name_; }
    uint64_t budget_ns() const { return budget_ns_; }

    void record_ns(uint64_t observed_ns) {
        count_.fetch_add(1, std::memory_order_relaxed);
        total_ns_.fetch_add(observed_ns, std::memory_order_relaxed);

        uint64_t prev_max = max_ns_.load(std::memory_order_relaxed);
        while (observed_ns > prev_max &&
               !max_ns_.compare_exchange_weak(prev_max, observed_ns,
                                              std::memory_order_relaxed)) {
        }
        if (observed_ns > budget_ns_) {
            violations_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    uint64_t count() const { return count_.load(std::memory_order_relaxed); }
    uint64_t violations() const { return violations_.load(std::memory_order_relaxed); }
    uint64_t max_ns() const { return max_ns_.load(std::memory_order_relaxed); }

    double mean_ns() const {
        const uint64_t n = count();
        return n ? static_cast<double>(total_ns_.load(std::memory_order_relaxed)) / n : 0.0;
    }

    double violation_rate() const {
        const uint64_t n = count();
        return n ? static_cast<double>(violations()) / n : 0.0;
    }

    /// @brief A stage passes only if it never exceeded its declared budget.
    bool passed() const { return violations() == 0; }

    void reset() {
        count_.store(0, std::memory_order_relaxed);
        violations_.store(0, std::memory_order_relaxed);
        total_ns_.store(0, std::memory_order_relaxed);
        max_ns_.store(0, std::memory_order_relaxed);
    }

private:
    std::string name_;
    uint64_t budget_ns_;
    std::atomic<uint64_t> count_{0};
    std::atomic<uint64_t> violations_{0};
    std::atomic<uint64_t> total_ns_{0};
    std::atomic<uint64_t> max_ns_{0};
};

/**
 * @brief RAII scope that times its lifetime and reports it to a budget.
 *
 * @warning Costs two rdtsc reads. Guard stages, not primitives.
 */
class BudgetScope {
public:
    BudgetScope(LatencyBudget& budget, double ticks_per_ns)
        : budget_(budget), ticks_per_ns_(ticks_per_ns),
          start_(bench::rdtsc_start()) {}

    ~BudgetScope() {
        const uint64_t elapsed = bench::rdtsc_end() - start_;
        budget_.record_ns(static_cast<uint64_t>(elapsed / ticks_per_ns_));
    }

    BudgetScope(const BudgetScope&) = delete;
    BudgetScope& operator=(const BudgetScope&) = delete;

private:
    LatencyBudget& budget_;
    double ticks_per_ns_;
    uint64_t start_;
};

/**
 * @brief The full set of budgets for a pipeline, and a pass/fail verdict.
 */
class BudgetReport {
public:
    void add(LatencyBudget* b) { budgets_.push_back(b); }

    /// @brief True only if every stage stayed inside its declared budget.
    bool all_passed() const {
        for (const auto* b : budgets_) {
            if (!b->passed()) return false;
        }
        return true;
    }

    void print() const {
        std::printf("\n%-34s %10s %10s %10s %12s %10s\n",
                    "Stage", "budget", "mean", "max", "violations", "verdict");
        std::printf("%s\n", std::string(92, '-').c_str());
        for (const auto* b : budgets_) {
            std::printf("%-34s %8lu ns %8.1f ns %8lu ns %6lu (%4.2f%%) %10s\n",
                        b->name().c_str(), b->budget_ns(), b->mean_ns(), b->max_ns(),
                        b->violations(), b->violation_rate() * 100.0,
                        b->passed() ? "PASS" : "BREACH");
        }
    }

    const std::vector<LatencyBudget*>& budgets() const { return budgets_; }

private:
    std::vector<LatencyBudget*> budgets_;
};

}  // namespace lanes
}  // namespace titans
