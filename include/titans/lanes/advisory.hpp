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
 *      treats an expired advisory as absent: staleness is not a heuristic to be
 *      tuned, it is a timestamp comparison the fast path performs on every read.
 *
 *   4. DECLARED FRESHNESS. Every advisory also carries `signal_horizon_ns`, the
 *      horizon of the thing it is predicting, and the consumer declares what
 *      fraction of that horizon it will tolerate.
 *
 * Property 4 exists because property 3 was measured and found to bound the
 * wrong quantity. With a 60-second TTL, so wide that almost nothing expired,
 * acted-on advice had a p50 age of 82 ms and a p99 of 1507 ms against a 1000 ms
 * prediction horizon -- one read in a hundred acting on advice older than the
 * entire horizon it predicted over, while comfortably inside its lifetime.
 * `valid_until` expresses a latency budget; what decides whether advice is
 * worth anything is its age relative to its signal's horizon. See
 * lanes/freshness.hpp and docs/ROADMAP.md.
 *
 * Properties 3 and 4 are also where this system touches the LLM
 * context-contamination problem: an advisory derived from context that has
 * since been invalidated is exactly a contaminated inference, and `valid_until`
 * plus `context_generation` are the mechanism that bounds its blast radius. See
 * docs/ARCHITECTURE.md.
 */

#pragma once

