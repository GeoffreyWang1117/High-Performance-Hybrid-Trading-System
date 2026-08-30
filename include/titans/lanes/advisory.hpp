/**
 * @file advisory.hpp
 * @brief The only channel by which slow-lane (LLM) output reaches the fast path.
 *
 * DESIGN CONTRACT
 * ---------------
 * An LLM cannot be on a microsecond path: a local 8B model answers in tens to
 * hundreds of milliseconds, five to six orders of magnitude above the fast
 * lane's budget. So the LLM never sits in the request path. It publishes
 * *advisories* -- small, immutable, explicitly expiring facts -- into a slot
 * that the fast lane samples with a bounded, non-blocking read.
 *
 * Three properties make this safe, and each is tested in
 * tests/test_lane_isolation.cpp:
 *
 *   1. NON-BLOCKING READ. The fast lane never waits on the writer. A read is a
 *      bounded seqlock retry; on exhaustion it keeps its previously cached
 *      advisory rather than stalling. Worst-case read cost is bounded by
 *      kMaxReadRetries, independent of what the slow lane is doing.
 *
 *   2. NO BACKPRESSURE. The writer never waits on readers and never allocates.
 *      A slow lane that hangs, times out, or dies leaves the last advisory in
 *      place; it cannot stall, slow, or block the fast lane.
 *
 *   3. EXPLICIT EXPIRY. Every advisory carries `valid_until`. The fast lane
 *      treats an expired advisory as absent. This is the load-bearing defence
 *      against acting on stale model output: staleness is not a heuristic to be
 *      tuned, it is a timestamp comparison the fast path performs on every read.
 *
 * Property 3 is also where this system touches the LLM context-contamination
 * problem: an advisory derived from context that has since been invalidated is
 * exactly a contaminated inference, and `valid_until` plus `context_generation`
 * are the mechanism that bounds its blast radius. See docs/ARCHITECTURE.md.
 */

#pragma once

#include "titans/core/types.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace titans {
namespace lanes {

/// @brief Direction of a model's view, kept deliberately coarse.
enum class AdvisoryStance : uint8_t {
    None = 0,
    RiskOff,     ///< Reduce exposure / widen quotes.
    Neutral,
    RiskOn,      ///< Normal operation permitted.
    Halt         ///< Stop quoting this symbol entirely.
};

/**
 * @brief One slow-lane conclusion, valid for a bounded window.
 *
 * Trivially copyable POD by design: the seqlock below copies it byte-for-byte
 * and must never touch a heap pointer that could be freed underneath a reader.
 */
struct Advisory {
    AdvisoryStance stance = AdvisoryStance::None;

    /// Model's self-reported confidence in [0, 1]. Advisory only.
    float confidence = 0.0f;

    /// Multiplier the fast lane applies to position limits, in [0, 1].
    float size_multiplier = 1.0f;

    /// When the slow lane produced this.
    Timestamp issued_at = 0;

    /// After this instant the advisory is ignored. Never 0 in a live advisory.
    Timestamp valid_until = 0;

    /**
     * @brief Generation of the context this conclusion was drawn from.
     *
     * The fast lane bumps a context generation whenever it observes an event
     * that invalidates prior state (a book reset, a halt, a reconnect). An
     * advisory whose generation is behind the current one was reasoned from
     * facts that no longer hold, and is rejected regardless of `valid_until`.
     */
    uint64_t context_generation = 0;

    /// Which model produced it, for attribution in post-trade analysis.
    char source[32] = {};

    bool is_valid_at(Timestamp now, uint64_t current_generation) const {
        return stance != AdvisoryStance::None &&
               valid_until > now &&
               context_generation == current_generation;
    }
};

static_assert(std::is_trivially_copyable_v<Advisory>,
              "Advisory is copied under a seqlock; it must not own resources");

/**
 * @brief Single-writer / multi-reader latest-value slot with bounded reads.
 *
 * IMPLEMENTATION: versioned ring with lap detection, not a plain seqlock.
 *
 * A textbook seqlock (odd sequence => write in progress) was tried first and
 * `test_seqlock_never_tears` caught it handing back torn values -- 414 of them
 * in 2.8M reads under a tight-loop writer. The C++ fence formulation of a
 * seqlock is easy to get subtly wrong, because a release fence stops earlier
 * accesses from sinking below it but does NOT stop the payload writes that
 * follow from being hoisted above the "write in progress" marker.
 *
 * This version removes the hazard structurally instead of ordering around it.
 * The writer never touches the slot a reader could be reading:
 *
 *   - `generation_` counts publishes and only ever increases.
 *   - A reader reads slot `g0 % kRing`.
 *   - The writer, at generation `g`, writes slot `(g + 1) % kRing` and only
 *     then publishes `g + 1`. That slot is distinct from every slot a reader
 *     sampled at any generation in `(g + 1 - kRing, g]`.
 *   - So a read is valid unless the writer LAPPED it, which requires the
 *     generation to advance by `kRing - 1` during the copy. The reader
 *     re-reads `generation_` afterwards and detects exactly that.
 *
 * Reads are therefore bounded (at most kMaxReadRetries attempts) and either
 * return a value that was coherent at some instant, or report failure.
 *
 * @note As with every seqlock-family structure, the lapped case performs a
 *       copy that races formally under the C++ memory model; the result is
 *       detected and discarded, never used. This is the same trade the Linux
 *       kernel, folly, and Rust's seqlock crate make.
 */
class AdvisorySlot {
public:
    /// Ring depth. Sized so a fast-path reader is never lapped in practice:
    /// the slow lane publishes at model speed (single-digit Hz), the reader
    /// copies 72 bytes in nanoseconds.
    static constexpr uint64_t kRing = 16;
    static_assert((kRing & (kRing - 1)) == 0, "kRing must be a power of two");

