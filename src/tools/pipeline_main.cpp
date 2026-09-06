/**
 * @file pipeline_main.cpp
 * @brief Tick-to-trade latency: five real stages, end to end, with attribution.
 *
 * WHAT THIS ADDS THAT titans_benchmark CANNOT
 * -------------------------------------------
 * Every latency number this repository has published is a PER-OPERATION cost
 * measured by amortized batch timing: `try_push` 1.21 ns, `update_level`
 * 24.03 ns. Those are correct and they are not a system. Batch timing is
 * structurally incapable of seeing a stall -- it divides total time by a
 * repetition count -- and there was no composite figure anywhere for how long a
 * trade takes to become an order.
 *
 * This runs the real path over the real tape:
 *
 *   ingest   the row's fields -> internal event, fixed-point conversion
 *   book     L2OrderBook::update_level + top-of-book read
 *   signal   AdvisoryView::current + FlowPolicy evaluation, with the slow lane
 *            running on another core, and the candidate quote built from it
 *   risk     RiskManager::check_order
 *   order    id assignment and hand-off to the outbound queue
 *
 * The strategy logic is INSIDE the measured path on purpose. Databento's page
 * on tick-to-trade names the opposite as the standard pitfall: "tick-to-trade
 * latency is usually measured without any strategy or model logic. Hence, it
 * can be misleading since some low latency trading strategies compute a very
 * large number of features."
 *
 * THE THREE END-TO-END NUMBERS
 * ----------------------------
 * `service` is handler work; `response` measures from when the tick was DUE, so
 * it carries the queueing a stall causes; `corrected` is `service` put through
 * the coordinated-omission correction. A replay knows each tick's own due time,
 * so `response` is exact and `corrected` is an estimate of it -- printing both
 * says how good that estimate is on arrivals that are not uniform. See
 * bench/stage_trace.hpp and docs/LATENCY.md.
 *
 * WHAT THE MEASUREMENT COSTS
 * --------------------------
 * A hardware tap would keep the instrument off the critical path. Without one,
 * the next best thing is to know what the probe costs rather than hope it is
 * small. `StageTrace` is templated on whether it stamps at all, so both a
 * probed and an unprobed fast path are compiled with no branch in either, and
 * an unpaced burst of each gives the per-tick difference directly. It is
 * printed in the header of every run.
 */

#include "titans/bench/cycle_timer.hpp"
#include "titans/bench/platform.hpp"
#include "titans/bench/stage_trace.hpp"
#include "titans/context/binance_dataset.hpp"
#include "titans/core/event_bus.hpp"
#include "titans/core/spsc_queue.hpp"
#include "titans/lanes/advisory.hpp"
#include "titans/lanes/flow_policy.hpp"
#include "titans/lanes/freshness.hpp"
#include "titans/lanes/lane_bridge.hpp"
#include "titans/trading/order_book.hpp"
#include "titans/trading/risk_manager.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace titans;
using namespace titans::bench;
using namespace titans::lanes;
using namespace titans::context;

namespace {

// ============================================================================
// Options
// ============================================================================

struct Options {
    std::string data_path;
    size_t max_rows = 400000;
    /// Ticks used for the probe-cost A/B. Unpaced, so this is fast.
    size_t probe_rows = 200000;
    double speed = 1000.0;
    int core = -1;
    int slow_core = -1;
    /// Depth held on each side, in ticks of the trade price grid. An unbounded
    /// synthetic book would grow to tens of thousands of levels over a day and
    /// its cache behaviour would stop resembling a real one.
    int64_t depth_levels = 64;
    size_t calibration_n = 5000;
    double toxic_flow_quantile = 0.95;
    int64_t horizon_ms = 1000;
    int64_t advisory_ttl_ms = 60000;
    /// End-to-end service p99 budget. 0 disables the verdict.
    uint64_t budget_ns = 0;
    /// Replay the same trades at each of these accelerations, to find where
    /// the pipeline stops keeping up. Empty disables the sweep.
    std::vector<double> sweep;
    std::string json_path;
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? argv[++i] : std::string();
        };
        if (a == "--data") o.data_path = next();
        else if (a == "--rows") o.max_rows = std::stoul(next());
        else if (a == "--probe-rows") o.probe_rows = std::stoul(next());
        else if (a == "--speed") o.speed = std::stod(next());
        else if (a == "--core") o.core = std::stoi(next());
        else if (a == "--slow-core") o.slow_core = std::stoi(next());
        else if (a == "--depth") o.depth_levels = std::stoll(next());
        else if (a == "--budget-ns") o.budget_ns = std::stoull(next());
        else if (a == "--json") o.json_path = next();
        else if (a == "--sweep") {
            o.sweep = {125, 250, 500, 1000, 2000, 8000, 32000, 64000};
        }
        else if (a[0] != '-' && o.data_path.empty()) o.data_path = a;
        else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: titans_ticktotrade --data <aggTrades.csv> [options]\n\n"
                "  --rows N          trades to replay (default 400000)\n"
                "  --probe-rows N    trades for the probe-cost A/B (default 200000)\n"
                "  --speed X         replay speed multiplier (default 1000)\n"
                "  --core N          pin the fast lane here\n"
                "  --slow-core N     pin the slow lane here\n"
                "  --depth N         book levels held per side (default 64)\n"
                "  --budget-ns N     fail if end-to-end service p99 exceeds this\n"
                "  --sweep           also replay at 125x..64000x to find saturation\n"
                "  --json PATH       write the distributions as JSON\n");
            std::exit(0);
        }
    }
    return o;
}

