/**
 * @file test_pipeline_latency.cpp
 * @brief The properties a latency table is worthless without.
 *
 * Each of these pins something whose failure produces output that looks
 * completely normal:
 *
 *   - A stamp chain must PARTITION the interval it measures. If the stages do
 *     not sum to the end-to-end figure, the share column is a fiction and the
 *     missing time is attributed to nothing. Nested scopes fail this by
 *     construction, which is why they were not used.
 *   - The coordinated-omission correction must MOVE a percentile that a stall
 *     would otherwise hide. A correction that is silently a no-op still
 *     compiles, still runs, and still prints a reassuring p99.
 *   - The correction must land near the exact answer when the exact answer is
 *     computable. Asserting only that it moved would pass for a correction that
 *     moved to the wrong place.
 *   - A figure below the clock's resolution must be REFUSED, not printed. The
 *     first two stages of the real pipeline are under the floor, so this is the
 *     difference between a table that says so and a table that invents numbers
 *     for them.
 */

#include "titans/bench/histogram.hpp"
#include "titans/bench/stage_trace.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using titans::bench::Histogram;
using titans::bench::LatencyFormatter;
using titans::bench::PipelineHistograms;
using titans::bench::Stage;
using titans::bench::StageTrace;
using titans::bench::kStageCount;

namespace {

// ============================================================================
// A closed pipeline with one stall, simulated exactly.
// ============================================================================

/**
 * @brief One tick's worth of a deterministic queueing simulation.
 *
 * Ticks are due on a uniform grid. The handler takes `service` on each, except
 * one tick where it takes far longer; every tick behind that one waits. Because
 * the schedule is known, the response time is exact -- which is what makes it a
 * yardstick for the correction rather than another estimate of it.
 */
struct Sim {
    std::vector<uint64_t> service;
    std::vector<uint64_t> response;
    uint64_t interval = 0;
};

Sim simulate_stall(size_t n, uint64_t interval, uint64_t service_ns,
                   size_t stall_at, uint64_t stall_ns) {
    Sim s;
    s.interval = interval;
    s.service.reserve(n);
    s.response.reserve(n);

    uint64_t finish = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t due = static_cast<uint64_t>(i) * interval;
        const uint64_t cost = (i == stall_at) ? stall_ns : service_ns;
        const uint64_t start = (finish > due) ? finish : due;
        finish = start + cost;
        s.service.push_back(cost);
        s.response.push_back(finish - due);
    }
    return s;
}

// ============================================================================
// 1. The property ROADMAP P1 names.
// ============================================================================

/**
 * A stall must move the corrected percentile and must NOT move the raw one.
 *
 * The second half is the half that matters. A benchmark that reports the raw
 * number is not merely imprecise: it gets *better* as the system gets worse,
 * because a frozen system takes no samples and the samples it fails to take are
 * exactly the slow ones.
 */
bool test_stall_moves_corrected_p99_and_not_raw() {
    const Sim sim = simulate_stall(1000, 1000, 10, 500, 100000);

    Histogram raw, corrected;
    for (uint64_t v : sim.service) {
        raw.record(v);
        corrected.record_corrected(v, sim.interval);
    }

    const uint64_t raw99 = raw.percentile(99);
    const uint64_t cor99 = corrected.percentile(99);

    if (raw99 > 100) {
        std::fprintf(stderr,
            "FAIL: raw p99 is %llu; one stall in a thousand must not move it\n",
            static_cast<unsigned long long>(raw99));
        return false;
    }
    if (cor99 < 10000) {
        std::fprintf(stderr,
            "FAIL: corrected p99 is %llu; the correction did not fire\n",
            static_cast<unsigned long long>(cor99));
        return false;
    }
    if (corrected.count() <= raw.count()) {
        std::fprintf(stderr,
            "FAIL: correction synthesised no samples (%llu vs %llu)\n",
            static_cast<unsigned long long>(corrected.count()),
            static_cast<unsigned long long>(raw.count()));
        return false;
    }
    return true;
}

// ============================================================================
// 2. The correction has to land somewhere, not just move.
// ============================================================================

/**
 * With uniform arrivals the exact answer is computable, so the estimate can be
 * scored rather than trusted. This is what licenses reading `corrected` at all;
 * `titans_ticktotrade` then reports how far it drifts once arrivals are real
 * and bursty, which is a different and larger number.
 */
