/**
 * @file harness.hpp
 * @brief Benchmark harness that refuses to report unresolvable measurements.
 *
 * TWO MEASUREMENT MODES
 * ---------------------
 * Which one is valid depends entirely on how the operation's cost compares to
 * the timing noise floor (see cycle_timer.hpp). The harness picks, and labels
 * its choice in the output so the mode is never ambiguous to a reader.
 *
 *   AMORTIZED  - `batch_size` operations inside a single timed region, cost
 *                divided by the count. This is the ONLY valid mode for
 *                operations costing less than ~3x the noise floor
 *                (i.e. most lock-free primitives). It yields an accurate mean
 *                but, by construction, NO per-operation distribution: an
 *                amortized run cannot produce a p99 and this harness will not
 *                pretend otherwise.
 *
 *   PER_OP     - each operation individually bracketed by rdtsc. Valid only
 *                when the operation clears the noise floor. Yields a real
 *                latency distribution.
 *
 * REPETITIONS
 * -----------
 * Every measurement is repeated `reps` times. We report the median across
 * repetitions (robust to a single interfering repetition) and the minimum
 * (the best estimate of interference-free cost), plus the spread between them
 * as an interference indicator. A single run of a microbenchmark on a
 * non-isolated machine is not a result.
 */

#pragma once

#include "cycle_timer.hpp"
#include "platform.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace titans {
namespace bench {

// ============================================================================
// Results
// ============================================================================

enum class MeasurementMode { Amortized, PerOp };

inline const char* to_string(MeasurementMode m) {
    return m == MeasurementMode::Amortized ? "amortized" : "per_op";
}

struct BenchmarkResult {
    std::string name;
    std::string unit = "ns/op";
    MeasurementMode mode = MeasurementMode::Amortized;

    // Always populated.
    double mean_ns = 0.0;          // median across repetitions
    double min_ns = 0.0;           // interference-free estimate
    double max_ns = 0.0;
    double rep_spread_pct = 0.0;   // (median - min) / min * 100
    int repetitions = 0;
    uint64_t ops_per_rep = 0;

    // Populated in PER_OP mode only. In AMORTIZED mode these stay
    // has_distribution=false and MUST NOT be reported as percentiles.
    bool has_distribution = false;
    double p50_ns = 0.0, p90_ns = 0.0, p99_ns = 0.0, p999_ns = 0.0;
    double stddev_ns = 0.0;

    // Derived throughput: 1/mean, from the amortized cost. Honest for
    // single-threaded serial issue; it is NOT a pipelined-system throughput.
    double ops_per_sec() const { return mean_ns > 0 ? 1e9 / mean_ns : 0.0; }

    std::string mode_note() const {
        if (mode == MeasurementMode::Amortized) {
            return "amortized batch timing; per-operation distribution is not "
                   "measurable at this cost scale";
        }
        return "per-operation timing; distribution is measured";
    }
};

// ============================================================================
// Harness
// ============================================================================

class Harness {
public:
    /**
     * @param pin_core_index  Physical core index to pin to. Core 0 handles most
     *                        kernel work on a default Linux install, so we
     *                        default to a mid-range core.
     */
    explicit Harness(size_t pin_core_index = 4, int calibration_ms = 200)
        : topo_(CpuTopology::detect()) {
        const int cpu = topo_.primary_cpu_of_core(pin_core_index);
        pinned_ok_ = (cpu >= 0) && pin_to_cpu(cpu);

        clock_ = std::make_unique<TscClock>(calibration_ms);

        fingerprint_ = MachineFingerprint::capture();
        fingerprint_.pinned_cpu = pinned_ok_ ? cpu : current_cpu();
        fingerprint_.tsc_ghz = clock_->tsc_hz() / 1e9;
        fingerprint_.noise_floor_ns = clock_->noise_floor_ns();
    }

    const TscClock& clock() const { return *clock_; }
    const MachineFingerprint& fingerprint() const { return fingerprint_; }
    bool pinned() const { return pinned_ok_; }

