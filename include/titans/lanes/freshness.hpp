/**
 * @file freshness.hpp
 * @brief Advisory age as a declared contract, measured against the signal's own
 *        prediction horizon.
 *
 * WHY THIS EXISTS
 * ---------------
 * `Advisory::valid_until` is a wall-clock expiry. It bounds how stale acted-on
 * advice can be, and measurement showed it bounds the wrong quantity.
 *
 * Over 100 000 BTCUSDT trades with a 60-second TTL -- wide enough that almost
 * nothing expired -- the age of the advisory at the moment the fast lane acted
 * on it was p50 82 ms, p99 1507 ms, max 4418 ms, against a 1000 ms toxic-flow
 * prediction horizon. One read in a hundred acted on advice older than the
 * entire horizon it was predicting over, while sitting comfortably inside its
 * declared lifetime. Raising the TTL from 250 ms to 60 s cut the rejection rate
 * from 34.4% to 10.9% and did not move the outcome at all.
 *
 * The same policy scores +0.19 informedness when evaluated without the lane
 * machinery and roughly zero through it. The signal is there; it arrives too
 * old to be worth anything.
 *
 * WHAT THE REST OF THE INDUSTRY CALLS THIS
 * ----------------------------------------
 * Feature freshness. Outside trading it is a solved and named problem: online
 * feature stores register a freshness SLO per feature alongside its owner and
 * lineage, split the feature set by staleness budget rather than applying one
 * globally, and size the serving path backwards from a declared p99. The
 * recurring observation there is the same one measured here -- the model
 * answers in milliseconds while the features it scored are seconds old, because
 * feature pipelines run at 100-1000x the inference latency.
 *
 * THE CONTRACT
 * ------------
 * An advisory declares the horizon of the signal it carries. A consumer
 * declares what fraction of that horizon it is willing to tolerate. Age is then
 * measured in the units that decide whether the advice is worth anything, and
 * the slow lane's publish cadence becomes a budget with a violation count, the
 * way the fast lane's per-event cost already is.
 *
 * Two distributions are kept, and conflating them would hide the whole point:
 *
 *   OFFERED   age of the advisory present when the fast lane looked. This is
 *             the slow lane's delivery property. The gate does not change it,
 *             so this is what the SLO is declared on.
 *   ACTED-ON  age of advice the gate allowed through. Bounded by the gate by
 *             construction, so an SLO on it would be a tautology.
 */

#pragma once

#include "titans/bench/histogram.hpp"
#include "titans/core/types.hpp"

#include <cstdint>

namespace titans {
namespace lanes {

/// @brief What a consumer will accept, expressed against the signal's horizon.
struct FreshnessPolicy {
    /**
     * @brief Reject advice older than this fraction of its declared horizon.
     *
     * 0 disables the gate, which is the behaviour every result in this
     * repository before the gate existed was measured under.
     *
     * A fraction rather than a duration because the tolerable age is a property
     * of the SIGNAL, not of the machine. Advice about what the price does in the
     * next second is worthless at 900 ms old; advice about the next hour is
     * fine. One absolute number cannot express both, which is exactly why
     * `valid_until` could not.
     */
    double max_age_fraction = 0.0;

    /**
     * @brief Declared SLO on the p99 of OFFERED age, as a fraction of horizon.
     *
     * Default 1.0 states the weakest contract that is still obviously right:
     * advice should not be older than the horizon it predicts over. The system
     * currently breaches this, which is the point of declaring it.
     */
    double slo_p99_fraction = 1.0;

    bool gate_enabled() const { return max_age_fraction > 0.0; }

    Timestamp max_age_ns(Timestamp signal_horizon_ns) const {
        return static_cast<Timestamp>(
            static_cast<double>(signal_horizon_ns) * max_age_fraction);
    }
    Timestamp slo_p99_ns(Timestamp signal_horizon_ns) const {
        return static_cast<Timestamp>(
            static_cast<double>(signal_horizon_ns) * slo_p99_fraction);
    }
};

/// @brief Age distributions and the SLO verdict drawn from them.
class FreshnessMonitor {
public:
    void record_offered(Timestamp age_ns) {
        offered_.record(static_cast<uint64_t>(age_ns < 0 ? 0 : age_ns));
    }
    void record_acted(Timestamp age_ns) {
        acted_.record(static_cast<uint64_t>(age_ns < 0 ? 0 : age_ns));
    }
    void record_gate_rejection() { ++gate_rejections_; }

    const bench::Histogram& offered() const { return offered_; }
    const bench::Histogram& acted() const { return acted_; }
    uint64_t gate_rejections() const { return gate_rejections_; }

    /**
     * @brief Whether the offered-age p99 met the declared SLO.
     *
     * Returns true when nothing was offered: an SLO on an empty distribution is
     * vacuous, and reporting "met" for a lane that published nothing would be a
     * lie of a different kind. Callers check `offered().count()` too, and the
     * report below prints it.
     */
    bool slo_met(const FreshnessPolicy& policy, Timestamp signal_horizon_ns) const {
        if (offered_.count() == 0) return true;
        return offered_.percentile(99) <= static_cast<uint64_t>(
                   policy.slo_p99_ns(signal_horizon_ns));
    }

private:
    bench::Histogram offered_;
    bench::Histogram acted_;
    uint64_t gate_rejections_ = 0;
};

/**
 * @brief The slow lane's obligation to keep publishing.
 *
 * A slow lane can satisfy every advisory-level guarantee -- nothing expired,
 * nothing torn, nothing dropped -- and still be useless, by simply not
 * publishing often enough. Nothing in the previous design named that failure,
 * so it did not appear in any report.
 *
 * Intervals are measured in MARKET time, on the timestamp of the newest
 * observation folded into each publish, not in wall time. Under a replay the
 * two differ by the speed factor, and it is the market-time interval that
 * governs how old the advice can get relative to its horizon.
 */
class CadenceBudget {
public:
    /**
     * @param budget_ns Longest tolerable gap between publishes. Derived from
     *                  the horizon by the caller, for the same reason
     *                  FreshnessPolicy takes a fraction.
     */
    explicit CadenceBudget(Timestamp budget_ns = 0) : budget_ns_(budget_ns) {}

    void observe_publish(Timestamp market_time_ns) {
        // `have_last_` rather than `last_publish_ns_ != 0`: a publish at market
        // time zero is a legitimate timestamp, and using it as the "no previous
        // publish" sentinel silently swallowed the first real interval.
        if (have_last_ && market_time_ns > last_publish_ns_) {
            const auto gap = static_cast<uint64_t>(market_time_ns - last_publish_ns_);
            intervals_.record(gap);
            if (budget_ns_ > 0 && gap > static_cast<uint64_t>(budget_ns_)) {
                ++violations_;
            }
        }
        last_publish_ns_ = market_time_ns;
        have_last_ = true;
        ++publishes_;
    }

    const bench::Histogram& intervals() const { return intervals_; }
    uint64_t violations() const { return violations_; }
    uint64_t publishes() const { return publishes_; }
    Timestamp budget_ns() const { return budget_ns_; }
    bool passed() const { return violations_ == 0; }

private:
    Timestamp budget_ns_;
    Timestamp last_publish_ns_ = 0;
    bool have_last_ = false;
    uint64_t violations_ = 0;
    uint64_t publishes_ = 0;
    bench::Histogram intervals_;
};

}  // namespace lanes
}  // namespace titans
