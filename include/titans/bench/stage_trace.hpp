/**
 * @file stage_trace.hpp
 * @brief Per-stage and end-to-end latency for one pass down the fast path.
 *
 * WHY NOT NESTED BudgetScope
 * --------------------------
 * `lanes/latency_budget.hpp` already times a stage with an RAII scope, and the
 * obvious way to get per-stage attribution is to nest five of them. Two reasons
 * that is the wrong instrument here:
 *
 *   1. COST. A scope reads the TSC twice, so five scopes read it ten times. On
 *      this class of machine an ordered read pair costs ~20 ns, which is ~200 ns
 *      of probe on a path that may cost 300 ns. The measurement would dominate
 *      the measurement. A chain of boundary stamps needs N+1 reads for N stages
 *      -- six instead of ten -- and each is a single read, not a pair.
 *
 *   2. GAPS. Scopes measure disjoint intervals with unmeasured space between
 *      them, so the stages sum to LESS than the end-to-end figure and the
 *      difference is silently unattributed. Nothing in the output says so. A
 *      stamp chain partitions the interval exactly: stage i is
 *      `stamps[i+1] - stamps[i]`, and the stages sum to `stamps[N] - stamps[0]`
 *      by construction. `test_pipeline_latency.cpp` pins that equality, because
 *      an attribution table that loses time is worse than no table.
 *
 * THREE END-TO-END NUMBERS
 * ------------------------
 * They disagree, and the disagreement is the point.
 *
 *   service   last stamp - first stamp. The handler's own work. This is what a
 *             closed-loop benchmark reports, and a stall cannot move it: while
 *             the system is frozen it takes no samples, so the samples that
 *             would have been slow are simply missing. Gil Tene's coordinated
 *             omission, in one line.
 *   response  last stamp - the moment the tick was DUE. Includes the queueing
 *             that the stall caused. Exact, because a replay knows each tick's
 *             own scheduled arrival; no assumption about the arrival process is
 *             needed.
 *   corrected `service` fed through `Histogram::record_corrected`, which
 *             synthesises the swallowed samples from an assumed uniform
 *             interval. This is what a system that does NOT know its own
 *             arrival times has to do instead.
 *
 * Reporting `corrected` beside `response` on real, irregular arrivals says how
 * good that substitute actually is. Asserting it in a test with uniform
 * arrivals says only that the arithmetic is right.
 *
 * TICKS, NOT NANOSECONDS
 * ----------------------
 * Everything here is raw TSC ticks; conversion happens once, at print time.
 * A division per stage per tick would be probe added to measure probe.
 *
 * TSC SKEW
 * --------
 * Every stamp in a trace is taken by one thread, which is pinned. The known
 * failure -- comparing timestamps taken on different cores whose TSCs are
 * offset -- cannot arise from a chain that never crosses a core.
 */

#pragma once

#include "titans/bench/cycle_timer.hpp"
#include "titans/bench/histogram.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace titans {
namespace bench {

// ============================================================================
// Stages
// ============================================================================

/**
 * @brief The boundaries a tick crosses on its way to becoming an order.
 *
 * The names are the ones `docs/ROADMAP.md` P1 uses. Each is a real call into
 * production code, not a placeholder: see `src/tools/pipeline_main.cpp`.
 */
enum class Stage : unsigned {
    Ingest = 0,   ///< wire representation -> internal event, fixed-point conversion
    Book,         ///< order book update and top-of-book read
    Signal,       ///< advisory read + policy evaluation (the strategy logic)
    Risk,         ///< pre-trade risk check
    Order,        ///< order construction and hand-off to the outbound queue
};

inline constexpr unsigned kStageCount = 5;

inline const char* stage_name(unsigned i) {
    switch (i) {
        case 0: return "ingest";
        case 1: return "book";
        case 2: return "signal";
        case 3: return "risk";
        case 4: return "order";
        default: return "?";
    }
}

// ============================================================================
// StageTrace
// ============================================================================

/**
 * @brief One tick's boundary stamps.
 *
 * @tparam Instrumented When false every method is empty and the object holds no
 *         stamps, so the same source compiles to a fast path with no probe at
 *         all. Both instantiations are built and the tool selects between them
 *         at the top of the replay loop, so neither carries a per-tick branch --
 *         which is what makes the probe cost measurable by difference rather
 *         than assumed. See `docs/LATENCY.md`.
 */
template <bool Instrumented>
struct StageTrace {
    static constexpr bool instrumented = Instrumented;

    /// When this tick was scheduled to arrive, in TSC ticks.
    uint64_t due = 0;
    /// kStageCount + 1 boundaries: entry, then one per completed stage.
    std::array<uint64_t, kStageCount + 1> stamps{};

    /**
     * @brief Open the trace. @p due_ticks is when the tick was SUPPOSED to
     *        arrive, which is what separates `response` from `service`.
     */
    inline void open(uint64_t due_ticks) {
        if constexpr (Instrumented) {
            due = due_ticks;
            stamps[0] = rdtsc_start();
        } else {
            (void)due_ticks;
        }
    }

