/**
 * @file flow_policy.hpp
 * @brief The trailing order-flow slow-lane policy, as a pure function.
 *
 * WHY THIS IS ITS OWN FILE
 * ------------------------
 * This policy used to live inside the slow-lane thread lambda in
 * `titans_lanes`. Evaluating it out-of-sample meant writing a second copy in
 * the walk-forward tool, and two copies of a decision rule drift: the moment
 * they disagree, the walk-forward number describes a policy that does not run.
 *
 * So the rule lives here, with no threads, no clock, and no I/O, and both
 * callers use it:
 *
 *   titans_lanes         runs it on the slow-lane thread, under real-time
 *                        pacing, to test the ARCHITECTURE (isolation, budget,
 *                        advisory expiry).
 *   titans_walkforward   runs it single-threaded over a day of trades, to test
 *                        the POLICY (does it predict adverse selection on a day
 *                        it was not calibrated on).
 *
 * Those are different questions. The threaded replay could not separate them --
 * its own verdict was that the informedness it measured was dominated by thread
 * scheduling rather than by the rule.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <numeric>
#include <vector>

namespace titans {
namespace lanes {

/**
 * @brief Trailing signed order-flow imbalance over a fixed trade window.
 *
 * Signed quantity is `aggressor_sign * quantity`: positive when buyers are
 * lifting offers. The net over the last `window` trades is the imbalance, and
 * the policy warns when its magnitude exceeds a calibrated threshold.
 *
 * Direction is carried, not just magnitude. Dropping it was a real defect:
 * adverse selection is directional -- flow that has been buying makes the next
 * aggressive BUY dangerous, not the next sell -- and an undirected advisory
 * fires on both sides and cancels its own signal.
 */
struct FlowPolicyConfig {
    /// Trades in the trailing window.
    std::size_t window = 50;
    /// Warn when |net| exceeds this. Calibrated, never hardcoded: the first
    /// version used a flat 3.0 BTC, which has no defensible relationship to
    /// this symbol, this window, or this session's activity.
    double warn_above = 0.0;
    /// Sign-flip and attenuate the input, standing in for a contaminated view
    /// of order flow. Same class of fault as EntityBinding contamination in
    /// the research framework, in this lane's units.
    bool contaminate = false;
};

struct FlowDecision {
    bool risk_off = false;
    /// +1 when buy pressure dominates, -1 for sell pressure, 0 when flat.
    std::int8_t direction = 0;
    float confidence = 0.0f;
    /// Net signed flow over the window, before thresholding. Exposed so a
    /// calibrator can sample the same quantity the decision uses.
    double net = 0.0;
};

class FlowPolicy {
public:
    // Held at namespace scope rather than nested: a nested class's default
    // member initializers are not available inside the enclosing class's own
    // member declarations, so `Config cfg = {}` as a default argument does not
    // compile. The aliases keep call sites reading as FlowPolicy::Config.
    using Config = FlowPolicyConfig;
    using Decision = FlowDecision;

    explicit FlowPolicy(Config cfg = {}) : cfg_(cfg) {}

    /// @brief Admit one trade's signed quantity into the trailing window.
    void observe(double signed_quantity) {
        const double v = cfg_.contaminate ? -signed_quantity * 0.5 : signed_quantity;
        flow_.push_back(v);
        if (flow_.size() > cfg_.window) flow_.pop_front();
    }

    /// @brief True once the window is full. Deciding before that compares a
    ///        partial sum against a threshold calibrated on full ones.
    bool warm() const { return flow_.size() >= cfg_.window; }

    /**
     * @brief The current decision.
     *
     * The window sum is recomputed rather than maintained incrementally. A
     * running sum over 1.4M updates accumulates floating-point drift, and the
     * calibrated threshold would then be compared against a slightly different
     * quantity than the one that was calibrated. Fifty adds is not the
     * bottleneck on this side of the boundary.
     */
    Decision decide() const {
        Decision d;
        d.net = std::accumulate(flow_.begin(), flow_.end(), 0.0);
        d.risk_off = std::abs(d.net) > cfg_.warn_above;
        d.direction = static_cast<std::int8_t>(d.net > 0 ? 1 : (d.net < 0 ? -1 : 0));
        d.confidence = static_cast<float>(
            std::min(1.0, cfg_.warn_above > 0.0
                              ? std::abs(d.net) / (2.0 * cfg_.warn_above) : 0.0));
        return d;
    }

    /// @brief Size to quote at, given the decision. 1.0 is full size.
    static float size_multiplier(const Decision& d) {
        return d.risk_off ? 0.25f : 1.0f;
    }

    const Config& config() const { return cfg_; }
    void set_warn_above(double v) { cfg_.warn_above = v; }
    void reset() { flow_.clear(); }

private:
    Config cfg_;
    std::deque<double> flow_;
};

/**
 * @brief Collects |net flow| observations and returns a quantile of them.
 *
 * Held separately from the policy because WHERE the samples come from is the
 * whole question a walk-forward asks. In `titans_lanes` they come from the
 * first N observations of the same session the policy is then scored on, which
 * is a prefix -- causal, but drawn from the day being scored. In
 * `titans_walkforward` they come from earlier DAYS, and the day under test
 * contributes nothing to its own threshold.
 *
 * Samples are strided down to `cap` per feed so that an expanding window over
 * a month of trades stays bounded; 200k samples put the 0.95 quantile's
 * sampling error far below the day-to-day variation being measured.
 */
class FlowCalibrator {
public:
    explicit FlowCalibrator(std::size_t cap_per_feed = 200000)
        : cap_(cap_per_feed) {}

    void add(double abs_net) { pending_.push_back(abs_net); }

    /// @brief Fold `pending` into the accumulator, strided down to the cap.
    void commit() {
        if (pending_.empty()) return;
        const std::size_t stride =
            cap_ && pending_.size() > cap_ ? (pending_.size() + cap_ - 1) / cap_ : 1;
        for (std::size_t i = 0; i < pending_.size(); i += stride) {
            samples_.push_back(pending_[i]);
        }
        pending_.clear();
        sorted_ = false;
    }

    std::size_t size() const { return samples_.size(); }

    /**
     * @brief Quantile of the accumulated samples, or 0 when there are none.
     *
     * Returns 0 rather than throwing so that an uncalibrated policy warns on
     * nothing; a threshold of 0 would make |net| > 0 fire on almost every
     * trade, which is why callers must check `size()` before trusting it.
     */
    double quantile(double q) {
        if (samples_.empty()) return 0.0;
        if (!sorted_) { std::sort(samples_.begin(), samples_.end()); sorted_ = true; }
        const std::size_t idx = std::min(
            samples_.size() - 1,
            static_cast<std::size_t>(samples_.size() * q));
        return samples_[idx];
    }

private:
    std::size_t cap_;
    std::vector<double> pending_;
    std::vector<double> samples_;
    bool sorted_ = false;
};

}  // namespace lanes
}  // namespace titans