    /// Bounded so a fast-path read has a hard worst case. See try_read().
    static constexpr int kMaxReadRetries = 4;

    /**
     * @brief Publish a new advisory. Called only by the slow lane.
     *
     * Wait-free: no loop, no allocation, no blocking. Never waits on a reader.
     */
    void publish(const Advisory& a) {
        const uint64_t g = generation_.load(std::memory_order_relaxed);
        ring_[(g + 1) & (kRing - 1)] = a;
        generation_.store(g + 1, std::memory_order_release);
    }

    /**
     * @brief Attempt a consistent read. Called by the fast lane.
     *
     * @param[out] out  Filled only when the function returns true.
     * @return false if the writer lapped the reader on every attempt, in which
     *         case @p out is untouched and the caller keeps its cached
     *         advisory. Returning false is normal and safe; it is never a
     *         reason to wait.
     */
    bool try_read(Advisory& out) const {
        for (int attempt = 0; attempt < kMaxReadRetries; ++attempt) {
            const uint64_t g0 = generation_.load(std::memory_order_acquire);
            if (g0 == 0) return false;               // nothing published yet

            const Advisory tmp = ring_[g0 & (kRing - 1)];

            const uint64_t g1 = generation_.load(std::memory_order_acquire);
            if (g1 - g0 < kRing - 1) {               // not lapped: the copy is coherent
                out = tmp;
                return true;
            }
        }
        read_retry_exhausted_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    uint64_t publishes() const { return generation_.load(std::memory_order_relaxed); }

    /// @brief How often a fast-path read gave up and used its cached value.
    uint64_t read_retry_exhausted() const {
        return read_retry_exhausted_.load(std::memory_order_relaxed);
    }

private:
    alignas(64) std::atomic<uint64_t> generation_{0};
    alignas(64) Advisory ring_[kRing]{};
    alignas(64) mutable std::atomic<uint64_t> read_retry_exhausted_{0};
};

/**
 * @brief The fast lane's view of slow-lane advice.
 *
 * Holds a cached copy so that a failed read, an expired advisory, or a dead
 * slow lane all degrade to the same well-defined behaviour: fall back to the
 * configured default stance and keep trading. Never throws, never allocates,
 * never blocks.
 */
class AdvisoryView {
public:
    explicit AdvisoryView(const AdvisorySlot& slot,
                          AdvisoryStance fallback = AdvisoryStance::Neutral)
        : slot_(slot), fallback_(fallback) {}

    /**
     * @brief Refresh the cache and return the advisory in force at @p now.
     *
     * Cost: one bounded seqlock read plus two timestamp comparisons. No
     * syscalls, no allocation, no lock.
     */
    Advisory current(Timestamp now, uint64_t current_generation) {
        Advisory fresh;
        if (slot_.try_read(fresh)) {
            cached_ = fresh;
        } else {
            ++stale_reads_;   // kept the previous value; see AdvisorySlot::try_read
        }

        if (!cached_.is_valid_at(now, current_generation)) {
            ++rejected_;
            Advisory neutral;
            neutral.stance = fallback_;
            neutral.size_multiplier = 1.0f;
            neutral.context_generation = current_generation;
            return neutral;
        }
        return cached_;
    }

    uint64_t stale_reads() const { return stale_reads_; }

    /// @brief Advisories discarded for being expired or generation-stale.
    uint64_t rejected() const { return rejected_; }

private:
    const AdvisorySlot& slot_;
    AdvisoryStance fallback_;
    Advisory cached_{};
    uint64_t stale_reads_ = 0;
    uint64_t rejected_ = 0;
};

}  // namespace lanes
}  // namespace titans