#include "titans/core/types.hpp"
#include "freshness.hpp"

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

    /**
     * @brief Which side the risk is on: +1 buy-side, -1 sell-side, 0 undirected.
     *
     * Risk is usually directional and an undirected advisory throws that away.
     * Measured concretely in the lanes demo: a slow lane that flagged large
     * |order flow| without saying WHICH way scored an informedness of
     * +0.013 +/- 0.020 -- indistinguishable from zero -- even though the same
     * underlying feature reaches AUC 0.76 against the label when the sign is
     * kept aligned with the trade under review. The fast lane is expected to
     * act only when its own side matches this one.
     */
    int8_t risk_direction = 0;

    /// When the slow lane produced this.
    Timestamp issued_at = 0;

    /// After this instant the advisory is ignored. Never 0 in a live advisory.
    Timestamp valid_until = 0;

    /**
     * @brief Horizon of the signal this advisory carries, in nanoseconds.
     *
     * Not a lifetime. It is how far into the future the underlying claim
     * reaches -- for the toxic-flow policy, the window over which the price is
     * predicted to move. A consumer uses it to decide how old is too old, which
     * `valid_until` cannot express: advice about the next second is worthless
     * at 900 ms old and advice about the next hour is not, and one absolute
     * expiry cannot say both.
     *
     * 0 means the producer did not declare one, and a freshness gate then has
     * nothing to work against and must let the advisory through rather than
     * silently reject everything.
     */
    Timestamp signal_horizon_ns = 0;

    /**
     * @brief Generation of the context this conclusion was drawn from.
     *
     * The fast lane bumps a context generation whenever it observes an event
     * that invalidates prior state (a book reset, a halt, a reconnect). An
     * advisory whose generation is behind the current one was reasoned from
     * facts that no longer hold, and is rejected regardless of `valid_until`.
     */
    uint64_t context_generation = 0;

    /**
     * @brief A slow-lane-computed PARAMETER the fast lane applies itself.
     *
     * The alternative to shipping a decision. Measurement forced this field
     * into existence: a lane shipping decisions fired at nearly the right rate
     * (3211 actions against an ideal 3320) and agreed with the ideal on only
     * 49.1% of them, because a threshold-crossing decision is only correct at
     * the instant it is taken. Three trades of delay -- the median here -- moves
     * it onto the wrong trades, and no expiry rule or freshness gate can
     * realign it, which is why neither helped.
     *
     * A parameter does not have that problem. The calibrated warn threshold is
     * a 5000-sample quantile; it moves slowly, so it survives the trip. The
     * decision moves at every trade, so it does not. Put the slow-changing
     * quantity on the slow lane and let the fast lane evaluate the rule on
     * state it already holds.
     *
     * This is the same split online feature stores make when they assign each
     * feature its own staleness budget rather than one global freshness target.
     */
    double parameter = 0.0;

    /// Which model produced it, for attribution in post-trade analysis.
    char source[32] = {};

    bool is_valid_at(Timestamp now, uint64_t current_generation) const {
        return stance != AdvisoryStance::None &&
               valid_until > now &&
               context_generation == current_generation;
    }

    /// @brief How long ago this was issued, clamped at 0 for clock skew.
    Timestamp age_at(Timestamp now) const {
        return now > issued_at ? now - issued_at : 0;
    }

    /**
     * @brief Fresh enough to act on under @p policy.
     *
     * True when the gate is disabled, and true when the producer declared no
     * horizon: a gate with no horizon to measure against would reject
     * everything, which is a worse failure than not gating.
     */
    bool is_fresh_at(Timestamp now, const FreshnessPolicy& policy) const {
        if (!policy.gate_enabled() || signal_horizon_ns <= 0) return true;
        return age_at(now) <= policy.max_age_ns(signal_horizon_ns);
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
                          AdvisoryStance fallback = AdvisoryStance::Neutral,
                          FreshnessPolicy freshness = {})
        : slot_(slot), fallback_(fallback), freshness_(freshness) {}

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

        // Record the age of the newest advice the slow lane has produced,
        // BEFORE any consumer-side check. Expiry and the freshness gate are
        // both the consumer's policy; how old the available advice is, is the
        // producer's property, and it is what the SLO is declared on.
        //
        // Recording this after the expiry check would make the SLO tautological
        // whenever the TTL is tighter than the horizon: the distribution would
        // be truncated at the TTL and the contract would report MET because it
        // had thrown away every sample that would have failed it. That is
        // exactly what the first version of this code did.
        // Guarded on stance alone. A default-constructed advisory -- what
        // `cached_` holds before the slow lane has published anything -- has
        // stance None, so that is the complete condition. An earlier version
        // also required `issued_at > 0`, which silently dropped every advisory
        // issued at timestamp zero; the test that caught it publishes at 0
        // because a sentinel that collides with a legitimate value is a bug
        // whether or not the collision is likely.
        const Timestamp age = cached_.age_at(now);
        if (cached_.stance != AdvisoryStance::None) {
            monitor_.record_offered(age);
        }

        if (!cached_.is_valid_at(now, current_generation)) {
            ++rejected_;
            return neutral_for(current_generation);
        }

        if (!cached_.is_fresh_at(now, freshness_)) {
            ++rejected_stale_;
            monitor_.record_gate_rejection();
            return neutral_for(current_generation);
        }

        monitor_.record_acted(age);
        return cached_;
    }

    uint64_t stale_reads() const { return stale_reads_; }

    /// @brief Advisories discarded for being expired or generation-stale.
    uint64_t rejected() const { return rejected_; }

    /// @brief Advisories discarded for being too old relative to their horizon.
    ///        Counted apart from `rejected()` on purpose: one says the advice
    ///        ran out of time, the other says it was never going to be useful.
    uint64_t rejected_stale() const { return rejected_stale_; }

    const FreshnessMonitor& freshness() const { return monitor_; }
    const FreshnessPolicy& freshness_policy() const { return freshness_; }

private:
    Advisory neutral_for(uint64_t generation) const {
        Advisory neutral;
        neutral.stance = fallback_;
        neutral.size_multiplier = 1.0f;
        neutral.context_generation = generation;
        return neutral;
    }

    const AdvisorySlot& slot_;
    AdvisoryStance fallback_;
    FreshnessPolicy freshness_;
    FreshnessMonitor monitor_;
    Advisory cached_{};
    uint64_t stale_reads_ = 0;
    uint64_t rejected_ = 0;
    uint64_t rejected_stale_ = 0;
};

}  // namespace lanes
}  // namespace titans
