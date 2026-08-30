/**
 * @file benchmark_main.cpp
 * @brief Latency and throughput benchmarks for the Titans fast path.
 *
 * Every number this program prints is accompanied by (a) the measurement mode
 * used, (b) the machine state that produced it, and (c) the error sources that
 * were NOT controlled. Results are also written as JSON for regression
 * tracking.
 *
 * The first section is a methodology self-check. It reproduces the naive
 * timing approach and shows why it cannot be used at this cost scale; the
 * check FAILS the build's benchmark step if the harness itself is miscalibrated.
 *
 * Usage:
 *   titans_benchmark [--json <path>] [--core <physical_core_index>]
 *                    [--reps <n>] [--quick]
 */

#include "titans/bench/harness.hpp"
#include "titans/core/types.hpp"
#include "titans/core/spsc_queue.hpp"
#include "titans/core/memory_pool.hpp"
#include "titans/core/event_bus.hpp"
#include "titans/trading/order_book.hpp"

#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

using namespace titans;
using namespace titans::bench;

namespace {

struct Options {
    std::string json_path;
    size_t core_index = 4;
    int reps = 15;
    uint64_t batch = 2000000;
    uint64_t per_op_iters = 200000;
};

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--json" && i + 1 < argc) {
            o.json_path = argv[++i];
        } else if (a == "--core" && i + 1 < argc) {
            o.core_index = static_cast<size_t>(std::atoi(argv[++i]));
        } else if (a == "--reps" && i + 1 < argc) {
            o.reps = std::atoi(argv[++i]);
        } else if (a == "--quick") {
            o.reps = 5;
            o.batch = 200000;
            o.per_op_iters = 20000;
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "Usage: titans_benchmark [--json PATH] [--core N] [--reps N] [--quick]\n");
            std::exit(0);
        }
    }
    return o;
}

// ============================================================================
// Section 0: methodology self-check
// ============================================================================

/**
 * @brief Demonstrate that naive clock-bracketed timing is invalid here.
 *
 * Measures, using the SAME naive method the pre-rewrite benchmark used:
 *   (a) the cost of `now_ns()` itself, and
 *   (b) the cost of a trivial operation (an integer increment).
 *
 * If the method were valid, (b) would be far smaller than (a) and both would
 * be stable. In practice (b) comes out comparable to or larger than the real
 * cost of (a), because both are dominated by clock overhead. Reporting (b) as
 * "operation latency" -- which the old benchmark did -- is reporting jitter.
 *
 * @return true if the harness is calibrated and the demonstration held.
 */
bool methodology_self_check(Harness& h) {
    std::printf("\n");
    std::printf("================================================================\n");
    std::printf(" SECTION 0 - MEASUREMENT METHODOLOGY SELF-CHECK\n");
    std::printf("================================================================\n\n");

    std::printf("%s\n\n", h.clock().describe().c_str());

    // (a) Naive measurement of the clock call itself.
    constexpr int kN = 200000;
    std::vector<int64_t> naive(kN);
    for (int i = 0; i < kN; ++i) {
        const auto t0 = now_ns();
        do_not_optimize(now_ns());
        naive[i] = now_ns() - t0;
    }
    std::sort(naive.begin(), naive.end());
    const double naive_clock_p50 = static_cast<double>(naive[kN / 2]);

    // (b) Naive measurement of an integer increment (true cost: << 1 ns).
    volatile int64_t counter = 0;
    std::vector<int64_t> naive_op(kN);
    for (int i = 0; i < kN; ++i) {
        const auto t0 = now_ns();
        counter = counter + 1;
        naive_op[i] = now_ns() - t0;
    }
    std::sort(naive_op.begin(), naive_op.end());
    const double naive_op_p50 = static_cast<double>(naive_op[kN / 2]);
    do_not_optimize(counter);

    // (c) The same increment, measured correctly (amortized).
    volatile int64_t counter2 = 0;
    const auto amortized = h.measure_amortized(
        "integer increment (reference)", 20000000, 5,
        [&counter2] { counter2 = counter2 + 1; });

    std::printf("Naive clock-bracketed timing, as used before this rewrite:\n");
    std::printf("  cost of now_ns() itself, measured with now_ns():   %8.2f ns (p50)\n",
                naive_clock_p50);
    std::printf("  cost of an integer increment, same method:         %8.2f ns (p50)\n",
                naive_op_p50);
    std::printf("  clock granularity floor (rdtsc, this harness):     %8.2f ns\n",
                h.clock().noise_floor_ns());
    std::printf("\nSame integer increment, amortized batch timing:      %8.3f ns/op\n",
                amortized.mean_ns);

    const bool contradiction = (naive_op_p50 >= naive_clock_p50 * 0.5);
    const bool amortized_sane = (amortized.mean_ns < 5.0);

    std::printf("\nVERDICT\n");
    if (contradiction) {
        std::printf(
            "  [confirmed] The naive method reports an integer increment at %.0f ns,\n"
            "  within a factor of two of the %.0f ns clock read used to measure it.\n"
            "  An increment does not cost %.0f ns. The naive method measures the\n"
            "  clock, not the operation, and any per-op figure it produced at this\n"
            "  scale -- including every number in the pre-rewrite README table --\n"
            "  is timing noise.\n",
            naive_op_p50, naive_clock_p50, naive_op_p50);
    } else {
        std::printf("  [unexpected] Naive method did not exhibit the expected floor.\n"
                    "  Clock may be unusually cheap on this host; interpret with care.\n");
    }
    if (amortized_sane) {
        std::printf(
            "  [confirmed] Amortized timing resolves the same increment at %.3f ns/op,\n"
            "  consistent with a single retired ALU op on a %.2f GHz core.\n"
            "  The harness is calibrated.\n",
            amortized.mean_ns, h.fingerprint().tsc_ghz);
    } else {
        std::printf("  [FAIL] Amortized timing returned %.3f ns/op for an integer\n"
                    "  increment. The harness is NOT calibrated; results below are\n"
                    "  not trustworthy.\n", amortized.mean_ns);
    }
    return amortized_sane;
}

