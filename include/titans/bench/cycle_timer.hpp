/**
 * @file cycle_timer.hpp
 * @brief TSC-based cycle counting with explicit resolution accounting.
 *
 * WHY THIS EXISTS
 * ---------------
 * The obvious way to time an operation --
 *
 *     auto t0 = clock::now(); op(); record(clock::now() - t0);
 *
 * -- is invalid when `op` costs less than the clock read itself. On this
 * class of machine `clock_gettime(CLOCK_MONOTONIC)` via libstdc++ costs
 * ~20-45 ns and has ~10 ns granularity, while a lock-free SPSC push costs
 * single-digit nanoseconds. Bracketing the push with two clock reads
 * measures clock jitter, not the push: the result is both meaningless and
 * *smaller* than the measured cost of the clock call, which is arithmetically
 * impossible for a real per-operation cost.
 *
 * This header provides the two primitives needed to measure correctly:
 *
 *   1. `rdtsc_*()` - a ~6-8 cycle timestamp read, cheap enough to bracket
 *      operations in the tens-of-nanoseconds range.
 *   2. `TscClock` - calibration of TSC ticks to nanoseconds, plus an
 *      empirically measured *noise floor* (the cost of an empty measurement).
 *      Any per-operation figure at or below that floor is not a measurement
 *      and must be reported via amortized batch timing instead.
 *
 * REQUIREMENTS
 *   - x86-64 with constant_tsc + nonstop_tsc (invariant TSC).
 *     Verify with: grep -o 'constant_tsc\|nonstop_tsc' /proc/cpuinfo | sort -u
 *   - The measuring thread must be pinned (see bench/platform.hpp); the TSC is
 *     invariant across cores on such CPUs, but migration still perturbs caches.
 */

#pragma once

#include <cstdint>
#include <ctime>
#include <algorithm>
#include <array>
#include <string>

#if defined(__x86_64__) || defined(_M_X64)
#  include <x86intrin.h>
#  define TITANS_HAVE_RDTSC 1
#else
#  define TITANS_HAVE_RDTSC 0
#endif

namespace titans {
namespace bench {

// ============================================================================
// Optimization barriers
// ============================================================================

/**
 * @brief Force @p value to be materialized, preventing dead-code elimination.
 *
 * Without this the compiler is free to delete the very operation under test.
 * Modeled on the barriers in Google Benchmark.
 */
template <typename T>
inline void do_not_optimize(T const& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

template <typename T>
inline void do_not_optimize(T& value) {
    asm volatile("" : "+r,m"(value) : : "memory");
}

/// @brief Prevent the compiler from reordering memory operations across here.
inline void clobber_memory() {
    asm volatile("" : : : "memory");
}

// ============================================================================
// TSC access
// ============================================================================

/**
 * @brief Read the TSC, ordered against *preceding* instructions.
 *
 * `lfence` before `rdtsc` prevents the timestamp read from being hoisted above
 * the code we intend to measure. Use at the START of a timed region.
 */
inline uint64_t rdtsc_start() {
#if TITANS_HAVE_RDTSC
    _mm_lfence();
    uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
#else
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
#endif
}

/**
 * @brief Read the TSC, ordered against *following* instructions.
 *
 * `rdtscp` waits for prior instructions to retire; the trailing `lfence` keeps
 * later code from being sunk above it. Use at the END of a timed region.
 */
inline uint64_t rdtsc_end() {
#if TITANS_HAVE_RDTSC
    unsigned aux;
    uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
#else
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
#endif
}

/// @brief Monotonic wall clock, used only to calibrate the TSC.
inline uint64_t wall_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

// ============================================================================
// TscClock: calibration + noise floor
// ============================================================================

/**
 * @brief Converts TSC ticks to nanoseconds and reports the measurement floor.
 *
 * Construct ONCE per process, after pinning the thread. Calibration takes
 * roughly `calibration_ms` of wall time.
 */
class TscClock {
public:
    explicit TscClock(int calibration_ms = 200) {
        calibrate(calibration_ms);
        measure_noise_floor();
    }

    /// @brief TSC ticks per nanosecond (>1 on multi-GHz parts).
    double ticks_per_ns() const { return ticks_per_ns_; }

    /// @brief Nominal TSC frequency in Hz, as calibrated.
    double tsc_hz() const { return ticks_per_ns_ * 1e9; }

    double to_ns(uint64_t ticks) const {
        return static_cast<double>(ticks) / ticks_per_ns_;
    }

    /**
     * @brief Median cost, in ticks, of an empty rdtsc_start/rdtsc_end pair.
     *
     * This is the resolution limit of single-shot timing. A measured
     * per-operation cost at or below this value carries no information.
     */
    uint64_t noise_floor_ticks() const { return noise_floor_ticks_; }
    double noise_floor_ns() const { return to_ns(noise_floor_ticks_); }

    /// @brief 99th percentile of the empty-pair cost; the jitter tail.
    double noise_floor_p99_ns() const { return to_ns(noise_floor_p99_ticks_); }

    /**
     * @brief Is a single-shot measurement of @p ns trustworthy?
     *
     * We require the signal to clear the noise floor by 3x. Below that,
     * report amortized batch timing instead and say so.
     */
    bool is_resolvable(double ns) const {
        return ns > 3.0 * noise_floor_ns();
    }

    std::string describe() const {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "TSC %.4f GHz | single-shot floor: %.2f ns (p99 %.2f ns) | "
                      "resolvable above %.2f ns",
                      tsc_hz() / 1e9, noise_floor_ns(), noise_floor_p99_ns(),
                      3.0 * noise_floor_ns());
        return std::string(buf);
    }

private:
    void calibrate(int ms) {
        // Warm up so the first read is not a cold-path outlier.
        for (int i = 0; i < 1000; ++i) do_not_optimize(rdtsc_end());

        const uint64_t w0 = wall_ns();
        const uint64_t t0 = rdtsc_start();
        const uint64_t target = w0 + static_cast<uint64_t>(ms) * 1000000ULL;
        while (wall_ns() < target) {
            // Spin. `performance` governor + invariant TSC means the tick rate
            // is independent of the core's actual P-state.
        }
        const uint64_t t1 = rdtsc_end();
        const uint64_t w1 = wall_ns();

        ticks_per_ns_ = static_cast<double>(t1 - t0) / static_cast<double>(w1 - w0);
    }

    void measure_noise_floor() {
        constexpr int kSamples = 20000;
        std::array<uint64_t, kSamples> samples{};

        for (int i = 0; i < 2000; ++i) {  // warm up
            const uint64_t a = rdtsc_start();
            const uint64_t b = rdtsc_end();
            do_not_optimize(b - a);
        }
        for (int i = 0; i < kSamples; ++i) {
            const uint64_t a = rdtsc_start();
            const uint64_t b = rdtsc_end();
            samples[i] = b - a;
        }

        std::sort(samples.begin(), samples.end());
        noise_floor_ticks_ = samples[kSamples / 2];
        noise_floor_p99_ticks_ = samples[static_cast<size_t>(kSamples * 0.99)];
    }

    double ticks_per_ns_ = 1.0;
    uint64_t noise_floor_ticks_ = 0;
    uint64_t noise_floor_p99_ticks_ = 0;
};

}  // namespace bench
}  // namespace titans
