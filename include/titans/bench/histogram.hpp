/**
 * @file histogram.hpp
 * @brief Fixed-layout latency histogram with a coordinated-omission correction.
 *
 * WHY NOT A SORTED VECTOR
 * -----------------------
 * The existing tools collect samples into a `std::vector<int64_t>`, sort it, and
 * index. That is exact and it does not scale: it allocates per sample, it cannot
 * run on the fast path, and two runs of different length produce percentiles
 * that are not directly comparable.
 *
 * This uses the HdrHistogram bucket layout -- powers of two, each split into a
 * fixed number of linear sub-buckets -- which gives constant-time recording into
 * pre-allocated storage and a bucket boundary that depends only on the value,
 * never on the sample count. Two runs are therefore comparable bucket for
 * bucket, which is what a regression gate needs (see docs/ROADMAP.md P2).
 *
 * PRECISION
 * ---------
 * With `kSubBucketBits = 8` there are 256 sub-buckets per octave, so any
 * reported value is within 1/128 (0.8%) of the true one, and `relative_error()`
 * returns that bound rather than leaving the reader to assume exactness. The
 * whole table is 7424 counters, 58 KB, allocated once.
 *
 * COORDINATED OMISSION
 * --------------------
 * `record_corrected` implements Gil Tene's correction: when a sample took much
 * longer than the interval at which samples were supposed to be taken, the
 * samples that *would* have been taken during the stall never happened, and
 * omitting them makes a system that froze look fast. The correction synthesises
 * them with the latency each would have seen. A closed-loop measurement of a
 * service that froze for 500 ms reports a p99 of 10 ms uncorrected and 460 ms
 * corrected; the difference is not a rounding detail.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace titans {
namespace bench {

class Histogram {
public:
    /// Sub-buckets per octave, as a power of two. 8 => 256 => 0.8% precision.
    static constexpr unsigned kSubBucketBits = 8;
    static constexpr uint64_t kSubBucketCount = 1ULL << kSubBucketBits;
    static constexpr uint64_t kSubBucketHalfCount = kSubBucketCount >> 1;
    /// Highest bucket index a 64-bit value can reach.
    static constexpr unsigned kMaxBucket = 63 - (kSubBucketBits - 1);
    static constexpr std::size_t kCounts =
        static_cast<std::size_t>(kMaxBucket) * kSubBucketHalfCount + kSubBucketCount;

    Histogram() : counts_(kCounts, 0) {}

    void record(uint64_t value) {
        ++counts_[index_of(value)];
        ++total_;
        if (value > max_) max_ = value;
        sum_ += value;
    }

    /**
     * @brief Record a sample, synthesising the samples a stall swallowed.
     *
     * If `value` exceeds `expected_interval`, the caller was blocked and could
     * not take the samples that were due during that window. Those samples are
     * added back with the latency they would have observed: one interval less
     * on each step down. Passing `expected_interval == 0` disables the
     * correction and this behaves exactly like `record`.
     */
    void record_corrected(uint64_t value, uint64_t expected_interval) {
        record(value);
        if (expected_interval == 0 || value <= expected_interval) return;
        for (uint64_t missing = value - expected_interval;
             missing >= expected_interval; missing -= expected_interval) {
            record(missing);
        }
    }

    /**
     * @brief Highest value in the bucket holding the requested percentile.
     *
     * Deliberately the bucket's upper bound rather than its midpoint: a latency
     * percentile that is reported low is the failure mode that matters, so the
     * rounding goes the safe way. `relative_error()` bounds how far up.
     */
    uint64_t percentile(double p) const {
        if (total_ == 0) return 0;
        const double clamped = std::min(100.0, std::max(0.0, p));
        // ceil so that percentile(100) requires the whole population.
        const auto want = static_cast<uint64_t>(
            std::ceil(clamped / 100.0 * static_cast<double>(total_)));
        uint64_t seen = 0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            seen += counts_[i];
            if (seen >= want && counts_[i] > 0) {
                return std::min(highest_equivalent(i), max_);
            }
        }
        return max_;
    }

    uint64_t count() const { return total_; }
    uint64_t max() const { return max_; }
    double mean() const {
        return total_ ? static_cast<double>(sum_) / static_cast<double>(total_) : 0.0;
    }
    /// @brief Worst-case relative error on any reported value.
    static constexpr double relative_error() {
        return 1.0 / static_cast<double>(kSubBucketHalfCount);
    }

    /// @brief How many recorded samples were at or above `threshold`.
    uint64_t count_at_or_above(uint64_t threshold) const {
        uint64_t n = 0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            if (counts_[i] && lowest_equivalent(i) >= threshold) n += counts_[i];
        }
        return n;
    }

    void reset() {
        std::fill(counts_.begin(), counts_.end(), 0);
        total_ = 0;
        max_ = 0;
        sum_ = 0;
    }

    /// @brief Bucket index for a value. Monotone non-decreasing in `value`.
    static std::size_t index_of(uint64_t value) {
        if (value < kSubBucketCount) return static_cast<std::size_t>(value);
        const unsigned msb = 63u - static_cast<unsigned>(__builtin_clzll(value));
        const unsigned bucket = msb - (kSubBucketBits - 1);
        const uint64_t sub = value >> bucket;   // in [half, full)
        return static_cast<std::size_t>(bucket) * kSubBucketHalfCount + sub;
    }

    /// @brief Smallest value that lands in `index`.
    static uint64_t lowest_equivalent(std::size_t index) {
        if (index < kSubBucketCount) return index;
        const auto bucket = static_cast<unsigned>(
            (index - kSubBucketHalfCount) / kSubBucketHalfCount);
        const uint64_t sub = index - static_cast<std::size_t>(bucket) * kSubBucketHalfCount;
        return sub << bucket;
    }

    /// @brief Largest value that lands in `index`.
    static uint64_t highest_equivalent(std::size_t index) {
        if (index < kSubBucketCount) return index;
        const auto bucket = static_cast<unsigned>(
            (index - kSubBucketHalfCount) / kSubBucketHalfCount);
        return lowest_equivalent(index) + (1ULL << bucket) - 1;
    }

private:
    std::vector<uint64_t> counts_;
    uint64_t total_ = 0;
    uint64_t max_ = 0;
    uint64_t sum_ = 0;
};

}  // namespace bench
}  // namespace titans