    /**
     * @brief Time @p op with amortized batch timing.
     *
     * @p op is invoked `batch_size` times per repetition inside one timed
     * region. Use for anything in the single- or double-digit nanosecond range.
     *
     * @p setup runs before each repetition, OUTSIDE the timed region (e.g. to
     * refill a queue that the batch drains).
     */
    template <typename Op, typename Setup = std::function<void()>>
    BenchmarkResult measure_amortized(
        const std::string& name,
        uint64_t batch_size,
        int reps,
        Op&& op,
        Setup&& setup = [] {}
    ) {
        // Warm caches, branch predictors, and any lazily-touched pages.
        setup();
        for (uint64_t i = 0; i < std::min<uint64_t>(batch_size, 10000); ++i) op();

        std::vector<double> per_rep_ns;
        per_rep_ns.reserve(static_cast<size_t>(reps));

        for (int r = 0; r < reps; ++r) {
            setup();
            clobber_memory();
            const uint64_t t0 = rdtsc_start();
            for (uint64_t i = 0; i < batch_size; ++i) {
                op();
            }
            const uint64_t t1 = rdtsc_end();
            clobber_memory();

            const double total_ns = clock_->to_ns(t1 - t0);
            per_rep_ns.push_back(total_ns / static_cast<double>(batch_size));
        }

        BenchmarkResult res;
        res.name = name;
        res.mode = MeasurementMode::Amortized;
        res.repetitions = reps;
        res.ops_per_rep = batch_size;
        res.has_distribution = false;
        summarize_reps(per_rep_ns, res);
        return res;
    }

    /**
     * @brief Time @p op per-operation, producing a real latency distribution.
     *
     * Rejected automatically (falls back to reporting mode=Amortized with
     * has_distribution=false) if the resulting cost does not clear the noise
     * floor, because percentiles of clock jitter are not percentiles of the
     * operation.
     *
     * @p op receives the iteration index and must do its own work; the harness
     * brackets each call. Samples land in a preallocated buffer -- no
     * allocation occurs inside the timed region.
     */
    template <typename Op, typename Setup = std::function<void()>>
    BenchmarkResult measure_per_op(
        const std::string& name,
        uint64_t iterations,
        int reps,
        Op&& op,
        Setup&& setup = [] {}
    ) {
        std::vector<uint64_t> ticks(iterations);   // preallocated, reused
        std::vector<double> all_ns;
        all_ns.reserve(static_cast<size_t>(iterations) * reps);
        std::vector<double> per_rep_mean;
        per_rep_mean.reserve(static_cast<size_t>(reps));

        setup();
        for (uint64_t i = 0; i < std::min<uint64_t>(iterations, 10000); ++i) op(i);

        for (int r = 0; r < reps; ++r) {
            setup();
            for (uint64_t i = 0; i < iterations; ++i) {
                const uint64_t t0 = rdtsc_start();
                op(i);
                const uint64_t t1 = rdtsc_end();
                ticks[i] = t1 - t0;
            }
            double sum = 0;
            for (uint64_t i = 0; i < iterations; ++i) {
                const double ns = clock_->to_ns(ticks[i]);
                all_ns.push_back(ns);
                sum += ns;
            }
            per_rep_mean.push_back(sum / static_cast<double>(iterations));
        }

        BenchmarkResult res;
        res.name = name;
        res.repetitions = reps;
        res.ops_per_rep = iterations;
        summarize_reps(per_rep_mean, res);

        // Validity gate: is the signal above the timing noise floor?
        if (!clock_->is_resolvable(res.mean_ns)) {
            res.mode = MeasurementMode::Amortized;
            res.has_distribution = false;
            res.name += " [UNRESOLVABLE per-op: cost " +
                        fmt(res.mean_ns) + " ns <= 3x noise floor " +
                        fmt(clock_->noise_floor_ns()) + " ns]";
            return res;
        }

        res.mode = MeasurementMode::PerOp;
        res.has_distribution = true;

        std::sort(all_ns.begin(), all_ns.end());
        res.p50_ns  = pct(all_ns, 50.0);
        res.p90_ns  = pct(all_ns, 90.0);
        res.p99_ns  = pct(all_ns, 99.0);
        res.p999_ns = pct(all_ns, 99.9);
        res.max_ns  = all_ns.back();

        const double mean = std::accumulate(all_ns.begin(), all_ns.end(), 0.0) / all_ns.size();
        double sq = 0;
        for (double v : all_ns) sq += (v - mean) * (v - mean);
        res.stddev_ns = std::sqrt(sq / all_ns.size());

        return res;
    }