// ============================================================================
// Fast-path component benchmarks
// ============================================================================

std::vector<BenchmarkResult> run_spsc(Harness& h, const Options& o) {
    static SPSCQueue<int64_t, 65536> q;
    std::vector<BenchmarkResult> out;

    // Push: drain in setup so the batch never hits a full queue. Batch is
    // capped at capacity-1 because a full queue turns try_push into a
    // different (and much cheaper) operation.
    constexpr uint64_t kCap = 65535;
    out.push_back(h.measure_amortized(
        "SPSCQueue::try_push", kCap, o.reps,
        [] { static int64_t v = 0; do_not_optimize(q.try_push(v++)); },
        [] { int64_t sink; while (q.try_pop(sink)) { do_not_optimize(sink); } }));

    out.push_back(h.measure_amortized(
        "SPSCQueue::try_pop", kCap, o.reps,
        [] { int64_t v; do_not_optimize(q.try_pop(v)); },
        [] {
            int64_t sink; while (q.try_pop(sink)) { do_not_optimize(sink); }
            for (uint64_t i = 0; i < kCap; ++i) q.try_push(static_cast<int64_t>(i));
        }));

    return out;
}

std::vector<BenchmarkResult> run_memory_pool(Harness& h, const Options& o) {
    static ObjectPool<Order, 4096> pool;
    static std::vector<Order*> held;
    held.reserve(4096);
    std::vector<BenchmarkResult> out;

    constexpr uint64_t kBatch = 4000;
    out.push_back(h.measure_amortized(
        "ObjectPool::allocate", kBatch, o.reps,
        [] { Order* p = pool.allocate(); do_not_optimize(p); held.push_back(p); },
        [] { for (Order* p : held) pool.deallocate(p); held.clear(); }));

    out.push_back(h.measure_amortized(
        "ObjectPool::deallocate", kBatch, o.reps,
        [] { if (!held.empty()) { pool.deallocate(held.back()); held.pop_back(); } },
        [] {
            for (Order* p : held) pool.deallocate(p);
            held.clear();
            for (uint64_t i = 0; i < kBatch; ++i) held.push_back(pool.allocate());
        }));

    // Baseline for comparison: the allocator the pool replaces.
    out.push_back(h.measure_amortized(
        "operator new/delete (baseline)", 100000, o.reps,
        [] { Order* p = new Order(); do_not_optimize(p); delete p; }));

    return out;
}