// ============================================================================
// Fixed pipeline state
// ============================================================================

/// @brief Everything the fast path touches, constructed once per pass.
struct Pipeline {
    Symbol symbol{"BTCUSDT"};
    L2OrderBook book{Symbol("BTCUSDT")};
    EventBus bus;
    RiskManager risk;
    /// The wire. On the heap: `Order` is cache-line aligned, so a 4096-slot
    /// ring is ~512 KB and a stack-resident one overflows the default 8 MB
    /// stack once the rest of the pipeline is beside it. `EventBus` heap-
    /// allocates its own queue for the same reason.
    std::unique_ptr<SPSCQueue<Order, 4096>> outbound;
    FlowPolicy policy;

    uint64_t next_order_id = 1;
    uint64_t orders_sent = 0;
    uint64_t risk_rejected = 0;
    uint64_t wire_full = 0;
    uint64_t sized_down = 0;

    Pipeline(const RiskLimits& limits, FlowPolicy::Config cfg)
        : risk(bus, limits),
          outbound(std::make_unique<SPSCQueue<Order, 4096>>()),
          policy(cfg) {}
};

/**
 * @brief Nanosecond offsets from the first trade, with the millisecond grid
 *        removed.
 *
 * Binance records `transact_time` to the millisecond and trades cluster inside
 * one. Replaying against the raw stamps makes every trade after the first in a
 * cluster due at the same instant, so it is late before the handler starts and
 * the response-time tail measures the exchange's timestamp resolution rather
 * than this pipeline. Spreading a cluster evenly across its millisecond keeps
 * the recorded order, uses only information the feed actually carries, and is
 * the maximum-entropy choice among the arrival times consistent with it.
 *
 * `titans_lanes` met the same problem and answered it differently -- it reports
 * a lag distribution and refuses a late/on-time count -- because there the
 * quantity of interest was the outcome, not the schedule.
 */
std::vector<double> arrival_offsets(const std::vector<AggTrade>& trades,
                                    size_t n, size_t* clustered) {
    std::vector<double> out(n, 0.0);
    const Timestamp epoch_ms = trades.front().transact_time_ms;
    size_t shared = 0;
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && trades[j].transact_time_ms == trades[i].transact_time_ms) ++j;
        const size_t k = j - i;
        const double base = static_cast<double>(trades[i].transact_time_ms - epoch_ms) * 1e6;
        for (size_t q = 0; q < k; ++q) {
            out[i + q] = base + 1e6 * static_cast<double>(q) / static_cast<double>(k);
        }
        if (k > 1) shared += k - 1;
        i = j;
    }
    if (clustered) *clustered = shared;
    return out;
}

/**
 * @brief Risk limits chosen so the checks RUN rather than short-circuit.
 *
 * `check_order` returns at the first failure, so a limit that trips turns the
 * risk stage into a measurement of its own reject path. Two settings matter:
 *
 *   - The rate limiter counts against the WALL clock. Replaying a day at speed
 *     1000 offers orders at ~13k/s, so the default 10/s would reject almost
 *     everything after the first millisecond of each second -- an artifact of
 *     accelerated replay, not of the system.
 *   - Price deviation is measured against a reference price seeded once. Over a
 *     day BTC moves further than the default 1%, so the check would begin
 *     rejecting partway through and stop exercising the branches after it.
 *
 * Both are raised, and both are stated in the output, because a risk stage that
 * rejects everything is cheap for reasons that have nothing to do with latency.
 */
RiskLimits permissive_limits() {
    RiskLimits l;
    l.max_orders_per_second = 1000000000;
    l.max_orders_per_minute = 1000000000;
    l.max_price_deviation_pct = 1e9;
    l.max_gross_exposure_usd = 1e15;
    l.max_net_exposure_usd = 1e15;
    l.max_position_size = to_quantity(1e9);
    l.max_order_size = to_quantity(1e9);
    l.max_daily_loss_usd = 1e15;
    return l;
}

// ============================================================================
// The replay
// ============================================================================

struct PassResult {
    uint64_t wall_ns = 0;
    uint64_t ticks = 0;
    uint64_t orders_sent = 0;
    uint64_t risk_rejected = 0;
    uint64_t wire_full = 0;
    uint64_t sized_down = 0;
    uint64_t behind = 0;      ///< ticks the handler reached after they were due
    uint64_t advisories_read = 0;
};