    // ------------------------------------------------------------------
    // Reporting
    // ------------------------------------------------------------------

    static void print_header() {
        std::printf("\n%-42s %6s %12s %12s %10s %14s\n",
                    "Benchmark", "mode", "cost (ns)", "min (ns)", "spread",
                    "ops/sec");
        std::printf("%s\n", std::string(100, '-').c_str());
    }

    static void print_result(const BenchmarkResult& r) {
        std::printf("%-42s %6s %12.2f %12.2f %9.1f%% %14.2fM\n",
                    r.name.c_str(), to_string(r.mode), r.mean_ns, r.min_ns,
                    r.rep_spread_pct, r.ops_per_sec() / 1e6);
        if (r.has_distribution) {
            std::printf("%-42s %6s p50=%.1f p90=%.1f p99=%.1f p99.9=%.1f sd=%.1f\n",
                        "", "", r.p50_ns, r.p90_ns, r.p99_ns, r.p999_ns, r.stddev_ns);
        }
    }

    static std::string result_to_json(const BenchmarkResult& r, int indent = 4) {
        const std::string pad(indent, ' ');
        std::ostringstream os;
        os << "{\n";
        os << pad << "\"name\": \"" << r.name << "\",\n";
        os << pad << "\"mode\": \"" << to_string(r.mode) << "\",\n";
        os << pad << "\"mode_note\": \"" << r.mode_note() << "\",\n";
        os << pad << "\"cost_ns\": " << r.mean_ns << ",\n";
        os << pad << "\"min_ns\": " << r.min_ns << ",\n";
        os << pad << "\"rep_spread_pct\": " << r.rep_spread_pct << ",\n";
        os << pad << "\"repetitions\": " << r.repetitions << ",\n";
        os << pad << "\"ops_per_rep\": " << r.ops_per_rep << ",\n";
        os << pad << "\"ops_per_sec\": " << r.ops_per_sec() << ",\n";
        os << pad << "\"has_distribution\": " << (r.has_distribution ? "true" : "false");
        if (r.has_distribution) {
            os << ",\n";
            os << pad << "\"p50_ns\": " << r.p50_ns << ",\n";
            os << pad << "\"p90_ns\": " << r.p90_ns << ",\n";
            os << pad << "\"p99_ns\": " << r.p99_ns << ",\n";
            os << pad << "\"p999_ns\": " << r.p999_ns << ",\n";
            os << pad << "\"stddev_ns\": " << r.stddev_ns << "\n";
        } else {
            os << "\n";
        }
        os << std::string(indent >= 2 ? indent - 2 : 0, ' ') << "}";
        return os.str();
    }

private:
    static std::string fmt(double v) {
        char b[32];
        std::snprintf(b, sizeof(b), "%.2f", v);
        return std::string(b);
    }

    static double pct(const std::vector<double>& sorted, double p) {
        if (sorted.empty()) return 0.0;
        const size_t idx = static_cast<size_t>(sorted.size() * p / 100.0);
        return sorted[std::min(idx, sorted.size() - 1)];
    }

    static void summarize_reps(std::vector<double> per_rep, BenchmarkResult& res) {
        if (per_rep.empty()) return;
        std::sort(per_rep.begin(), per_rep.end());
        res.mean_ns = per_rep[per_rep.size() / 2];   // median across reps
        res.min_ns = per_rep.front();
        res.max_ns = std::max(res.max_ns, per_rep.back());
        res.rep_spread_pct = res.min_ns > 0
            ? (res.mean_ns - res.min_ns) / res.min_ns * 100.0
            : 0.0;
    }

    CpuTopology topo_;
    std::unique_ptr<TscClock> clock_;
    MachineFingerprint fingerprint_;
    bool pinned_ok_ = false;
};

}  // namespace bench
}  // namespace titans