std::vector<BenchmarkResult> run_event_bus(Harness& h, const Options& o) {
    static EventBus bus;
    static uint64_t sink = 0;
    static bool subscribed = false;
    if (!subscribed) {
        bus.subscribe<MarketDataEvent>(
            EventType::MarketDataSnapshot,
            [](const MarketDataEvent& e) { sink += static_cast<uint64_t>(e.last_price); });
        subscribed = true;
    }

    std::vector<BenchmarkResult> out;

    // Pre-stamped: measures dispatch only.
    out.push_back(h.measure_amortized(
        "EventBus::publish (timestamp preset)", 200000, o.reps,
        [] {
            MarketDataEvent e;
            e.type = EventType::MarketDataSnapshot;
            e.timestamp = 1;                 // non-zero: skips the internal clock read
            e.last_price = to_price(50000.0);
            bus.publish(e);
        }));

    // Unstamped: the path an ordinary caller takes. The delta between the two
    // is the cost of the clock_gettime that publish() performs on the hot path.
    out.push_back(h.measure_amortized(
        "EventBus::publish (auto-timestamp)", 200000, o.reps,
        [] {
            MarketDataEvent e;
            e.type = EventType::MarketDataSnapshot;
            e.timestamp = 0;                 // triggers now_ns() inside publish()
            e.last_price = to_price(50000.0);
            bus.publish(e);
        }));

    do_not_optimize(sink);
    return out;
}

std::vector<BenchmarkResult> run_order_book(Harness& h, const Options& o) {
    static L2OrderBook book{Symbol("BTCUSDT")};
    std::vector<BenchmarkResult> out;

    // Steady-state update over a bounded set of levels: the realistic pattern
    // for an L2 feed. Growing the book without bound would measure map growth.
    constexpr uint64_t kLevels = 50;
    out.push_back(h.measure_amortized(
        "L2OrderBook::update_level (steady state)", 100000, o.reps,
        [] {
            static uint64_t i = 0;
            const int lvl = static_cast<int>(i++ % kLevels);
            book.update_level(Side::Buy, to_price(50000.0 - lvl), to_quantity(1.0 + lvl));
        },
        [] {
            for (uint64_t l = 0; l < kLevels; ++l) {
                book.update_level(Side::Buy, to_price(50000.0 - l), to_quantity(1.0));
                book.update_level(Side::Sell, to_price(50001.0 + l), to_quantity(1.0));
            }
        }));

    out.push_back(h.measure_amortized(
        "L2OrderBook::best_bid", 1000000, o.reps,
        [] { do_not_optimize(book.best_bid()); }));

    return out;
}

// ============================================================================
// Output
// ============================================================================

void write_json(const std::string& path,
                const MachineFingerprint& fp,
                const std::vector<BenchmarkResult>& results) {
    std::ofstream f(path);
    if (!f) {
        std::fprintf(stderr, "warning: could not write %s\n", path.c_str());
        return;
    }
    f << "{\n";
    f << "  \"schema\": \"titans.benchmark.v2\",\n";
    f << "  \"machine\": " << fp.to_json(4) << ",\n";
    f << "  \"results\": [\n";
    for (size_t i = 0; i < results.size(); ++i) {
        f << "    " << Harness::result_to_json(results[i], 6);
        if (i + 1 < results.size()) f << ",";
        f << "\n";
    }
    f << "  ]\n";
    f << "}\n";
    std::printf("\nResults written to %s\n", path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    const Options opts = parse_args(argc, argv);

    std::printf("Titans fast-path benchmarks\n");

    Harness h(opts.core_index);
    if (!h.pinned()) {
        std::fprintf(stderr,
                     "warning: failed to pin to a core; measurements will be noisy\n");
    }
    h.fingerprint().print();

    const bool calibrated = methodology_self_check(h);

    std::printf("\n");
    std::printf("================================================================\n");
    std::printf(" FAST-PATH COMPONENT LATENCY\n");
    std::printf("================================================================\n");
    std::printf(
        "\nAll figures below are amortized batch timings: %d repetitions,\n"
        "median across repetitions reported as cost, minimum reported as the\n"
        "interference-free estimate. 'spread' is how far the median sits above\n"
        "the minimum -- large spread means the host was busy, not that the\n"
        "operation is slow. These operations cost less than the %.1f ns timing\n"
        "floor, so no per-operation p99 is measurable and none is reported.\n",
        opts.reps, h.clock().noise_floor_ns());

    std::vector<BenchmarkResult> all;
    for (auto* fn : {&run_spsc, &run_memory_pool, &run_event_bus, &run_order_book}) {
        auto part = (*fn)(h, opts);
        all.insert(all.end(), part.begin(), part.end());
    }

    Harness::print_header();
    for (const auto& r : all) Harness::print_result(r);

    if (!opts.json_path.empty()) write_json(opts.json_path, h.fingerprint(), all);

    if (!calibrated) {
        std::fprintf(stderr, "\nFAILED: harness self-check did not pass.\n");
        return 1;
    }
    std::printf("\nSelf-check passed.\n");
    return 0;
}