/**
 * @brief One pass down the pipeline for every trade.
 *
 * @tparam Instrumented Selects a probed or unprobed fast path at compile time.
 *         Both are instantiated; the caller picks. There is no per-tick branch
 *         in either, which is what makes the difference between them a
 *         measurement of the probe rather than of a predicted branch.
 * @param paced When true the handler waits for each tick's due time, which is
 *         the only way `response` means anything. When false it runs flat out,
 *         which is what the probe-cost A/B wants and what an outcome
 *         measurement must never use (see the header of lanes_demo_main.cpp).
 */
template <bool Instrumented>
PassResult replay(const std::vector<AggTrade>& trades,
                  size_t n,
                  const std::vector<uint64_t>& due_ticks,
                  uint64_t base_tick,
                  uint64_t expected_interval_ticks,
                  bool paced,
                  LaneBridge<>& bridge,
                  const AdvisorySlot& slot,
                  const Options& opts,
                  PipelineHistograms& hists) {
    FlowPolicy::Config cfg;
    cfg.window = 50;
    cfg.contaminate = false;
    Pipeline p(permissive_limits(), cfg);

    // Seed a position and a reference price. `on_price_update` returns early
    // when the symbol has no position, so without this the price-deviation
    // branch inside check_order never executes and the risk stage would be
    // timed with one of its checks switched off.
    p.risk.on_fill(p.symbol, Side::Buy, to_quantity(0.001),
                   to_price(trades[0].price));
    p.risk.on_price_update(p.symbol, to_price(trades[0].price * 0.9999),
                           to_price(trades[0].price * 1.0001));

    FreshnessPolicy freshness;   // gate off; P0 measured that it buys nothing
    AdvisoryView view(slot, AdvisoryStance::RiskOn, freshness);

    const Quantity base_qty = to_quantity(0.01);
    const Price tick_size = to_price(0.01);
    const uint64_t generation = 1;

    PassResult r;
    r.ticks = n;

    const uint64_t wall0 = wall_ns();
    for (size_t i = 0; i < n; ++i) {
        if (paced) {
            const uint64_t due = due_ticks[i];
            if (rdtsc_end() < due) {
                while (rdtsc_end() < due) do_not_optimize(due);
            } else {
                ++r.behind;
            }
        }

        StageTrace<Instrumented> tr;
        tr.open(paced ? due_ticks[i] : base_tick);

        // -- ingest ------------------------------------------------------
        const AggTrade& t = trades[i];
        const Price px = to_price(t.price);
        const Quantity qty = to_quantity(t.quantity);
        const int sign = t.aggressor_sign();
        const Side side = (sign > 0) ? Side::Buy : Side::Sell;
        const Timestamp ts = t.transact_time_ms * 1000000LL;
        tr.mark(Stage::Ingest);

        // -- book --------------------------------------------------------
        // One level in, one level out, so depth stays bounded. The second call
        // is the trim; a quantity of 0 is the erase path.
        p.book.update_level(side, px, qty);
        p.book.update_level(side, px - sign * opts.depth_levels * tick_size, 0);
        const auto bb = p.book.best_bid();
        const auto ba = p.book.best_ask();
        tr.mark(Stage::Book);

        // -- signal ------------------------------------------------------
        LaneObservation obs;
        obs.ingress_time = ts;
        obs.context_generation = generation;
        std::strncpy(obs.symbol, "BTCUSDT", sizeof(obs.symbol) - 1);
        obs.mid_price = px;
        obs.imbalance = sign * t.quantity;
        obs.sequence = i;
        bridge.offer(obs);

        const Advisory a = view.current(ts, generation);
        if (a.parameter > 0.0) p.policy.set_warn_above(a.parameter);
        bool risk_off = false;
        if (p.policy.warm()) {
            const FlowPolicy::Decision d = p.policy.decide();
            risk_off = d.risk_off && d.direction == sign;
        }
        p.policy.observe(sign * t.quantity);

        Order cand;
        cand.symbol = p.symbol;
        // Quote the other side of the trade that just printed: this is a maker
        // sizing its resting quote, which is what the advisory scales.
        cand.side = (side == Side::Buy) ? Side::Sell : Side::Buy;
        cand.type = OrderType::Limit;
        cand.price = (cand.side == Side::Buy)
                         ? (bb ? bb->price : px)
                         : (ba ? ba->price : px);
        cand.quantity = risk_off ? base_qty / 4 : base_qty;
        tr.mark(Stage::Signal);

        // -- risk --------------------------------------------------------
        const RiskCheckResult rc = p.risk.check_order(cand);
        tr.mark(Stage::Risk);

        // -- order -------------------------------------------------------
        if (rc.passed) {
            cand.id = p.next_order_id++;
            cand.created_at = ts;
            cand.updated_at = ts;
            if (p.outbound->try_push(cand)) ++p.orders_sent;
            else ++p.wire_full;
        } else {
            ++p.risk_rejected;
        }
        tr.mark(Stage::Order);

        if constexpr (Instrumented) {
            hists.fold(tr, expected_interval_ticks);
        }
        if (risk_off) ++p.sized_down;

        // Drain the wire outside the measured region so the queue never fills
        // and turns a push into a different operation partway through the run.
        Order sunk;
        while (p.outbound->try_pop(sunk)) do_not_optimize(sunk);
    }
    r.wall_ns = wall_ns() - wall0;
    r.orders_sent = p.orders_sent;
    r.risk_rejected = p.risk_rejected;
    r.wire_full = p.wire_full;
    r.sized_down = p.sized_down;
    r.advisories_read = view.freshness().offered().count();
    return r;
}

