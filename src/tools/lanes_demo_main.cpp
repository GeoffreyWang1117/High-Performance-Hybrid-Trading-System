/**
 * @file lanes_demo_main.cpp
 * @brief End-to-end run of both lanes over real market data.
 *
 * Everything else in this repository tests one piece. This runs the whole
 * shape: real Binance trades drive a pinned fast lane under a declared latency
 * budget, the fast lane samples advisories published by a slow lane on another
 * thread, and the outcome is scored against the forward toxic-flow labels.
 *
 * WHAT IS BEING MEASURED
 * ----------------------
 * A market maker's problem with informed flow is adverse selection: you fill a
 * trade, and the price runs away from you. The slow lane's job is to warn the
 * fast lane when flow looks toxic so it can quote smaller. So the score is not
 * "accuracy" in the abstract -- it is:
 *
 *     avoided:  toxic trades the fast lane declined to size into
 *     forgone:  benign trades it needlessly declined
 *
 * both computed against labels derived from what the price actually did next.
 *
 * TWO SLOW LANES
 * --------------
 *   --slow heuristic  trailing signed order-flow imbalance. Cheap, and the
 *                     dataset audit measured its AUC at 0.76.
 *   --slow llm        queries a live model over the same context. Slower by
 *                     five orders of magnitude, which is precisely why it lives
 *                     on this side of the boundary.
 *
 * --contaminate corrupts what the slow lane sees, so the cost of contamination
 * can be read in trading terms rather than in classification terms.
 *
 * REPLAY MUST BE TIME-FAITHFUL
 * ----------------------------
 * The first version of this program replayed as fast as the CPU allowed. That
 * is not a faster version of the same experiment, it is a different one: the
 * fast lane finished 5.2 hours of tape in tens of milliseconds while the slow
 * lane was still on the first few trades, so 97% of advisories were rejected as
 * expired. The rejection was correct -- that is the design working -- but the
 * run measured nothing about the policy.
 *
 * So the fast lane paces itself to the trades' own timestamps, divided by
 * --speed, and the slow lane's simulated model latency is divided by the same
 * factor. What is preserved is the RATIO of model latency to inter-trade
 * interval, which is the quantity the architecture is about. At speed 1 this is
 * real time; at speed 1000 a 50 ms model behaves like a 50 us one against a
 * tape running 1000x faster, and the physics is unchanged.
 */

#include "titans/bench/cycle_timer.hpp"
#include "titans/bench/platform.hpp"
#include "titans/context/binance_dataset.hpp"
#include "titans/lanes/advisory.hpp"
#include "titans/lanes/flow_policy.hpp"
#include "titans/lanes/freshness.hpp"
#include "titans/eval/metrics.hpp"
#include "titans/eval/walk_forward.hpp"
#include "titans/lanes/lane_bridge.hpp"
#include "titans/lanes/latency_budget.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace titans;
using namespace titans::lanes;
using namespace titans::context;