bool test_correction_approximates_the_exact_response_time() {
    const Sim sim = simulate_stall(1000, 1000, 10, 500, 100000);

    Histogram corrected, response;
    for (uint64_t v : sim.service) corrected.record_corrected(v, sim.interval);
    for (uint64_t v : sim.response) response.record(v);

    const double c = static_cast<double>(corrected.percentile(99));
    const double r = static_cast<double>(response.percentile(99));
    const double err = std::fabs(c - r) / r;

    if (err > 0.05) {
        std::fprintf(stderr,
            "FAIL: corrected p99 %.0f vs exact response p99 %.0f (%.1f%% off);\n"
            "      on uniform arrivals the correction should be close\n",
            c, r, err * 100.0);
        return false;
    }
    return true;
}

// ============================================================================
// 3. The chain partitions. This is why it is a chain.
// ============================================================================

/**
 * @brief A chain loses no time; a scope per stage does.
 *
 * This is the design claim stated as an experiment rather than an assertion.
 * The same five pieces of work are timed two ways:
 *
 *   chain   one stamp per boundary. Stage i is stamps[i+1] - stamps[i], so the
 *           stages telescope to the whole interval exactly.
 *   scopes  a start/end pair around each stage, the way BudgetScope does it.
 *           The space between one scope's end and the next scope's start --
 *           the loop, the branch, the second clock read -- belongs to no stage.
 *
 * The scope version's stages therefore sum to LESS than its own end-to-end
 * figure, and nothing in a report built on it would say where the difference
 * went. The chain's sum is exact.
 *
 * The test also requires the stamps to be strictly increasing, which is what
 * catches a boundary that was never marked: a missing stamp still telescopes,
 * so the sum alone would not notice.
 */
bool test_a_chain_partitions_and_scopes_lose_time() {
    volatile uint64_t sink = 0;
    auto work = [&sink](unsigned stage) {
        for (int k = 0; k < 200 * static_cast<int>(stage + 1); ++k) sink += k;
    };

    uint64_t scope_gap_total = 0;
    for (int rep = 0; rep < 200; ++rep) {
        // -- the chain ---------------------------------------------------
        StageTrace<true> tr;
        tr.open(0);
        for (unsigned st = 0; st < kStageCount; ++st) {
            work(st);
            tr.mark(static_cast<Stage>(st));
        }

        for (unsigned st = 0; st < kStageCount; ++st) {
            if (tr.stamps[st + 1] <= tr.stamps[st]) {
                std::fprintf(stderr,
                    "FAIL: boundary %u did not advance (%llu -> %llu); a stage was\n"
                    "      never marked, and the sum would telescope over the hole\n",
                    st + 1,
                    static_cast<unsigned long long>(tr.stamps[st]),
                    static_cast<unsigned long long>(tr.stamps[st + 1]));
                return false;
            }
        }

        uint64_t chain_sum = 0;
        for (unsigned st = 0; st < kStageCount; ++st) chain_sum += tr.duration(st);
        if (chain_sum != tr.service()) {
            std::fprintf(stderr,
                "FAIL: chain stages sum to %llu, end to end is %llu\n",
                static_cast<unsigned long long>(chain_sum),
                static_cast<unsigned long long>(tr.service()));
            return false;
        }

        // -- a scope per stage, as BudgetScope would do it ----------------
        uint64_t scope_sum = 0;
        const uint64_t outer_start = titans::bench::rdtsc_start();
        uint64_t last_end = 0;
        for (unsigned st = 0; st < kStageCount; ++st) {
            const uint64_t a = titans::bench::rdtsc_start();
            work(st);
            const uint64_t b = titans::bench::rdtsc_end();
            scope_sum += b - a;
            last_end = b;
        }
        const uint64_t scope_whole = last_end - outer_start;
        if (scope_sum >= scope_whole) continue;   // too quiet to resolve this rep
        scope_gap_total += scope_whole - scope_sum;
    }

    if (scope_gap_total == 0) {
        std::fprintf(stderr,
            "FAIL: scope-per-stage timing lost nothing across 200 repetitions.\n"
            "      Either the clock got free or this test stopped testing.\n");
        return false;
    }
    return true;
}

/// @brief The same property at the aggregate the report actually prints.
bool test_stage_means_sum_to_the_service_mean() {
    PipelineHistograms h;
    for (int rep = 0; rep < 500; ++rep) {
        StageTrace<true> tr;
        tr.open(0);
        volatile uint64_t sink = 0;
        for (unsigned st = 0; st < kStageCount; ++st) {
            for (int k = 0; k < 30 * (st + 1); ++k) sink += k;
            tr.mark(static_cast<Stage>(st));
        }
        h.fold(tr, 0);
    }
    const double sum = h.stage_mean_sum();
    const double whole = h.service().mean();
    if (whole <= 0.0 || std::fabs(sum - whole) / whole > 1e-9) {
        std::fprintf(stderr,
            "FAIL: stage means sum to %.3f, service mean is %.3f\n", sum, whole);
        return false;
    }
    return true;
}

