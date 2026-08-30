/**
 * @file lane_bridge.hpp
 * @brief Fast-lane -> slow-lane handoff that cannot block the fast lane.
 *
 * The fast lane offers observations to the slow lane; the slow lane consumes
 * them when it can. The bridge is deliberately lossy: when the consumer falls
 * behind, observations are DROPPED and counted, never queued without bound and
 * never allowed to apply backpressure.
 *
 * This is the opposite of the usual default. Most queueing libraries block or
 * grow when full, which is correct for a batch pipeline and catastrophic here:
 * a 200 ms LLM call would otherwise stall the market-data thread. Dropping is
 * the only acceptable failure mode on this edge, so the drop counter is a
 * first-class metric rather than a hidden error path -- if the slow lane is
 * losing 90% of observations, that is a capacity fact the operator must see,
 * not an exception to swallow.
 */

#pragma once

#include "titans/core/spsc_queue.hpp"
#include "titans/core/types.hpp"

#include <atomic>
#include <cstdint>

namespace titans {
namespace lanes {

/**
 * @brief What the fast lane tells the slow lane about the market.
 *
 * Fixed size and trivially copyable: it crosses a lock-free queue, so it must
 * not own memory whose lifetime the fast lane would have to manage.
 */
struct LaneObservation {
    Timestamp ingress_time = 0;      ///< When the fast lane saw the underlying event.
    uint64_t context_generation = 0; ///< Generation this observation belongs to.
    char symbol[16] = {};
    Price mid_price = 0;
    Price spread = 0;
    double imbalance = 0.0;          ///< Book imbalance at time of observation.
    double realized_vol = 0.0;
    uint64_t sequence = 0;
};

static_assert(std::is_trivially_copyable_v<LaneObservation>,
              "LaneObservation crosses a lock-free queue; it must be a POD");

/**
 * @brief Bounded, non-blocking, drop-on-full channel from fast to slow lane.
 *
 * @tparam Capacity  Ring size, a power of two. Sized for the burst the slow
 *                   lane can absorb, NOT for the fast lane's peak rate --
 *                   oversizing only delays and hides the drop signal.
 */
template <size_t Capacity = 1024>
class LaneBridge {
public:
    /**
     * @brief Offer an observation. Called from the fast lane.
     *
     * @return true if accepted, false if the slow lane is behind and the
     *         observation was dropped. Wait-free: a single failed CAS-free
     *         ring push. Never blocks, never allocates.
     */
    bool offer(const LaneObservation& obs) {
        offered_.fetch_add(1, std::memory_order_relaxed);
        if (queue_.try_push(obs)) {
            return true;
        }
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    /// @brief Consume one observation. Called from the slow lane.
    bool poll(LaneObservation& out) {
        if (queue_.try_pop(out)) {
            consumed_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    uint64_t offered() const { return offered_.load(std::memory_order_relaxed); }
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t consumed() const { return consumed_.load(std::memory_order_relaxed); }

    /**
     * @brief Fraction of observations the slow lane never saw.
     *
     * Expected to be high and that is fine: the slow lane samples the market,
     * it does not mirror it. A drop rate near 1.0 means the slow lane is
     * effectively blind and its advisories should be distrusted -- which is why
     * this is reported rather than silently absorbed.
     */
    double drop_rate() const {
        const uint64_t o = offered();
        return o ? static_cast<double>(dropped()) / o : 0.0;
    }

private:
    SPSCQueue<LaneObservation, Capacity> queue_;
    std::atomic<uint64_t> offered_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> consumed_{0};
};

}  // namespace lanes
}  // namespace titans