/**
 * @brief How late the handler is when it has nothing to do.
 *
 * A control, and the table is not readable without it. The loop below waits for
 * exactly the same due times as the real run and does no work at all, so
 * whatever lateness it records is the host: timer interrupts, the scheduler,
 * the sibling hyperthread, an SMI. Response times at or below this floor say
 * nothing about the pipeline.
 *
 * Without this control the low-speed rows of the saturation sweep read as a
 * pipeline that cannot keep up even at 1/8000th of its capacity, which would be
 * wrong -- the floor is flat there because it is the machine.
 */
Histogram measure_schedule_jitter(const std::vector<uint64_t>& due, size_t n) {
    Histogram h;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t d = due[i];
        while (rdtsc_end() < d) do_not_optimize(d);
        const uint64_t now = rdtsc_end();
        h.record(now > d ? now - d : 0);
    }
    return h;
}

// ============================================================================
// Reporting
// ============================================================================

void print_stage_table(const PipelineHistograms& h, const LatencyFormatter& f) {
    const double total_mean = h.service().mean();

    std::printf("\nPER-STAGE (%llu ticks)\n",
                static_cast<unsigned long long>(h.count()));
    std::printf("  %-8s %13s %13s %13s %13s %13s %8s\n",
                "stage", "mean", "p50", "p90", "p99", "p99.9", "share");
    std::printf("  %s\n", std::string(88, '-').c_str());
    for (unsigned i = 0; i < kStageCount; ++i) {
        const Histogram& s = h.stage(i);
        const double share = total_mean > 0.0 ? 100.0 * s.mean() / total_mean : 0.0;
        std::printf("  %-8s %13s %13s %13s %13s %13s %7.1f%%\n",
                    stage_name(i),
                    f.mean_cell(s).c_str(),
                    f.cell(s, 50).c_str(),
                    f.cell(s, 90).c_str(),
                    f.cell(s, 99).c_str(),
                    f.cell(s, 99.9).c_str(),
                    share);
    }
    std::printf("  %s\n", std::string(88, '-').c_str());
    std::printf("  %-8s %13.1f ns  (sum of stage means; end-to-end service mean %.1f ns)\n",
                "sum", f.to_ns(static_cast<uint64_t>(h.stage_mean_sum())),
                f.to_ns(static_cast<uint64_t>(total_mean)));
    std::printf(
        "\n  The share column uses means, which add. The percentile columns do\n"
        "  NOT add: the tick holding a stage's p99 is rarely the tick holding\n"
        "  another's. Read them as separate tails, not as a decomposition.\n"
        "  \"under floor\" is a refusal, not a small number: below %.1f ns the\n"
        "  histogram is recording the probe rather than the stage.\n",
        f.resolvable_above_ns());
}