// ============================================================================
// 4. A figure under the clock's resolution is refused.
// ============================================================================

bool test_percentile_below_the_floor_is_refused() {
    // 1 tick per ns and a 20 ns floor, so anything at or under 60 ns is refused.
    const LatencyFormatter f(1.0, 20.0);

    Histogram tiny, real;
    for (int i = 0; i < 1000; ++i) {
        tiny.record(10);
        real.record(1000);
    }

    const std::string refused = f.cell(tiny, 99);
    if (refused.find("under floor") == std::string::npos) {
        std::fprintf(stderr,
            "FAIL: a 10 ns figure under a 60 ns floor printed as '%s'\n",
            refused.c_str());
        return false;
    }
    if (f.mean_cell(tiny).find("under floor") == std::string::npos) {
        std::fprintf(stderr, "FAIL: the mean cell did not refuse\n");
        return false;
    }
    const std::string kept = f.cell(real, 99);
    if (kept.find("under floor") != std::string::npos) {
        std::fprintf(stderr,
            "FAIL: a 1000 ns figure was refused as '%s'\n", kept.c_str());
        return false;
    }
    if (std::fabs(f.resolvable_above_ns() - 60.0) > 1e-9) {
        std::fprintf(stderr, "FAIL: resolvable threshold is %.1f, expected 60\n",
                     f.resolvable_above_ns());
        return false;
    }
    return true;
}

// ============================================================================
// 5. The uninstrumented path really is uninstrumented.
// ============================================================================

/**
 * The probe cost is measured by differencing an instrumented run against an
 * uninstrumented one. If the "uninstrumented" build still stamps, that
 * difference is zero and the tool reports a free probe.
 */
bool test_uninstrumented_trace_takes_no_stamps() {
    StageTrace<false> tr;
    tr.open(12345);
    for (unsigned st = 0; st < kStageCount; ++st) tr.mark(static_cast<Stage>(st));

    if (tr.due != 0) {
        std::fprintf(stderr, "FAIL: uninstrumented trace stored a due time\n");
        return false;
    }
    for (unsigned i = 0; i <= kStageCount; ++i) {
        if (tr.stamps[i] != 0) {
            std::fprintf(stderr, "FAIL: uninstrumented trace stamped boundary %u\n", i);
            return false;
        }
    }
    if (tr.service() != 0 || tr.response() != 0 || tr.queueing() != 0) {
        std::fprintf(stderr, "FAIL: uninstrumented trace reported a duration\n");
        return false;
    }
    return true;
}

// ============================================================================
// 6. Response never runs backwards.
// ============================================================================

/**
 * A tick handled before it was due would produce a negative response time,
 * which in unsigned arithmetic is an enormous positive one -- and it would land
 * in the top bucket of the histogram and dominate every tail figure in the
 * report.
 */
bool test_response_clamps_when_handled_early() {
    StageTrace<true> tr;
    const uint64_t far_future = ~uint64_t{0} / 2;
    tr.open(far_future);
    for (unsigned st = 0; st < kStageCount; ++st) tr.mark(static_cast<Stage>(st));

    if (tr.response() != 0 || tr.queueing() != 0) {
        std::fprintf(stderr,
            "FAIL: a tick handled before its due time reported response %llu\n",
            static_cast<unsigned long long>(tr.response()));
        return false;
    }
    return true;
}

struct Case {
    const char* name;
    bool (*fn)();
};

const Case kCases[] = {
    {"stall moves corrected p99, not raw", test_stall_moves_corrected_p99_and_not_raw},
    {"correction lands near the exact response", test_correction_approximates_the_exact_response_time},
    {"a chain partitions, scopes lose time", test_a_chain_partitions_and_scopes_lose_time},
    {"stage means sum to the service mean", test_stage_means_sum_to_the_service_mean},
    {"a figure under the floor is refused", test_percentile_below_the_floor_is_refused},
    {"uninstrumented trace takes no stamps", test_uninstrumented_trace_takes_no_stamps},
    {"response clamps when handled early", test_response_clamps_when_handled_early},
};

}  // namespace

bool run_pipeline_latency_tests() {
    bool all = true;
    for (const auto& c : kCases) {
        const bool ok = c.fn();
        std::printf("  [%s] %s\n", ok ? "pass" : "FAIL", c.name);
        if (!ok) all = false;
    }
    return all;
}