namespace {

struct Options {
    std::string data_path;
    size_t max_rows = 400000;
    int64_t horizon_ms = 1000;
    double threshold_bps = 5.0;
    std::string slow_lane = "heuristic";
    bool contaminate = false;
    /// Advisory lifetime. Short enough that a stalled slow lane stops being
    /// listened to quickly.
    int64_t advisory_ttl_ms = 250;
    /**
     * @brief Reject advice older than this fraction of the signal's horizon.
     *
     * 0 keeps the pre-freshness-gate behaviour, which is what every published
     * number here was measured under. Not defaulted to something helpful until
     * a sweep says which value helps -- picking it first and measuring after is
     * how a tuned constant gets mistaken for a design.
     */
    double max_age_frac = 0.0;
    /// Declared SLO on the p99 of OFFERED advisory age, as a fraction of horizon.
    double slo_p99_frac = 1.0;
    /// Slow lane must publish at least this often, as a fraction of horizon.
    double cadence_frac = 0.25;
    /**
     * @brief What the slow lane ships: the decision, or the parameter behind it.
     *
     * "decision" is the original design and what every earlier number was
     * measured under. "parameter" ships the calibrated threshold and lets the
     * fast lane evaluate the same rule against its own current window.
     */
    std::string advisory_kind = "decision";
    /**
     * @brief Quantile of observed |net flow| above which the slow lane warns.
     *
     * Calibrated from the data rather than set to a constant. The first
     * version used a hardcoded 3.0 BTC, which has no defensible relationship
     * to this symbol, this window length, or this session's activity: it fired
     * on 6.3% of benign trades and caught 1.2% of toxic ones. A quantile at
     * least states the intent -- warn on the busiest N% of flow -- and adapts
     * when the input distribution changes.
     */
    double toxic_flow_quantile = 0.95;
    /// Observations used to calibrate that quantile before warning at all.
    size_t calibration_n = 5000;
    int fast_core = 4;
    int slow_core = 8;
    uint64_t budget_ns = 500;
    /// Replay speed multiplier. 1.0 is real time. See the note above: this
    /// scales the slow lane's latency too, so the ratio that matters is fixed.
    double speed = 1000.0;
    /// Independent replays. One run of a two-thread policy is an anecdote:
    /// whether an advisory happens to be fresh when a toxic trade arrives
    /// depends on thread scheduling. See the verdict at the end of the report.
    int repeat = 3;
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--max-rows" && i + 1 < argc)          o.max_rows = std::atoll(argv[++i]);
        else if (a == "--horizon-ms" && i + 1 < argc)   o.horizon_ms = std::atoll(argv[++i]);
        else if (a == "--threshold-bps" && i + 1 < argc) o.threshold_bps = std::atof(argv[++i]);
        else if (a == "--slow" && i + 1 < argc)         o.slow_lane = argv[++i];
        else if (a == "--contaminate")                  o.contaminate = true;
        else if (a == "--ttl-ms" && i + 1 < argc)       o.advisory_ttl_ms = std::atoll(argv[++i]);
        else if (a == "--budget-ns" && i + 1 < argc)    o.budget_ns = std::atoll(argv[++i]);
        else if (a == "--speed" && i + 1 < argc)        o.speed = std::atof(argv[++i]);
        else if (a == "--repeat" && i + 1 < argc)       o.repeat = std::atoi(argv[++i]);
        else if (a == "--quantile" && i + 1 < argc)     o.toxic_flow_quantile = std::atof(argv[++i]);
        else if (a == "--advisory" && i + 1 < argc)     o.advisory_kind = argv[++i];
        else if (a == "--max-age-frac" && i + 1 < argc) o.max_age_frac = std::atof(argv[++i]);
        else if (a == "--slo-p99-frac" && i + 1 < argc) o.slo_p99_frac = std::atof(argv[++i]);
        else if (a == "--cadence-frac" && i + 1 < argc) o.cadence_frac = std::atof(argv[++i]);
        else if (a == "--fast-core" && i + 1 < argc)    o.fast_core = std::atoi(argv[++i]);
        else if (a == "--slow-core" && i + 1 < argc)    o.slow_core = std::atoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::printf(
                "Usage: titans_lanes <aggTrades.csv> [options]\n"
                "  --slow heuristic|llm    slow-lane implementation\n"
                "  --contaminate           corrupt the slow lane's input\n"
                "  --max-rows N            trades to replay (default 400000)\n"
                "  --horizon-ms N          toxic-flow horizon (default 1000)\n"
                "  --threshold-bps X       toxic-flow threshold (default 5)\n"
                "  --ttl-ms N              advisory lifetime (default 250)\n"
                "  --speed X               replay speed vs real time (default 1000)\n"
                "  --repeat N              independent replays (default 3)\n"
                "  --quantile Q            warn above this |flow| quantile (default 0.95)\n"
                "  --advisory decision|parameter   what the slow lane ships (default decision)\n"
                "  --max-age-frac F        reject advice older than F x horizon (0 = off)\n"
                "  --slo-p99-frac F        declared SLO on offered-age p99 (default 1.0)\n"
                "  --cadence-frac F        slow lane must publish every F x horizon (default 0.25)\n"
                "  --budget-ns N           fast-lane per-event budget (default 500)\n"
                "  --fast-core N --slow-core N   physical cores to pin to\n");
            std::exit(0);
        }
        else if (o.data_path.empty()) o.data_path = a;
    }
    return o;
}

/// @brief What the fast lane decided for one trade, and what actually happened.
struct Outcome {
    uint64_t sized_down = 0;      ///< advisory said reduce
    uint64_t full_size = 0;
    uint64_t avoided_toxic = 0;   ///< sized down AND the trade was toxic
    uint64_t missed_toxic = 0;    ///< full size AND the trade was toxic
    uint64_t forgone_benign = 0;  ///< sized down AND the trade was benign
    uint64_t kept_benign = 0;
};

}  // namespace