    /// @brief Close stage @p s. Must be called in order, once per stage.
    inline void mark(Stage s) {
        if constexpr (Instrumented) {
            stamps[static_cast<unsigned>(s) + 1] = rdtsc_end();
        } else {
            (void)s;
        }
    }

    uint64_t duration(unsigned stage) const {
        if constexpr (Instrumented) {
            return stamps[stage + 1] - stamps[stage];
        } else {
            (void)stage;
            return 0;
        }
    }

    /// @brief Handler work only. The number a stall cannot move.
    uint64_t service() const {
        if constexpr (Instrumented) {
            return stamps[kStageCount] - stamps[0];
        } else {
            return 0;
        }
    }

    /**
     * @brief From when the tick was due to when the order left. The number a
     *        stall does move, and the one a client would experience.
     */
    uint64_t response() const {
        if constexpr (Instrumented) {
            const uint64_t out = stamps[kStageCount];
            return out > due ? out - due : 0;
        } else {
            return 0;
        }
    }

    /// @brief Time spent waiting for the handler to reach a tick already due.
    uint64_t queueing() const {
        if constexpr (Instrumented) {
            return stamps[0] > due ? stamps[0] - due : 0;
        } else {
            return 0;
        }
    }
};

// ============================================================================
// Aggregation
// ============================================================================

/**
 * @brief Per-stage and end-to-end distributions over a whole run.
 *
 * All in TSC ticks. Folding is off the measured region: the trace is closed
 * before anything here is called.
 */
class PipelineHistograms {
public:
    /**
     * @brief Accumulate one trace.
     *
     * @param expected_interval_ticks The interval at which ticks were SUPPOSED
     *        to arrive. Drives the coordinated-omission correction; pass 0 to
     *        disable it, in which case `corrected()` equals `service()`.
     */
    void fold(const StageTrace<true>& t, uint64_t expected_interval_ticks) {
        for (unsigned i = 0; i < kStageCount; ++i) {
            stage_[i].record(t.duration(i));
        }
        const uint64_t svc = t.service();
        service_.record(svc);
        corrected_.record_corrected(svc, expected_interval_ticks);
        response_.record(t.response());
        queueing_.record(t.queueing());
    }

    const Histogram& stage(unsigned i) const { return stage_[i]; }
    const Histogram& service() const { return service_; }
    const Histogram& response() const { return response_; }
    const Histogram& corrected() const { return corrected_; }
    const Histogram& queueing() const { return queueing_; }

    uint64_t count() const { return service_.count(); }

    /**
     * @brief Sum of the per-stage means.
     *
     * Equals the mean of `service` exactly, because the stamp chain partitions
     * the interval. Printed beside it as a self-check: a mismatch means a stage
     * was left unstamped or stamped out of order.
     */
    double stage_mean_sum() const {
        double s = 0.0;
        for (unsigned i = 0; i < kStageCount; ++i) s += stage_[i].mean();
        return s;
    }

private:
    Histogram stage_[kStageCount];
    Histogram service_;
    Histogram response_;
    Histogram corrected_;
    Histogram queueing_;
};

// ============================================================================
// Reporting
// ============================================================================

/**
 * @brief Converts ticks to nanoseconds and refuses figures the clock cannot see.
 *
 * The benchmark table in the README prints no p99 for single operations,
 * because those operations cost less than the clock read used to measure them
 * and a distribution of noise is not a distribution. The same rule has to hold
 * here or the per-stage table becomes the exact thing that rule exists to
 * prevent: `ingest` is a few loads, and it will come back below the floor.
 */
class LatencyFormatter {
public:
    LatencyFormatter(double ticks_per_ns, double noise_floor_ns)
        : ticks_per_ns_(ticks_per_ns), floor_ns_(noise_floor_ns) {}

    double to_ns(uint64_t ticks) const {
        return static_cast<double>(ticks) / ticks_per_ns_;
    }

    /// @brief The threshold a single-shot figure must clear to be reported.
    double resolvable_above_ns() const { return 3.0 * floor_ns_; }
    bool is_resolvable(double ns) const { return ns > resolvable_above_ns(); }

    /**
     * @brief A percentile as a printable cell, or a refusal.
     *
     * The refusal is deliberately not a number. A "<60" would be read as a
     * measurement, and it is not one: below the floor the histogram is
     * recording the probe.
     */
    std::string cell(const Histogram& h, double p) const {
        const double ns = to_ns(h.percentile(p));
        if (!is_resolvable(ns)) return "  under floor";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%10.1f ns", ns);
        return std::string(buf);
    }

    std::string mean_cell(const Histogram& h) const {
        const double ns = h.mean() / ticks_per_ns_;
        if (!is_resolvable(ns)) return "  under floor";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%10.1f ns", ns);
        return std::string(buf);
    }

private:
    double ticks_per_ns_;
    double floor_ns_;
};

}  // namespace bench
}  // namespace titans