void print_end_to_end(const PipelineHistograms& h, const Histogram& jitter,
                      const LatencyFormatter& f) {
    auto row = [&](const char* name, const Histogram& g) {
        std::printf("  %-10s %13s %13s %13s %13s %13s\n", name,
                    f.mean_cell(g).c_str(),
                    f.cell(g, 50).c_str(),
                    f.cell(g, 90).c_str(),
                    f.cell(g, 99).c_str(),
                    f.cell(g, 99.9).c_str());
    };

    std::printf("\nEND TO END\n");
    std::printf("  %-10s %13s %13s %13s %13s %13s\n",
                "", "mean", "p50", "p90", "p99", "p99.9");
    std::printf("  %s\n", std::string(88, '-').c_str());
    row("service", h.service());
    row("corrected", h.corrected());
    row("response", h.response());
    row("queueing", h.queueing());
    std::printf("  %s\n", std::string(88, '.').c_str());
    row("host floor", jitter);

    const double svc99 = f.to_ns(h.service().percentile(99));
    const double cor99 = f.to_ns(h.corrected().percentile(99));
    const double rsp99 = f.to_ns(h.response().percentile(99));

    std::printf(
        "\n  service    handler work only. A closed-loop benchmark reports this,\n"
        "             and a stall cannot move it: while the system is frozen it\n"
        "             takes no samples.\n"
        "  corrected  service with the coordinated-omission correction applied\n"
        "             against the expected inter-tick interval.\n"
        "  response   measured from when each tick was DUE. Exact, because the\n"
        "             replay knows its own schedule.\n"
        "  queueing   response minus service: time a tick spent already late.\n"
        "  host floor CONTROL. The same schedule with no pipeline attached, so this\n"
        "             is the machine's own lateness. Response figures at or below\n"
        "             it are not measuring this system.\n");

    const uint64_t synthesised = h.corrected().count() - h.service().count();

    std::printf("\n  The correction synthesised %llu samples on top of %llu real ones.\n",
                static_cast<unsigned long long>(synthesised),
                static_cast<unsigned long long>(h.service().count()));

    const double floor99 = f.to_ns(jitter.percentile(99));
    if (rsp99 <= floor99) {
        std::printf(
            "\n  [below the host floor] response p99 (%.0f ns) does not clear the\n"
            "  control (%.0f ns). Whatever this run measured, it was not the\n"
            "  pipeline. Quiet the host, or raise --speed until the queue is the\n"
            "  larger effect.\n", rsp99, floor99);
    } else if (rsp99 <= svc99 * 1.05) {
        std::printf(
            "\n  [nothing deferred] response p99 (%.0f ns) is within 5%% of service\n"
            "  p99 (%.0f ns). The handler kept up, so this run says nothing about\n"
            "  omission -- there was nothing to defer. Raise --speed until it\n"
            "  falls behind, or run --sweep to find where that happens.\n",
            rsp99, svc99);
    } else if (synthesised == 0) {
        std::printf(
            "\n  [the correction found nothing, and it was wrong]\n"
            "  response p99 is %.1fx service p99: ticks waited, and a number that\n"
            "  timed only the handler would have reported %.0f ns for a path whose\n"
            "  real tail is %.0f ns. The correction still fired zero times, because\n"
            "  it looks for a SERVICE time longer than the expected interval and\n"
            "  there was none -- the handler is fast, and the queue built from\n"
            "  arrivals bunching, not from any one call blocking.\n"
            "\n  That is the limit of the technique, stated precisely: Tene's\n"
            "  correction repairs a tail suppressed by a slow handler. It cannot\n"
            "  see a tail caused by burst arrivals, and it reports success either\n"
            "  way. Knowing each request's own arrival time is not a refinement of\n"
            "  the correction; it is what the correction is substituting for.\n",
            rsp99 / svc99, svc99, rsp99);
    } else {
        const double err = 100.0 * (cor99 - rsp99) / rsp99;
        std::printf(
            "\n  [deferral observed, correction fired] response p99 is %.1fx service\n"
            "  p99, so a handler-only number would have hidden the tail. The\n"
            "  correction recovers %.0f ns against an exact %.0f ns, %+.1f%% off.\n"
            "  That error is the price of not knowing your own arrival times.\n"
            "  The tail clears the host floor by %.1fx, so it is queueing and not\n"
            "  the machine.\n",
            rsp99 / svc99, cor99, rsp99, err, rsp99 / floor99);
    }
}

void write_json(const std::string& path, const PipelineHistograms& h,
                const Histogram& jitter, const LatencyFormatter& f,
                const MachineFingerprint& fp, const PassResult& run,
                double probe_ns, const Options& opts, size_t clustered) {
    std::ofstream o(path);
    if (!o) {
        std::fprintf(stderr, "warning: could not write %s\n", path.c_str());
        return;
    }
    auto dist = [&](const Histogram& g) {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "{ \"n\": %llu, \"mean_ns\": %.1f, \"p50_ns\": %.1f, \"p90_ns\": %.1f,"
            " \"p99_ns\": %.1f, \"p999_ns\": %.1f, \"max_ns\": %.1f }",
            static_cast<unsigned long long>(g.count()),
            f.to_ns(static_cast<uint64_t>(g.mean())),
            f.to_ns(g.percentile(50)), f.to_ns(g.percentile(90)),
            f.to_ns(g.percentile(99)), f.to_ns(g.percentile(99.9)),
            f.to_ns(g.max()));
        return std::string(buf);
    };

    o << "{\n";
    o << "  \"schema\": \"titans.ticktotrade.v1\",\n";
    o << "  \"machine\": " << fp.to_json(4) << ",\n";
    o << "  \"resolvable_above_ns\": " << f.resolvable_above_ns() << ",\n";
    o << "  \"probe_cost_ns_per_tick\": " << probe_ns << ",\n";
    o << "  \"speed\": " << opts.speed << ",\n";
    o << "  \"ticks\": " << run.ticks << ",\n";
    o << "  \"ticks_already_late\": " << run.behind << ",\n";
    o << "  \"trades_sharing_a_millisecond\": " << clustered << ",\n";
    o << "  \"synthesised_by_correction\": "
      << (h.corrected().count() - h.service().count()) << ",\n";
    o << "  \"orders_sent\": " << run.orders_sent << ",\n";
    o << "  \"risk_rejected\": " << run.risk_rejected << ",\n";
    o << "  \"stages\": {\n";
    for (unsigned i = 0; i < kStageCount; ++i) {
        o << "    \"" << stage_name(i) << "\": " << dist(h.stage(i))
          << (i + 1 < kStageCount ? ",\n" : "\n");
    }
    o << "  },\n";
    o << "  \"end_to_end\": {\n";
    o << "    \"service\": " << dist(h.service()) << ",\n";
    o << "    \"corrected\": " << dist(h.corrected()) << ",\n";
    o << "    \"response\": " << dist(h.response()) << ",\n";
    o << "    \"queueing\": " << dist(h.queueing()) << ",\n";
    o << "    \"host_floor_control\": " << dist(jitter) << "\n";
    o << "  }\n";
    o << "}\n";
    std::printf("\nWrote %s\n", path.c_str());
}

}  // namespace