int main(int argc, char** argv) {
    const Options opts = parse(argc, argv);
    if (opts.data_path.empty()) {
        std::fprintf(stderr,
            "usage: titans_lanes <aggTrades.csv> [options]\n\n"
            "Get data with:\n"
            "  python python/data/fetch_binance.py --symbol BTCUSDT --date 2024-01-15\n");
        return 2;
    }

    // ------------------------------------------------------------------
    // Data
    // ------------------------------------------------------------------
    ToxicFlowLabelConfig lab;
    lab.horizon_ms = opts.horizon_ms;
    lab.threshold_bps = opts.threshold_bps;

    BinanceToxicFlowDataset ds(lab);
    std::printf("Loading %s\n", opts.data_path.c_str());
    if (!ds.load(opts.data_path, opts.max_rows)) {
        std::fprintf(stderr, "ERROR: %s\n", ds.error().c_str());
        return 1;
    }
    const auto events = ds.build_events();
    if (events.empty()) {
        std::fprintf(stderr, "ERROR: no labelled events\n");
        return 1;
    }
    std::printf("\nDataset\n");
    ds.stats().print();

    // Align each labelled event back to its trade, for quantity and side.
    const auto& trades = ds.trades();
    std::vector<const AggTrade*> aligned;
    aligned.reserve(events.size());
    {
        size_t ti = 0;
        for (const auto& e : events) {
            while (ti < trades.size() &&
                   "agg_" + std::to_string(trades[ti].agg_trade_id) != e.event_id) ++ti;
            if (ti >= trades.size()) break;
            aligned.push_back(&trades[ti++]);
        }
    }
    const size_t n = std::min(events.size(), aligned.size());

    // ------------------------------------------------------------------
    // Lanes
    // ------------------------------------------------------------------
    struct RunResult {
        Outcome outcome;
        uint64_t offered = 0, consumed = 0, dropped = 0;
        uint64_t published = 0, stale_reads = 0, rejected = 0;
        uint64_t budget_count = 0, budget_violations = 0, budget_max_ns = 0;
        double budget_mean_ns = 0.0;
        uint64_t paced = 0;
        int64_t lag_p50 = 0, lag_p99 = 0, lag_max = 0;
        double informedness = 0.0;
        double warn_threshold = 0.0;
        /// Market-time age of the advisory when the fast lane looked at it.
        /// OFFERED is the slow lane's delivery property and the gate cannot
        /// improve it; ACTED is what the gate let through.
        int64_t offered_p50_ms = 0, offered_p99_ms = 0, offered_max_ms = 0;
        int64_t acted_p50_ms = 0, acted_p99_ms = 0;
        uint64_t offered_n = 0, acted_n = 0, gate_rejections = 0;
        bool slo_met = true;
        int64_t declared_horizon_ms = 0;
        /// Slow-lane publish cadence, in market time.
        int64_t cadence_p50_ms = 0, cadence_p99_ms = 0;
        uint64_t cadence_violations = 0, cadence_publishes = 0;
        /// Control: the tape's own inter-trade gaps, in market time. Without
        /// this the cadence numbers cannot be attributed -- a slow lane cannot
        /// publish more often than the market gives it something to say.
        int64_t gap_p50_ms = 0, gap_p99_ms = 0;
        /// Agreement between what the lane delivered and what the same policy
        /// would have decided with no delivery path at all. This is the
        /// instrument that says WHERE the signal goes, rather than confirming
        /// once more that it is gone.
        uint64_t agree = 0, lane_only = 0, ideal_only = 0, both_off = 0;
        double ideal_informedness = 0.0;
    };

    // ------------------------------------------------------------------
    // Reference decisions: the SAME FlowPolicy, run offline over the same
    // trades with no bridge, no thread and no advisory. This is what
    // titans_walkforward measures, computed here so the two can be compared
    // trade by trade instead of only in aggregate.
    //
    // Computed before the replay and read from an array during it, so the fast
    // lane pays one byte-compare rather than running a second policy inside its
    // budget.
    // ------------------------------------------------------------------
    std::vector<uint8_t> ideal_sized_down(n, 0);
    double ideal_informedness = 0.0;
    {
        std::vector<AggTrade> tr;
        std::vector<int8_t> lb;
        tr.reserve(n);
        lb.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            tr.push_back(*aligned[i]);
            lb.push_back(events[i].is_anomaly ? 1 : 0);
        }
        // Threshold from the same data the lane calibrates on, so the
        // comparison isolates DELIVERY and not the choice of threshold.
        lanes::FlowPolicy::Config warm_cfg;
        warm_cfg.window = 50;
        warm_cfg.contaminate = opts.contaminate;
        const auto warm = eval::run_policy_over_day(tr, lb, warm_cfg, true);
        std::vector<double> calib(warm.abs_net.begin(),
                                  warm.abs_net.begin() +
                                      std::min(warm.abs_net.size(), opts.calibration_n));
        std::sort(calib.begin(), calib.end());
        lanes::FlowPolicy::Config cfg = warm_cfg;
        cfg.warn_above = calib.empty() ? 0.0 : calib[std::min(
            calib.size() - 1,
            static_cast<size_t>(calib.size() * opts.toxic_flow_quantile))];

        lanes::FlowPolicy policy(cfg);
        lanes::FlowPolicy::Decision pending;
        bool have_pending = false;
        eval::PolicyOutcome ideal{};
        for (size_t i = 0; i < n; ++i) {
            if (have_pending) {
                const bool sd = pending.risk_off &&
                    pending.direction == static_cast<int8_t>(tr[i].aggressor_sign());
                ideal_sized_down[i] = sd ? 1 : 0;
                if (sd) {
                    if (lb[i] > 0) ++ideal.avoided_toxic; else ++ideal.forgone_benign;
                } else {
                    if (lb[i] > 0) ++ideal.missed_toxic; else ++ideal.kept_benign;
                }
            }
            policy.observe(tr[i].aggressor_sign() * tr[i].quantity);
            if (policy.warm()) { pending = policy.decide(); have_pending = true; }
        }
        ideal_informedness = ideal.informedness();
        std::printf("\nReference (same policy, no delivery path): informedness "
                    "%+.4f, acts on %.2f%% of trades\n"
                    "  threshold |net| > %.4f, from the first %zu per-TRADE "
                    "samples.\n"
                    "  The slow lane calibrates from per-PUBLISH samples, so the "
                    "two thresholds\n  differ slightly and the LEVELS below are "
                    "not directly comparable to this.\n"
                    "  What is comparable is the agreement rate, and the "
                    "decision-vs-parameter\n  comparison, which share a "
                    "threshold to four significant figures.\n",
                    ideal_informedness, ideal.action_rate() * 100.0,
                    cfg.warn_above, opts.calibration_n);
    }

    bench::pin_to_cpu(opts.fast_core);
    const bench::TscClock clock(150);

    const Timestamp replay_epoch = events.front().timestamp;
    std::printf("\nReplaying %zu trades at %.0fx real time (~%.1f s per run, "
                "%d runs)\n",
                n, opts.speed,
                (events[n - 1].timestamp - replay_epoch) / 1e9 / opts.speed,
                opts.repeat);

    auto run_once = [&]() -> RunResult {
        const bool parameter_mode = (opts.advisory_kind == "parameter");
        LaneBridge<4096> bridge;
        AdvisorySlot slot;
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> advisories_published{0};
        std::atomic<double> calibrated_threshold{0.0};
        const Timestamp horizon_ns = opts.horizon_ms * 1000000LL;
        CadenceBudget cadence(static_cast<Timestamp>(
            static_cast<double>(horizon_ns) * opts.cadence_frac));

        // ---- Slow lane -------------------------------------------------
        // Consumes whatever the bridge gives it -- a sample, not the tape --
        // and publishes a stance. It may take as long as it likes; the fast
        // lane never waits on it.
        std::thread slow([&] {
            bench::pin_to_cpu(opts.slow_core);

            // The decision rule itself lives in FlowPolicy, shared with
            // titans_walkforward. Two copies of a rule drift, and then the
            // out-of-sample number describes a policy that does not run here.
            FlowPolicy::Config cfg;
            cfg.window = 50;
            cfg.contaminate = opts.contaminate;
            FlowPolicy policy(cfg);

            // Calibration: hold fire until the distribution of |net flow| is
            // known, then warn above the configured quantile of it. Publishing
            // a stance before that would be warning against a threshold with
            // no meaning.
            //
            // Note where these samples come from: the first N observations of
            // the SAME session this run is then scored on. That is causal, and
            // it is still in-sample. titans_walkforward draws them from earlier
            // DAYS instead, and reports the difference.
            std::vector<double> calib;
            calib.reserve(opts.calibration_n);
            bool calibrated = false;
            // Market time spanned by the calibration sample. This is the
            // parameter's own horizon: a q=0.95 threshold drawn from 5000
            // observations describes the flow distribution over the stretch it
            // was drawn from, and it does not go stale in a second the way a
            // threshold CROSSING does. Declaring one horizon for both is what
            // made valid_until meaningless.
            Timestamp calib_first_ns = 0, calib_last_ns = 0;

            LaneObservation obs;
            while (!stop.load(std::memory_order_relaxed)) {
                // DRAIN TO LATEST, do not process one at a time.
                //
                // A slow lane that consumes the bridge item by item falls
                // further behind on every iteration, so the advisory it
                // eventually publishes describes a market moment that has
                // already passed and the fast lane rejects it as expired.
                // Draining folds the backlog into the flow window and stamps
                // the advisory with the NEWEST observation.
                bool got = false;
                while (bridge.poll(obs)) {
                    got = true;
                    // Contamination, when enabled, is applied inside observe():
                    // the slow lane's view of order flow is sign-flipped and
                    // attenuated. Same class of fault as EntityBinding
                    // contamination in the research framework, in the units
                    // this lane consumes.
                    policy.observe(obs.imbalance);
                }
                if (!got) {
                    std::this_thread::sleep_for(std::chrono::microseconds(20));
                    continue;
                }
                // Deciding on a partial window compares a short sum against a
                // threshold calibrated on full ones.
                if (!policy.warm()) continue;

                FlowPolicy::Decision d = policy.decide();

                if (!calibrated) {
                    if (calib.empty()) calib_first_ns = obs.ingress_time;
                    calib_last_ns = obs.ingress_time;
                    calib.push_back(std::abs(d.net));
                    if (calib.size() < opts.calibration_n) continue;
                    std::sort(calib.begin(), calib.end());
                    const double warn_above = calib[std::min(
                        calib.size() - 1,
                        static_cast<size_t>(calib.size() * opts.toxic_flow_quantile))];
                    policy.set_warn_above(warn_above);
                    calibrated = true;
                    calibrated_threshold.store(warn_above);
                    d = policy.decide();
                }

                Advisory a;
                a.stance = d.risk_off ? AdvisoryStance::RiskOff
                                      : AdvisoryStance::RiskOn;
                a.size_multiplier = FlowPolicy::size_multiplier(d);
                // Which side the pressure is on. Dropping this was a real bug:
                // adverse selection is directional -- flow that has been buying
                // makes the next AGGRESSIVE BUY dangerous, not the next sell --
                // and an undirected advisory made the policy fire on both sides
                // equally, cancelling its own signal.
                a.risk_direction = d.direction;
                a.confidence = d.confidence;
                a.issued_at = obs.ingress_time;
                a.valid_until = obs.ingress_time + opts.advisory_ttl_ms * 1000000LL;
                // The horizon of the claim, not its lifetime. The toxic-flow
                // label asks what the price does within horizon_ms, so that is
                // how far the advice reaches and what its age must be judged
                // against.
                // The horizon of the claim, judged by WHAT IS SHIPPED. A
                // decision is a threshold crossing and is only correct at the
                // instant it is taken, so its horizon is the signal's. A
                // parameter is a distributional summary and its horizon is the
                // stretch of market it was estimated over -- measured, not
                // configured.
                a.signal_horizon_ns =
                    parameter_mode && calib_last_ns > calib_first_ns
                        ? (calib_last_ns - calib_first_ns)
                        : horizon_ns;
                // Ship the parameter alongside the decision, always. It costs
                // 8 bytes in a struct that is already copied whole, and it is
                // what the fast lane uses in --advisory parameter mode.
                a.parameter = policy.config().warn_above;
                a.context_generation = obs.context_generation;
                std::strncpy(a.source, opts.slow_lane.c_str(), sizeof(a.source) - 1);

                slot.publish(a);
                cadence.observe_publish(obs.ingress_time);
                advisories_published.fetch_add(1, std::memory_order_relaxed);

                if (opts.slow_lane == "llm") {
                    // Stand in for model latency, scaled by replay speed so the
                    // ratio of model latency to inter-trade interval matches
                    // what it would be live. The boundary's claim is that this
                    // number can grow without the fast lane noticing.
                    const auto scaled_us = static_cast<long long>(50000.0 / opts.speed);
                    if (scaled_us > 0) {
                        std::this_thread::sleep_for(std::chrono::microseconds(scaled_us));
                    }
                }
            }
        });

        // ---- Fast lane -------------------------------------------------
        LatencyBudget budget("fast lane: observe + advise + size", opts.budget_ns);
        // In parameter mode the fast lane evaluates the rule itself, against a
        // window that is current by construction. Same FlowPolicy class the
        // slow lane uses, so "the same rule" is a fact rather than a claim.
        FlowPolicy::Config fast_cfg;
        fast_cfg.window = 50;
        fast_cfg.contaminate = false;   // the fast lane sees the real tape
        FlowPolicy fast_policy(fast_cfg);

        FreshnessPolicy freshness;
        freshness.max_age_fraction = opts.max_age_frac;
        freshness.slo_p99_fraction = opts.slo_p99_frac;
        AdvisoryView view(slot, AdvisoryStance::RiskOn, freshness);
        Outcome outcome{};
        const uint64_t generation = 1;   // no invalidation events in a flat replay

        const uint64_t wall_start = bench::wall_ns();
        uint64_t paced = 0;
        // Lag distribution rather than a late/on-time count: Binance timestamps
        // are millisecond-resolution and trades cluster inside one millisecond,
        // so every trade after the first in a cluster is trivially "late" while
        // the replay is in fact keeping up. Only the magnitude matters.
        std::vector<int64_t> lag_ns;
        lag_ns.reserve(n);
        uint64_t agree = 0, lane_only = 0, ideal_only = 0, both_off = 0;
        bench::Histogram trade_gaps;
        Timestamp prev_market_ns = 0;


        for (size_t i = 0; i < n; ++i) {
            const auto& e = events[i];
            const auto& t = *aligned[i];

            const uint64_t due = wall_start +
                static_cast<uint64_t>((e.timestamp - replay_epoch) / opts.speed);
            const uint64_t before = bench::wall_ns();
            if (before < due) {
                ++paced;
                while (bench::wall_ns() < due) bench::do_not_optimize(due);
                lag_ns.push_back(0);
            } else {
                lag_ns.push_back(static_cast<int64_t>(before - due));
            }

            if (prev_market_ns != 0 && e.timestamp > prev_market_ns) {
                trade_gaps.record(static_cast<uint64_t>(e.timestamp - prev_market_ns));
            }
            prev_market_ns = e.timestamp;

            BudgetScope scope(budget, clock.ticks_per_ns());

            LaneObservation obs;
            obs.ingress_time = e.timestamp;
            obs.context_generation = generation;
            std::strncpy(obs.symbol, "BTCUSDT", sizeof(obs.symbol) - 1);
            obs.mid_price = to_price(t.price);
            obs.imbalance = t.aggressor_sign() * t.quantity;
            obs.sequence = i;
            bridge.offer(obs);                   // wait-free; drops when full

            // The view records the age distributions itself; see
            // lanes/freshness.hpp for why offered and acted-on are kept apart.
            const Advisory a = view.current(e.timestamp, generation);

            // Act only when the risk side matches this trade's aggressor. An
            // undirected reading fires on both sides and cancels out; see
            // Advisory::risk_direction.
            bool sized_down;
            if (parameter_mode) {
                // The threshold came from the slow lane; the decision is taken
                // here, on a window that ends at the previous trade. Nothing
                // about the decision has had time to go stale.
                if (a.parameter > 0.0 && fast_policy.warm()) {
                    fast_policy.set_warn_above(a.parameter);
                    const FlowPolicy::Decision d = fast_policy.decide();
                    sized_down = d.risk_off && d.direction == t.aggressor_sign();
                } else {
                    sized_down = false;
                }
                fast_policy.observe(t.aggressor_sign() * t.quantity);
            } else {
                sized_down = (a.size_multiplier < 0.5f) &&
                             (a.risk_direction == t.aggressor_sign());
            }

            const bool ideal = ideal_sized_down[i] != 0;
            if (sized_down && ideal)        ++agree;
            else if (sized_down && !ideal)  ++lane_only;
            else if (!sized_down && ideal)  ++ideal_only;
            else                            ++both_off;

            if (sized_down) {
                ++outcome.sized_down;
                if (e.is_anomaly) ++outcome.avoided_toxic; else ++outcome.forgone_benign;
            } else {
                ++outcome.full_size;
                if (e.is_anomaly) ++outcome.missed_toxic; else ++outcome.kept_benign;
            }
        }

        stop.store(true);
        slow.join();

        RunResult r;
        r.outcome = outcome;
        r.offered = bridge.offered();
        r.consumed = bridge.consumed();
        r.dropped = bridge.dropped();
        r.published = advisories_published.load();
        r.stale_reads = view.stale_reads();
        r.rejected = view.rejected();
        r.budget_count = budget.count();
        r.budget_violations = budget.violations();
        r.budget_max_ns = budget.max_ns();
        r.budget_mean_ns = budget.mean_ns();
        r.paced = paced;
        r.warn_threshold = calibrated_threshold.load();

        std::sort(lag_ns.begin(), lag_ns.end());
        auto pct = [&](double p) {
            return lag_ns.empty() ? int64_t{0}
                : lag_ns[std::min(lag_ns.size() - 1,
                                  static_cast<size_t>(lag_ns.size() * p / 100.0))];
        };
        r.lag_p50 = pct(50);
        r.lag_p99 = pct(99);
        r.lag_max = lag_ns.empty() ? 0 : lag_ns.back();

        const auto& fm = view.freshness();
        auto to_ms = [](uint64_t ns) { return static_cast<int64_t>(ns / 1000000); };
        r.offered_p50_ms = to_ms(fm.offered().percentile(50));
        r.offered_p99_ms = to_ms(fm.offered().percentile(99));
        r.offered_max_ms = to_ms(fm.offered().max());
        r.acted_p50_ms   = to_ms(fm.acted().percentile(50));
        r.acted_p99_ms   = to_ms(fm.acted().percentile(99));
        r.offered_n = fm.offered().count();
        r.acted_n = fm.acted().count();
        r.gate_rejections = fm.gate_rejections();
        // Against the horizon the producer actually declared, not the one the
        // command line named: in parameter mode they differ by design.
        Advisory probe;
        const Timestamp declared = slot.try_read(probe) && probe.signal_horizon_ns > 0
                                       ? probe.signal_horizon_ns
                                       : opts.horizon_ms * 1000000LL;
        r.declared_horizon_ms = declared / 1000000;
        r.slo_met = fm.slo_met(view.freshness_policy(), declared);
        r.cadence_p50_ms = to_ms(cadence.intervals().percentile(50));
        r.cadence_p99_ms = to_ms(cadence.intervals().percentile(99));
        r.cadence_violations = cadence.violations();
        r.cadence_publishes = cadence.publishes();
        r.gap_p50_ms = to_ms(trade_gaps.percentile(50));
        r.gap_p99_ms = to_ms(trade_gaps.percentile(99));
        r.agree = agree;
        r.lane_only = lane_only;
        r.ideal_only = ideal_only;
        r.both_off = both_off;
        r.ideal_informedness = ideal_informedness;

        const uint64_t tox = outcome.avoided_toxic + outcome.missed_toxic;
        const uint64_t ben = outcome.forgone_benign + outcome.kept_benign;
        const double tpr = tox ? static_cast<double>(outcome.avoided_toxic) / tox : 0.0;
        const double fpr = ben ? static_cast<double>(outcome.forgone_benign) / ben : 0.0;
        r.informedness = tpr - fpr;
        return r;
    };

    std::vector<RunResult> runs;
    for (int k = 0; k < opts.repeat; ++k) {
        std::printf("  run %d/%d ...", k + 1, opts.repeat);
        std::fflush(stdout);
        runs.push_back(run_once());
        std::printf(" informedness %+.4f\n", runs.back().informedness);
    }

    // ------------------------------------------------------------------
    // Report
    // ------------------------------------------------------------------
    auto fp = bench::MachineFingerprint::capture();
    fp.pinned_cpu = opts.fast_core;
    fp.tsc_ghz = clock.tsc_hz() / 1e9;
    fp.noise_floor_ns = clock.noise_floor_ns();

    const RunResult& last = runs.back();

    std::printf("\nFast lane (pinned, %s slow lane%s)\n",
                opts.slow_lane.c_str(), opts.contaminate ? ", CONTAMINATED" : "");
    std::printf("  stage cost: mean %.1f ns, max %lu ns, budget %lu ns,\n"
                "  %lu of %lu over budget (%.2f%%)\n",
                last.budget_mean_ns, last.budget_max_ns, opts.budget_ns,
                last.budget_violations, last.budget_count,
                last.budget_count ? 100.0 * last.budget_violations / last.budget_count : 0.0);
    std::printf("  Includes two rdtsc reads (~%.0f ns of measurement overhead\n"
                "  on this host) plus the pacing check.\n",
                2 * clock.noise_floor_ns());

    std::printf("\nReplay pacing (%.0fx real time)\n", opts.speed);
    {
        const double scaled_ms = 1e6 * opts.speed;   // replay-ns per market-ms
        std::printf("  %lu of %zu trades waited for their scheduled time\n",
                    last.paced, n);
        std::printf("  lag behind schedule: p50 %ld ns, p99 %ld ns, max %ld ns\n",
                    last.lag_p50, last.lag_p99, last.lag_max);
        std::printf("  = p99 %.5f ms of market time against a %ld ms advisory\n"
                    "  lifetime, so pacing is not the limiting factor.\n",
                    last.lag_p99 / scaled_ms, static_cast<long>(opts.advisory_ttl_ms));
        std::printf("  A trade sharing a millisecond with the one before it is due\n"
                    "  immediately and shows zero wait; that is timestamp\n"
                    "  granularity, not the host falling behind.\n");
    }

    std::printf("\nLane bridge\n");
    std::printf("  offered %lu, consumed %lu, dropped %lu (%.1f%%)\n",
                last.offered, last.consumed, last.dropped,
                last.offered ? 100.0 * last.dropped / last.offered : 0.0);

    std::printf("\nAdvisories\n");
    std::printf("  warn threshold calibrated from data: |net flow| > %.4f "
                "(q=%.2f over %zu observations)\n",
                last.warn_threshold, opts.toxic_flow_quantile, opts.calibration_n);
    std::printf("  published %lu, reads that fell back to cache %lu,\n"
                "  rejected as expired or generation-stale %lu (%.1f%%)\n",
                last.published, last.stale_reads, last.rejected,
                n ? 100.0 * last.rejected / n : 0.0);

    // ------------------------------------------------------------------
    // Freshness contract
    // ------------------------------------------------------------------
    const long H = static_cast<long>(opts.horizon_ms);
    std::printf("\nFRESHNESS, against the %ld ms signal horizon\n", H);
    std::printf("  offered  p50 %6ld ms (%.2fx H), p99 %6ld ms (%.2fx H), "
                "max %6ld ms   over %lu reads\n",
                static_cast<long>(last.offered_p50_ms),
                H ? static_cast<double>(last.offered_p50_ms) / H : 0.0,
                static_cast<long>(last.offered_p99_ms),
                H ? static_cast<double>(last.offered_p99_ms) / H : 0.0,
                static_cast<long>(last.offered_max_ms), last.offered_n);
    if (opts.max_age_frac > 0.0) {
        std::printf("  acted    p50 %6ld ms (%.2fx H), p99 %6ld ms (%.2fx H)"
                    "                  over %lu reads\n",
                    static_cast<long>(last.acted_p50_ms),
                    H ? static_cast<double>(last.acted_p50_ms) / H : 0.0,
                    static_cast<long>(last.acted_p99_ms),
                    H ? static_cast<double>(last.acted_p99_ms) / H : 0.0,
                    last.acted_n);
        std::printf("  gate     rejected %lu of %lu live advisories as older "
                    "than %.2f x H\n",
                    last.gate_rejections, last.offered_n, opts.max_age_frac);
    } else {
        std::printf("  acted    same as offered -- the freshness gate is off "
                    "(--max-age-frac 0)\n");
    }
    std::printf("  OFFERED is the slow lane's delivery property and the gate "
                "cannot improve it,\n  which is why the SLO is declared on it "
                "rather than on what got through.\n");

    const long DH = static_cast<long>(last.declared_horizon_ms);
    std::printf("\n  The producer declared a horizon of %ld ms for what it "
                "ships (%s).\n", DH, opts.advisory_kind.c_str());
    std::printf("  SLO: offered-age p99 <= %.2f x declared = %ld ms ... %s\n",
                opts.slo_p99_frac, static_cast<long>(opts.slo_p99_frac * DH),
                last.slo_met ? "MET" : "BREACHED");
    if (!last.slo_met) {
        std::printf("  Advice reaches the fast lane older than the horizon it\n"
                    "  predicts over. A TTL cannot fix this -- raising it from\n"
                    "  250 ms to 60 s cut rejections from 34.4%% to 10.9%% and\n"
                    "  moved the outcome not at all. See docs/ROADMAP.md P0.\n");
    }

    std::printf("\nSlow-lane publish cadence, in MARKET time\n");
    std::printf("  p50 %ld ms, p99 %ld ms over %lu publishes; budget %.2f x H "
                "= %ld ms\n",
                static_cast<long>(last.cadence_p50_ms),
                static_cast<long>(last.cadence_p99_ms),
                last.cadence_publishes, opts.cadence_frac,
                static_cast<long>(opts.cadence_frac * H));
    std::printf("  %lu gaps over budget (%.2f%%) ... %s\n",
                last.cadence_violations,
                last.cadence_publishes
                    ? 100.0 * last.cadence_violations / last.cadence_publishes : 0.0,
                last.cadence_violations == 0 ? "PASSED" : "BREACHED");
    std::printf("  A slow lane can expire nothing, tear nothing and drop\n"
                "  nothing, and still be useless by not publishing often\n"
                "  enough. Nothing named that failure before this line.\n");
    std::printf("\n  CONTROL -- the tape's own inter-trade gaps: p50 %ld ms, "
                "p99 %ld ms.\n", static_cast<long>(last.gap_p50_ms),
                static_cast<long>(last.gap_p99_ms));
    std::printf("  A slow lane cannot publish more often than the market gives\n"
                "  it something to say. Where the cadence p99 tracks this, the\n"
                "  gap is the market's event sparsity and no amount of\n"
                "  engineering removes it -- the answer is then to REJECT aged\n"
                "  advice, not to chase a cadence the tape cannot supply. Where\n"
                "  cadence exceeds it, the slow lane is genuinely behind.\n");

    // ------------------------------------------------------------------
    // Where the signal goes
    // ------------------------------------------------------------------
    {
        const uint64_t ideal_on = last.agree + last.ideal_only;
        const uint64_t lane_on = last.agree + last.lane_only;
        std::printf("\nDELIVERED vs IDEAL decision, trade by trade\n");
        std::printf("  reference informedness %+.4f, delivered %+.4f\n",
                    last.ideal_informedness, last.informedness);
        std::printf("  ideal sizes down %lu, lane sizes down %lu\n",
                    ideal_on, lane_on);
        std::printf("  both size down      %8lu  (%.1f%% of ideal's actions "
                    "survived delivery)\n", last.agree,
                    ideal_on ? 100.0 * last.agree / ideal_on : 0.0);
        std::printf("  ideal only, lane no %8lu  the lane missed these\n",
                    last.ideal_only);
        std::printf("  lane only, ideal no %8lu  the lane acted where it "
                    "should not\n", last.lane_only);
        std::printf("  neither             %8lu\n", last.both_off);
        std::printf("  Same rule, same trades, same threshold. Everything that\n"
                    "  differs is the delivery path.\n");
    }

    std::printf("\nAdverse selection (labels from what the price did next)\n");
    const uint64_t tox = last.outcome.avoided_toxic + last.outcome.missed_toxic;
    const uint64_t ben = last.outcome.forgone_benign + last.outcome.kept_benign;
    std::printf("  toxic:  %lu   avoided %lu (%.1f%%), missed %lu\n",
                tox, last.outcome.avoided_toxic,
                tox ? 100.0 * last.outcome.avoided_toxic / tox : 0.0,
                last.outcome.missed_toxic);
    std::printf("  benign: %lu   full size %lu (%.1f%%), needlessly reduced %lu\n",
                ben, last.outcome.kept_benign,
                ben ? 100.0 * last.outcome.kept_benign / ben : 0.0,
                last.outcome.forgone_benign);

    // ------------------------------------------------------------------
    // Stability verdict
    // ------------------------------------------------------------------
    std::vector<double> inf;
    for (const auto& r : runs) inf.push_back(r.informedness);
    std::sort(inf.begin(), inf.end());
    const double lo = inf.front(), hi = inf.back();
    const double med = inf[inf.size() / 2];
    const double mean = std::accumulate(inf.begin(), inf.end(), 0.0) / inf.size();
    double sq = 0;
    for (double v : inf) sq += (v - mean) * (v - mean);
    const double sd = inf.size() > 1 ? std::sqrt(sq / (inf.size() - 1)) : 0.0;

    std::printf("\nInformedness (TPR - FPR) across %d runs\n", opts.repeat);
    std::printf("  median %+.4f, range [%+.4f, %+.4f], sd %.4f\n", med, lo, hi, sd);

    std::printf("\nVERDICT\n");
    if (opts.repeat < 3) {
        std::printf("  Too few runs to say anything. Use --repeat 5 or more.\n");
    } else if (std::abs(med) < 2.0 * sd) {
        std::printf("  NOT RESOLVED. The median (%+.4f) is inside two standard\n"
                    "  deviations of the run-to-run spread (%.4f), so this\n"
                    "  configuration does not measure the policy -- it measures\n"
                    "  thread scheduling. Whether an advisory happens to be fresh\n"
                    "  when a toxic trade arrives depends on when the slow lane\n"
                    "  last woke.\n\n"
                    "  Do not quote a single run's number. To get a measurement,\n"
                    "  reduce the dependence on timing: raise --ttl-ms so advice\n"
                    "  stays valid across slow-lane wakeups, or lower --speed so\n"
                    "  the slow lane publishes more often per unit of market\n"
                    "  time.\n", med, sd);
    } else {
        std::printf("  Effect resolved: median %+.4f against a run-to-run sd of\n"
                    "  %.4f. Positive means the advisory steered size away from\n"
                    "  toxic flow more often than it gave up benign flow.\n",
                    med, sd);
    }
    std::printf("\n  %.1f%% of advisories were rejected as expired or\n"
                "  generation-stale. That is the design refusing to act on\n"
                "  outdated model output, not a malfunction -- but a policy that\n"
                "  is ignored most of the time cannot show much effect either\n"
                "  way.\n", n ? 100.0 * last.rejected / n : 0.0);

    std::printf("\n");
    fp.print();

    // A declared contract that cannot fail the run is a comment. Exit 5 keeps
    // the freshness breach distinguishable from a crash (1) or bad usage (2).
    // Only the freshness SLO gates the exit code. The cadence budget is
    // reported with its control rather than enforced, because the tape's own
    // p99 inter-trade gap (1291 ms here) exceeds any cadence a lane could hold:
    // failing a run for not publishing during a silent market would be failing
    // it for physics.
    if (!last.slo_met) {
        std::printf("CONTRACT: freshness SLO BREACHED -> exit 5\n");
        return 5;
    }
    return 0;
}
