/**
 * @file test_lane_isolation.cpp
 * @brief Executable proof of the fast/slow lane isolation contract.
 *
 * The architecture claims that slow-lane behaviour -- an LLM taking 200 ms, an
 * HTTP timeout, a dead worker -- cannot degrade the fast path. A claim like
 * that is worth nothing unless it fails loudly when violated, so each property
 * below is asserted against a measured threshold rather than described in a
 * document.
 *
 * Properties under test:
 *   1. A hung slow lane does not slow the fast lane.
 *   2. A dead slow lane does not stall the fast lane.
 *   3. The fast->slow bridge drops instead of blocking when full.
 *   4. Expired advisories are rejected.
 *   5. Generation-stale advisories are rejected.
 *   6. The advisory seqlock never hands back a torn read under contention.
 */

#include "titans/lanes/advisory.hpp"
#include "titans/lanes/lane_bridge.hpp"
#include "titans/lanes/latency_budget.hpp"
#include "titans/bench/cycle_timer.hpp"
#include "titans/bench/platform.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace titans;
using namespace titans::lanes;

namespace {

/**
 * @brief Run the fast-path loop and return its p99 per-iteration cost in ns.
 *
 * The loop does what the real fast lane does per event: offer an observation to
 * the bridge and sample the advisory slot. Timing uses rdtsc; the loop body is
 * ~50-100 ns, comfortably above the ~20 ns timing floor, so per-iteration
 * percentiles are meaningful here (unlike for the single-digit-ns primitives in
 * the benchmark suite, which must be measured amortized).
 */
double fast_lane_p99_ns(LaneBridge<256>& bridge,
                        AdvisorySlot& slot,
                        const bench::TscClock& clock,
                        int iterations) {
    AdvisoryView view(slot);
    std::vector<uint64_t> ticks(iterations);
    uint64_t generation = 1;

    for (int i = 0; i < iterations; ++i) {
        const uint64_t t0 = bench::rdtsc_start();

        LaneObservation obs;
        obs.ingress_time = static_cast<Timestamp>(i) + 1;
        obs.context_generation = generation;
        obs.mid_price = 5000000000LL;
        obs.sequence = static_cast<uint64_t>(i);
        bridge.offer(obs);

        const Advisory a = view.current(obs.ingress_time, generation);
        bench::do_not_optimize(a);

        ticks[i] = bench::rdtsc_end() - t0;
    }

    std::sort(ticks.begin(), ticks.end());
    return clock.to_ns(ticks[static_cast<size_t>(iterations * 0.99)]);
}

// --------------------------------------------------------------------------
// Property 1 + 2: slow-lane pathology does not reach the fast lane
// --------------------------------------------------------------------------

bool test_hung_slow_lane_does_not_slow_fast_lane() {
    bench::pin_to_cpu(6);
    const bench::TscClock clock(100);

    constexpr int kIters = 200000;

    // Baseline: slow lane draining normally.
    {
        LaneBridge<256> bridge;
        AdvisorySlot slot;
        std::atomic<bool> stop{false};
        std::thread consumer([&] {
            bench::pin_to_cpu(8);
            LaneObservation o;
            while (!stop.load(std::memory_order_relaxed)) {
                while (bridge.poll(o)) bench::do_not_optimize(o);
                std::this_thread::yield();
            }
        });

        const double baseline_p99 = fast_lane_p99_ns(bridge, slot, clock, kIters);
        stop.store(true);
        consumer.join();

        // Pathological: slow lane sleeps 200 ms per item, the latency of a
        // local 8B model, and holds the advisory slot's writer role.
        LaneBridge<256> bridge2;
        AdvisorySlot slot2;
        std::atomic<bool> stop2{false};
        std::thread hung([&] {
            bench::pin_to_cpu(8);
            LaneObservation o;
            while (!stop2.load(std::memory_order_relaxed)) {
                if (bridge2.poll(o)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    Advisory a;
                    a.stance = AdvisoryStance::RiskOn;
                    a.confidence = 0.8f;
                    a.size_multiplier = 1.0f;
                    a.issued_at = o.ingress_time;
                    a.valid_until = o.ingress_time + 1000000000LL;
                    a.context_generation = o.context_generation;
                    slot2.publish(a);
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        });

        const double hung_p99 = fast_lane_p99_ns(bridge2, slot2, clock, kIters);
        stop2.store(true);
        hung.join();

        const double ratio = hung_p99 / std::max(baseline_p99, 1e-9);
        std::printf("    fast-lane p99: slow lane draining %.1f ns, slow lane hung "
                    "%.1f ns (%.2fx)\n", baseline_p99, hung_p99, ratio);
        // A ratio below 1 is expected here and is not a bug: the draining
        // consumer spins and yields on a sibling core, so it costs the fast
        // lane more than a consumer asleep inside a 200 ms model call does.
        // The assertion that matters is only that the hung case stays bounded.
        std::printf("    bridge drop rate while hung: %.1f%% (%lu of %lu dropped)\n",
                    bridge2.drop_rate() * 100.0, bridge2.dropped(), bridge2.offered());

        // The contract: a 200 ms stall in the slow lane must not move the fast
        // lane's p99 by more than 2x. It should not move at all in principle;
        // 2x leaves room for scheduler noise on a non-isolated host.
        if (ratio > 2.0) {
            std::fprintf(stderr,
                         "FAIL: hung slow lane degraded fast-lane p99 by %.2fx "
                         "(limit 2.0x). Isolation contract is broken.\n", ratio);
            return false;
        }
        // The bridge must have shed load rather than queued it without bound.
        if (bridge2.dropped() == 0) {
            std::fprintf(stderr,
                         "FAIL: slow lane consumed at most a handful of items yet "
                         "nothing was dropped; the bridge is not bounded.\n");
            return false;
        }
    }
    return true;
}

bool test_dead_slow_lane_does_not_stall_fast_lane() {
    bench::pin_to_cpu(6);
    const bench::TscClock clock(100);

    LaneBridge<256> bridge;
    AdvisorySlot slot;

    // No consumer thread at all: the slow lane never existed.
    const double p99 = fast_lane_p99_ns(bridge, slot, clock, 200000);
    std::printf("    fast-lane p99 with no slow lane at all: %.1f ns\n", p99);
    std::printf("    bridge drop rate: %.1f%%\n", bridge.drop_rate() * 100.0);

    if (bridge.drop_rate() < 0.9) {
        std::fprintf(stderr,
                     "FAIL: with no consumer, expected nearly everything to be "
                     "dropped; drop rate was %.2f\n", bridge.drop_rate());
        return false;
    }
    if (p99 > 1000.0) {
        std::fprintf(stderr,
                     "FAIL: fast-lane p99 of %.1f ns with a dead slow lane "
                     "suggests the fast path is waiting on something.\n", p99);
        return false;
    }
    return true;
}

// --------------------------------------------------------------------------
// Property 3: the bridge sheds load, it does not block
// --------------------------------------------------------------------------

bool test_bridge_drops_when_full() {
    LaneBridge<64> bridge;
    LaneObservation obs;
    obs.ingress_time = 1;

    int accepted = 0;
    for (int i = 0; i < 1000; ++i) {
        if (bridge.offer(obs)) ++accepted;
    }

    if (accepted >= 1000) {
        std::fprintf(stderr, "FAIL: a 64-slot bridge accepted %d offers\n", accepted);
        return false;
    }
    if (bridge.dropped() != 1000 - static_cast<uint64_t>(accepted)) {
        std::fprintf(stderr, "FAIL: drop accounting inconsistent\n");
        return false;
    }
    std::printf("    64-slot bridge: accepted %d of 1000, dropped %lu\n",
                accepted, bridge.dropped());
    return true;
}

// --------------------------------------------------------------------------
// Properties 4 + 5: staleness is rejected structurally
// --------------------------------------------------------------------------

bool test_expired_advisory_is_rejected() {
    AdvisorySlot slot;
    AdvisoryView view(slot, AdvisoryStance::Neutral);

    Advisory a;
    a.stance = AdvisoryStance::Halt;      // would stop trading if honoured
    a.confidence = 0.99f;
    a.size_multiplier = 0.0f;
    a.issued_at = 1000;
    a.valid_until = 2000;
    a.context_generation = 7;
    slot.publish(a);

    // Inside the validity window: honoured.
    const Advisory live = view.current(/*now=*/1500, /*generation=*/7);
    if (live.stance != AdvisoryStance::Halt) {
        std::fprintf(stderr, "FAIL: valid advisory was not honoured\n");
        return false;
    }

    // One nanosecond past expiry: must fall back, not halt trading forever.
    const Advisory expired = view.current(/*now=*/2001, /*generation=*/7);
    if (expired.stance != AdvisoryStance::Neutral) {
        std::fprintf(stderr,
                     "FAIL: expired advisory still in force (stance=%d). A model "
                     "conclusion outliving its validity window is exactly the "
                     "failure this design exists to prevent.\n",
                     static_cast<int>(expired.stance));
        return false;
    }
    if (expired.size_multiplier != 1.0f) {
        std::fprintf(stderr, "FAIL: fallback did not restore full size\n");
        return false;
    }
    std::printf("    expired advisory rejected; %lu rejections recorded\n",
                view.rejected());
    return true;
}

bool test_generation_stale_advisory_is_rejected() {
    AdvisorySlot slot;
    AdvisoryView view(slot, AdvisoryStance::Neutral);

    Advisory a;
    a.stance = AdvisoryStance::RiskOn;
    a.size_multiplier = 1.0f;
    a.issued_at = 1000;
    a.valid_until = 9'000'000'000LL;   // far future: expiry alone would not save us
    a.context_generation = 3;
    slot.publish(a);

    // Same generation: honoured.
    if (view.current(1500, 3).stance != AdvisoryStance::RiskOn) {
        std::fprintf(stderr, "FAIL: same-generation advisory was rejected\n");
        return false;
    }

    // The fast lane observed something that invalidated prior context (a book
    // reset, a reconnect) and bumped the generation. The advisory is still
    // within its time window but was reasoned from facts that no longer hold.
    const Advisory stale = view.current(1500, 4);
    if (stale.stance != AdvisoryStance::Neutral) {
        std::fprintf(stderr,
                     "FAIL: advisory from generation 3 survived into generation 4. "
                     "Time-based expiry alone does not catch context invalidation.\n");
        return false;
    }
    std::printf("    generation-stale advisory rejected while still within "
                "its time window\n");
    return true;
}

// --------------------------------------------------------------------------
// Property 6: no torn reads under contention
// --------------------------------------------------------------------------

/**
 * @brief Hammer the slot from one writer and one reader; verify coherence.
 *
 * The writer maintains an invariant that spans four fields: valid_until is
 * always issued_at + 1000, issued_at encodes the generation, and confidence
 * encodes it again. A value assembled from two different publishes breaks the
 * invariant, so any torn read is caught.
 *
 * @param writer_spin  Busy-work iterations between publishes. 0 is adversarial
 *                     (millions of publishes/sec, far beyond anything a model
 *                     can produce, so the reader is lapped constantly and
 *                     mostly gives up). A small value models a fast-but-real
 *                     producer, where reads should nearly always succeed.
 */
struct SlotStressResult { uint64_t reads, torn, gave_up; };

SlotStressResult stress_slot(int writer_spin, int duration_ms) {
    AdvisorySlot slot;
    std::atomic<bool> stop{false};

    std::thread writer([&] {
        uint64_t gen = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            Advisory a;
            a.stance = AdvisoryStance::RiskOn;
            a.issued_at = static_cast<Timestamp>(gen) * 10;
            a.valid_until = a.issued_at + 1000;
            a.context_generation = gen;
            a.confidence = static_cast<float>(gen % 1000);
            slot.publish(a);
            ++gen;
            for (int k = 0; k < writer_spin; ++k) bench::do_not_optimize(k);
        }
    });

    SlotStressResult r{0, 0, 0};
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        Advisory got;
        if (!slot.try_read(got)) { ++r.gave_up; continue; }
        ++r.reads;
        const bool consistent =
            (got.valid_until == got.issued_at + 1000) &&
            (got.issued_at == static_cast<Timestamp>(got.context_generation) * 10) &&
            (got.confidence == static_cast<float>(got.context_generation % 1000));
        if (!consistent) ++r.torn;
    }
    stop.store(true);
    writer.join();
    return r;
}

bool test_advisory_slot_never_tears() {
    // Phase 1: adversarial writer, no pause at all.
    const SlotStressResult hot = stress_slot(/*writer_spin=*/0, /*duration_ms=*/300);
    std::printf("    adversarial writer: %lu reads, %lu torn, %lu lapped give-ups\n",
                hot.reads, hot.torn, hot.gave_up);
    if (hot.torn != 0) {
        std::fprintf(stderr,
                     "FAIL: %lu torn reads under an adversarial writer. Lap "
                     "detection is not sound.\n", hot.torn);
        return false;
    }

    // Phase 2: fast but realistic producer. The slow lane publishes at model
    // speed; even a producer thousands of times faster than that must let the
    // fast lane read successfully essentially always.
    const SlotStressResult warm = stress_slot(/*writer_spin=*/2000, /*duration_ms=*/300);
    const double success =
        warm.reads ? static_cast<double>(warm.reads) / (warm.reads + warm.gave_up) : 0.0;
    std::printf("    realistic writer:   %lu reads, %lu torn, %lu give-ups "
                "(%.4f%% success)\n",
                warm.reads, warm.torn, warm.gave_up, success * 100.0);
    if (warm.torn != 0) {
        std::fprintf(stderr, "FAIL: %lu torn reads at realistic write rate\n", warm.torn);
        return false;
    }
    if (warm.reads == 0) {
        std::fprintf(stderr, "FAIL: no successful reads; test did not exercise the slot\n");
        return false;
    }
    if (success < 0.99) {
        std::fprintf(stderr,
                     "FAIL: only %.2f%% of fast-path reads succeeded against a "
                     "realistic producer; the ring is too shallow.\n", success * 100.0);
        return false;
    }
    return true;
}

}  // namespace

bool run_lane_isolation_tests() {
    struct Case { const char* name; bool (*fn)(); };
    const Case cases[] = {
        {"bridge drops when full",                    test_bridge_drops_when_full},
        {"expired advisory rejected",                 test_expired_advisory_is_rejected},
        {"generation-stale advisory rejected",        test_generation_stale_advisory_is_rejected},
        {"advisory slot never tears",                 test_advisory_slot_never_tears},
        {"dead slow lane does not stall fast lane",   test_dead_slow_lane_does_not_stall_fast_lane},
        {"hung slow lane does not slow fast lane",    test_hung_slow_lane_does_not_slow_fast_lane},
    };

    bool all = true;
    for (const auto& c : cases) {
        std::printf("  [ RUN ] %s\n", c.name);
        const bool ok = c.fn();
        std::printf("  [ %s ] %s\n", ok ? "OK  " : "FAIL", c.name);
        all = all && ok;
    }
    return all;
}