// ============================================================================

int main(int argc, char** argv) {
    const Options opts = parse(argc, argv);
    if (opts.data_path.empty()) {
        std::fprintf(stderr,
            "usage: titans_ticktotrade --data <aggTrades.csv> [options]\n\n"
            "Get data with:\n"
            "  python python/data/fetch_binance.py --symbol BTCUSDT --date 2024-01-15\n");
        return 2;
    }

    if (opts.core >= 0 && !pin_to_cpu(opts.core)) {
        std::fprintf(stderr, "warning: could not pin to core %d\n", opts.core);
    }
    const TscClock clock;
    const MachineFingerprint fp = MachineFingerprint::capture();

    std::printf("TICK-TO-TRADE\n");
    std::printf("  %s\n", clock.describe().c_str());
    for (const auto& c : fp.caveats()) std::printf("  [caveat] %s\n", c.c_str());

    // ------------------------------------------------------------------
    // Data. Labels are not needed here; this measures the path, not the
    // outcome. titans_lanes and titans_walkforward measure the outcome.
    // ------------------------------------------------------------------
    BinanceToxicFlowDataset ds;
    std::printf("\nLoading %s\n", opts.data_path.c_str());
    if (!ds.load(opts.data_path, opts.max_rows)) {
        std::fprintf(stderr, "ERROR: %s\n", ds.error().c_str());
        return 1;
    }
    const std::vector<AggTrade>& trades = ds.trades();
    const size_t n = trades.size();
    if (n < 1000) {
        std::fprintf(stderr, "ERROR: %zu trades is too few to measure\n", n);
        return 1;
    }
    std::printf("  %zu trades, %.1f minutes of tape, replayed at %.0fx\n",
                n,
                static_cast<double>(trades.back().transact_time_ms -
                                    trades.front().transact_time_ms) / 60000.0,
                opts.speed);

    std::printf(
        "\n  [caveat] aggTrades carries no book depth. The book stage is fed\n"
        "  levels synthesised from trades and trimmed to %lld per side. The\n"
        "  TIMING is real -- the real structure, real level counts, real cache\n"
        "  behaviour -- but the book's CONTENTS are not a real book, and no\n"
        "  claim here depends on them being one.\n",
        static_cast<long long>(opts.depth_levels));

    // ------------------------------------------------------------------
    // Probe cost. Two unpaced bursts, identical except that one stamps.
    // ------------------------------------------------------------------
    const size_t probe_n = std::min(opts.probe_rows, n);
    double probe_ns_per_tick = 0.0;
    {
        std::vector<uint64_t> no_due(1, 0);
        Advisory seed;
        seed.stance = AdvisoryStance::RiskOn;
        seed.parameter = 1.0;
        seed.issued_at = trades[0].transact_time_ms * 1000000LL;
        seed.valid_until = seed.issued_at + 86400LL * 1000000000LL;

        PipelineHistograms sink;
        double bare = 0.0, probed = 0.0;
        // Two repetitions each, alternating, so a drifting machine shows up as
        // disagreement between repetitions rather than as a probe cost.
        for (int rep = 0; rep < 2; ++rep) {
            LaneBridge<> b1, b2;
            AdvisorySlot s1, s2;
            s1.publish(seed);
            s2.publish(seed);
            PipelineHistograms h_bare;
            const PassResult a =
                replay<false>(trades, probe_n, no_due, 0, 0, false, b1, s1, opts, h_bare);
            const PassResult b =
                replay<true>(trades, probe_n, no_due, 0, 0, false, b2, s2, opts, sink);
            bare += static_cast<double>(a.wall_ns) / probe_n;
            probed += static_cast<double>(b.wall_ns) / probe_n;
        }
        bare /= 2.0;
        probed /= 2.0;
        probe_ns_per_tick = probed - bare;

        std::printf("\nPROBE COST (%zu ticks, unpaced, 2 repetitions each)\n", probe_n);
        std::printf("  without stamps  %8.1f ns/tick\n", bare);
        std::printf("  with stamps     %8.1f ns/tick\n", probed);
        std::printf("  probe           %8.1f ns/tick over %u boundaries"
                    " (%.1f ns each)\n",
                    probe_ns_per_tick, kStageCount + 1,
                    probe_ns_per_tick / (kStageCount + 1));
        std::printf(
            "\n  A hardware tap keeps the instrument off the critical path.\n"
            "  Without one the honest substitute is to measure what the probe\n"
            "  costs, not to assume it is negligible. Every stage figure below\n"
            "  includes roughly one boundary read.\n");
    }

    // ------------------------------------------------------------------
    // Schedule. Everything in TSC ticks: a division per tick to reach
    // nanoseconds would be probe added in order to measure probe.
    // ------------------------------------------------------------------
    size_t clustered = 0;
    const std::vector<double> offsets_ns = arrival_offsets(trades, n, &clustered);
    std::printf("\n  %zu of %zu trades (%.1f%%) share a millisecond with the one\n"
                "  before them. Binance stamps to the millisecond; left as recorded,\n"
                "  every trade after the first in a cluster is due at the same instant\n"
                "  and is trivially late, so the response tail would be measuring the\n"
                "  exchange's timestamp resolution rather than this pipeline. Each\n"
                "  millisecond's trades are spread evenly across it, which preserves\n"
                "  their order and is the maximum-entropy choice given what the feed\n"
                "  records.\n",
                clustered, n, 100.0 * static_cast<double>(clustered) / static_cast<double>(n));

    // A schedule is built fresh for each acceleration, so the sweep below runs
    // the same trades against a different arrival process rather than a
    // rescaled copy of one schedule.
    //
    // The expected inter-arrival interval drives the coordinated-omission
    // correction, and it is the MEDIAN rather than the mean: the tape's gap
    // distribution has a tail several orders of magnitude long, and a mean
    // dragged into it would synthesise far too few samples for a stall.
    struct Schedule {
        std::vector<uint64_t> due;
        uint64_t base = 0;
        uint64_t expected_interval_ticks = 0;
    };
    auto build_schedule = [&](double speed) {
        Schedule sc;
        sc.base = rdtsc_start();
        sc.due.resize(n);
        for (size_t i = 0; i < n; ++i) {
            sc.due[i] = sc.base + static_cast<uint64_t>(
                offsets_ns[i] / speed * clock.ticks_per_ns());
        }
        std::vector<uint64_t> gaps;
        gaps.reserve(n);
        for (size_t i = 1; i < n; ++i) {
            if (sc.due[i] > sc.due[i - 1]) gaps.push_back(sc.due[i] - sc.due[i - 1]);
        }
        if (!gaps.empty()) {
            std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
            sc.expected_interval_ticks = gaps[gaps.size() / 2];
        }
        return sc;
    };

    // Control first: the same schedule with no pipeline attached. Built and run
    // before the measured pass so it cannot inherit a warmed cache from it.
    const Histogram jitter = measure_schedule_jitter(build_schedule(opts.speed).due, n);

    const Schedule schedule = build_schedule(opts.speed);
    const uint64_t base_tick = schedule.base;
    const std::vector<uint64_t>& due_ticks = schedule.due;
    const uint64_t expected_interval_ticks = schedule.expected_interval_ticks;
    std::printf("\n  expected inter-tick interval (median): %.0f ns at %.0fx\n",
                static_cast<double>(expected_interval_ticks) / clock.ticks_per_ns(),
                opts.speed);

    // ------------------------------------------------------------------
    // Slow lane. Same shape as titans_lanes: drain to latest, calibrate a
    // threshold, publish it as Advisory::parameter (ROADMAP P0).
    // ------------------------------------------------------------------
    LaneBridge<> bridge;
    AdvisorySlot slot;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> published{0};

    std::thread slow([&] {
        if (opts.slow_core >= 0) pin_to_cpu(opts.slow_core);
        FlowPolicy::Config cfg;
        cfg.window = 50;
        FlowPolicy policy(cfg);
        std::vector<double> calib;
        calib.reserve(opts.calibration_n);
        bool calibrated = false;
        Timestamp calib_first = 0, calib_last = 0;

        LaneObservation obs;
        while (!stop.load(std::memory_order_relaxed)) {
            bool got = false;
            while (bridge.poll(obs)) {
                got = true;
                policy.observe(obs.imbalance);
            }
            if (!got) {
                std::this_thread::sleep_for(std::chrono::microseconds(20));
                continue;
            }
            if (!policy.warm()) continue;

            FlowPolicy::Decision d = policy.decide();
            if (!calibrated) {
                if (calib.empty()) calib_first = obs.ingress_time;
                calib_last = obs.ingress_time;
                calib.push_back(std::abs(d.net));
                if (calib.size() < opts.calibration_n) continue;
                std::sort(calib.begin(), calib.end());
                const double warn = calib[std::min(
                    calib.size() - 1,
                    static_cast<size_t>(calib.size() * opts.toxic_flow_quantile))];
                policy.set_warn_above(warn);
                calibrated = true;
                d = policy.decide();
            }

            Advisory a;
            a.stance = d.risk_off ? AdvisoryStance::RiskOff : AdvisoryStance::RiskOn;
            a.size_multiplier = FlowPolicy::size_multiplier(d);
            a.risk_direction = d.direction;
            a.confidence = d.confidence;
            a.issued_at = obs.ingress_time;
            a.valid_until = obs.ingress_time + opts.advisory_ttl_ms * 1000000LL;
            a.signal_horizon_ns = calib_last > calib_first
                                      ? (calib_last - calib_first)
                                      : opts.horizon_ms * 1000000LL;
            a.parameter = policy.config().warn_above;
            a.context_generation = 1;
            std::strncpy(a.source, "heuristic", sizeof(a.source) - 1);
            slot.publish(a);
            published.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // ------------------------------------------------------------------
    // The measured run.
    // ------------------------------------------------------------------
    PipelineHistograms hists;
    const PassResult run = replay<true>(trades, n, due_ticks, base_tick,
                                        expected_interval_ticks, true,
                                        bridge, slot, opts, hists);

    // ------------------------------------------------------------------
    // Saturation sweep, while the slow lane is still up.
    // ------------------------------------------------------------------
    struct SweepPoint {
        double speed;
        double interval_ns, service99, corrected99, response99;
        double late_pct;
        uint64_t synthesised;
    };
    std::vector<SweepPoint> sweep;
    for (double sp : opts.sweep) {
        const Schedule sc = build_schedule(sp);
        PipelineHistograms h;
        const PassResult pr = replay<true>(trades, n, sc.due, sc.base,
                                           sc.expected_interval_ticks, true,
                                           bridge, slot, opts, h);
        SweepPoint pt;
        pt.speed = sp;
        pt.interval_ns = static_cast<double>(sc.expected_interval_ticks) / clock.ticks_per_ns();
        pt.service99 = static_cast<double>(h.service().percentile(99)) / clock.ticks_per_ns();
        pt.corrected99 = static_cast<double>(h.corrected().percentile(99)) / clock.ticks_per_ns();
        pt.response99 = static_cast<double>(h.response().percentile(99)) / clock.ticks_per_ns();
        pt.late_pct = 100.0 * static_cast<double>(pr.behind) / static_cast<double>(pr.ticks);
        pt.synthesised = h.corrected().count() - h.service().count();
        sweep.push_back(pt);
    }

    stop.store(true);
    slow.join();

    const LatencyFormatter fmt(clock.ticks_per_ns(), clock.noise_floor_ns());
    print_stage_table(hists, fmt);
    print_end_to_end(hists, jitter, fmt);

    std::printf("\nPATH\n");
    std::printf("  orders sent        %10llu\n",
                static_cast<unsigned long long>(run.orders_sent));
    std::printf("  risk rejected      %10llu\n",
                static_cast<unsigned long long>(run.risk_rejected));
    std::printf("  wire full          %10llu\n",
                static_cast<unsigned long long>(run.wire_full));
    std::printf("  quotes sized down  %10llu  (advisory acted on)\n",
                static_cast<unsigned long long>(run.sized_down));
    std::printf("  advisories read    %10llu, published %llu\n",
                static_cast<unsigned long long>(run.advisories_read),
                static_cast<unsigned long long>(published.load()));
    std::printf("  ticks already late %10llu of %llu (%.1f%%)\n",
                static_cast<unsigned long long>(run.behind),
                static_cast<unsigned long long>(run.ticks),
                100.0 * static_cast<double>(run.behind) / static_cast<double>(run.ticks));
    std::printf(
        "\n  Risk limits were raised for this run so check_order exercises every\n"
        "  branch instead of short-circuiting: the rate limiter counts against\n"
        "  the wall clock and would reject nearly everything at %.0fx, and the\n"
        "  price-deviation bound would trip once the tape moved past it.\n",
        opts.speed);

    if (!sweep.empty()) {
        std::printf("\nSATURATION SWEEP (same %zu trades, different arrival rates)\n", n);
        std::printf("  %8s %12s %12s %12s %12s %8s %10s\n",
                    "speed", "interval", "service p99", "corrected", "response p99",
                    "late", "synth");
        std::printf("  %s\n", std::string(80, '-').c_str());
        for (const auto& pt : sweep) {
            std::printf("  %7.0fx %9.0f ns %9.0f ns %9.0f ns %9.0f ns %7.1f%% %10llu\n",
                        pt.speed, pt.interval_ns, pt.service99, pt.corrected99,
                        pt.response99, pt.late_pct,
                        static_cast<unsigned long long>(pt.synthesised));
        }
        std::printf(
            "\n  Accelerating the tape by S is asking the pipeline to handle a market\n"
            "  S times busier with the same burst structure. Read down the table:\n"
            "  service p99 barely moves -- the handler's own work does not depend on\n"
            "  how fast ticks arrive -- while response p99 climbs without bound once\n"
            "  arrivals outpace it. A benchmark reporting only the first column would\n"
            "  show a system with no saturation point at all.\n");
    }

    if (!opts.json_path.empty()) {
        write_json(opts.json_path, hists, jitter, fmt, fp, run,
                   probe_ns_per_tick, opts, clustered);
    }

    if (opts.budget_ns > 0) {
        const double p99 = fmt.to_ns(hists.service().percentile(99));
        std::printf("\nVERDICT\n");
        if (p99 <= static_cast<double>(opts.budget_ns)) {
            std::printf("  end-to-end service p99 %.0f ns <= budget %llu ns ... PASS\n",
                        p99, static_cast<unsigned long long>(opts.budget_ns));
        } else {
            std::printf("  end-to-end service p99 %.0f ns > budget %llu ns ... BREACH\n",
                        p99, static_cast<unsigned long long>(opts.budget_ns));
            return 5;
        }
    }
    return 0;
}
