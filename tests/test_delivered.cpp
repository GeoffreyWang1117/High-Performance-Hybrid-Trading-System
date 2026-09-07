/**
 * @file test_delivered.cpp
 * @brief Scoring on arrival, and the guard that stops a config file from
 *        looking like a finding.
 *
 * The load-bearing test here is the first one. `run_policy_delivered` is a
 * second evaluator for the same policy, and two evaluators that disagree by a
 * little produce a "cost of delay" that is really the difference between two
 * pieces of code. At zero delay it must reproduce `run_policy_over_day`
 * counter for counter, or nothing downstream means what it says.
 *
 * The rest guard the context-growth check, which exists because a serving stack
 * asked for more context than it holds truncates silently. Both directions
 * matter and both have already been wrong once: the first version compared raw
 * token counts and called a healthy 8-to-128-trade sweep truncated, because a
 * fixed system prompt dominates the small arms.
 */

#include "titans/eval/delivered.hpp"
#include "titans/eval/walk_forward.hpp"

#include <cstdio>
#include <vector>

using namespace titans;
using namespace titans::eval;

namespace {

std::vector<context::AggTrade> make_trades(std::size_t n, std::int64_t step_ms) {
    std::vector<context::AggTrade> tr;
    tr.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        context::AggTrade t;
        t.agg_trade_id = i;
        t.price = 100.0 + static_cast<double>(i % 13) * 0.01;
        t.quantity = 1.0 + static_cast<double>(i % 7) * 0.5;
        t.transact_time_ms = 1000 + static_cast<Timestamp>(i) * step_ms;
        t.is_buyer_maker = (i % 3 == 0);
        tr.push_back(t);
    }
    return tr;
}

std::vector<std::int8_t> make_labels(std::size_t n) {
    std::vector<std::int8_t> lb;
    lb.reserve(n);
    for (std::size_t i = 0; i < n; ++i) lb.push_back((i % 9 == 0) ? 1 : 0);
    return lb;
}

// ============================================================================
// The equivalence that makes the delay measurable
// ============================================================================

bool test_zero_delay_reproduces_the_undelayed_evaluator() {
    const auto trades = make_trades(3000, 3);
    const auto labels = make_labels(3000);
    lanes::FlowPolicy::Config cfg;
    cfg.window = 50;
    cfg.warn_above = 2.0;

    const PolicyOutcome a = run_policy_over_day(trades, labels, cfg, false).outcome;
    const PolicyOutcome b = run_policy_delivered(trades, labels, cfg, 0).outcome;

    if (a.avoided_toxic != b.avoided_toxic || a.forgone_benign != b.forgone_benign ||
        a.missed_toxic != b.missed_toxic || a.kept_benign != b.kept_benign) {
        std::fprintf(stderr,
            "FAIL: zero delay does not reproduce the undelayed evaluator\n"
            "      undelayed %llu/%llu/%llu/%llu\n"
            "      delay 0   %llu/%llu/%llu/%llu\n",
            (unsigned long long)a.avoided_toxic, (unsigned long long)a.forgone_benign,
            (unsigned long long)a.missed_toxic, (unsigned long long)a.kept_benign,
            (unsigned long long)b.avoided_toxic, (unsigned long long)b.forgone_benign,
            (unsigned long long)b.missed_toxic, (unsigned long long)b.kept_benign);
        return false;
    }
    return true;
}

/**
 * @brief Delivery is STRICTLY after the trade that produced the decision.
 *
 * Binance stamps to the millisecond and trades cluster inside one, so a
 * non-strict search at zero delay lands on the very trade the decision was
 * derived from -- the decision would then be scored against its own input and
 * every accuracy figure would be inflated.
 */
bool test_delivery_is_strictly_after_its_own_trade() {
    // Ten trades sharing one millisecond, then one a second later.
    std::vector<context::AggTrade> tr;
    for (int i = 0; i < 10; ++i) {
        context::AggTrade t;
        t.transact_time_ms = 5000;
        t.quantity = 1.0;
        tr.push_back(t);
    }
    context::AggTrade late;
    late.transact_time_ms = 6000;
    late.quantity = 1.0;
    tr.push_back(late);

    if (index_after_delay(tr, 0, 0) != 1) {
        std::fprintf(stderr, "FAIL: zero delay did not land on the next trade\n");
        return false;
    }
    // Any positive delay must skip the whole cluster sharing that millisecond.
    if (index_after_delay(tr, 0, 1) != 10) {
        std::fprintf(stderr,
            "FAIL: a 1 ms delay landed at %zu, expected past the tied cluster\n",
            index_after_delay(tr, 0, 1));
        return false;
    }
    // Running off the end is reported as the end, not clamped to the last trade.
    if (index_after_delay(tr, 0, 999999) != tr.size()) {
        std::fprintf(stderr, "FAIL: a delay past the end did not return size()\n");
        return false;
    }
    return true;
}

bool test_delay_moves_the_target_forward() {
    const auto trades = make_trades(500, 10);   // 10 ms apart
    std::size_t prev = 0;
    for (const std::int64_t d : {0, 5, 10, 50, 100, 1000}) {
        const std::size_t j = index_after_delay(trades, 100, d);
        if (j < prev) {
            std::fprintf(stderr, "FAIL: target went backwards at delay %lld\n",
                         static_cast<long long>(d));
            return false;
        }
        prev = j;
    }
    // 100 ms at 10 ms spacing is ten trades on.
    if (index_after_delay(trades, 100, 100) != 110) {
        std::fprintf(stderr, "FAIL: 100 ms at 10 ms spacing landed at %zu, expected 110\n",
                     index_after_delay(trades, 100, 100));
        return false;
    }
    return true;
}

/// @brief A delay that runs off the end is counted, never quietly dropped.
bool test_unscorable_decisions_are_counted() {
    const auto trades = make_trades(400, 10);   // 4 seconds of tape
    const auto labels = make_labels(400);
    lanes::FlowPolicy::Config cfg;
    cfg.window = 50;
    cfg.warn_above = 2.0;

    const DeliveredOutcome near = run_policy_delivered(trades, labels, cfg, 0);
    const DeliveredOutcome far = run_policy_delivered(trades, labels, cfg, 2000);

    if (far.unscorable <= near.unscorable) {
        std::fprintf(stderr,
            "FAIL: a 2 s delay on 4 s of tape did not increase the unscorable count"
            " (%zu vs %zu)\n", far.unscorable, near.unscorable);
        return false;
    }
    if (near.scored + near.unscorable != far.scored + far.unscorable) {
        std::fprintf(stderr,
            "FAIL: the two runs made different numbers of decisions (%zu vs %zu);\n"
            "      the delay must move where a decision is scored, not whether\n"
            "      it is made\n",
            near.scored + near.unscorable, far.scored + far.unscorable);
        return false;
    }
    if (far.scorable_fraction() >= 1.0) {
        std::fprintf(stderr, "FAIL: scorable fraction did not fall below 1\n");
        return false;
    }
    return true;
}

// ============================================================================
// The truncation guard, both directions
// ============================================================================

/**
 * @brief A healthy sweep must not be called truncated.
 *
 * This is the bug the check shipped with: comparing raw token counts, an 8 to
 * 128 trade sweep grows 16x in content and only 4x in total tokens, because a
 * ~200 token system prompt is most of the smallest arm. The first version
 * called that truncation and would have refused every real run.
 */
bool test_growth_check_tolerates_a_fixed_overhead() {
    const std::vector<std::size_t> ctx{8, 32, 128, 512};
    std::vector<double> tokens;
    for (const std::size_t c : ctx) tokens.push_back(201.0 + 7.0 * static_cast<double>(c));

    const ContextGrowth g = check_context_growth(ctx, tokens);
    if (!g.checked) {
        std::fprintf(stderr, "FAIL: four arms were not checked: %s\n", g.reason.c_str());
        return false;
    }
    if (g.truncated) {
        std::fprintf(stderr, "FAIL: a clean sweep was called truncated: %s\n",
                     g.reason.c_str());
        return false;
    }
    if (std::abs(g.overhead_tokens - 201.0) > 1e-6 ||
        std::abs(g.tokens_per_unit - 7.0) > 1e-6) {
        std::fprintf(stderr, "FAIL: fitted %.2f + %.4f/unit, expected 201 + 7\n",
                     g.overhead_tokens, g.tokens_per_unit);
        return false;
    }
    return true;
}

/// @brief And a prompt that stops growing must be caught.
bool test_growth_check_catches_a_plateau() {
    const std::vector<std::size_t> ctx{8, 32, 128, 512};
    // The server's window caps the prompt at 1000 tokens.
    std::vector<double> tokens;
    for (const std::size_t c : ctx) {
        tokens.push_back(std::min(1000.0, 201.0 + 7.0 * static_cast<double>(c)));
    }

    const ContextGrowth g = check_context_growth(ctx, tokens);
    if (!g.truncated) {
        std::fprintf(stderr,
            "FAIL: a prompt capped at 1000 tokens was not detected as truncated\n");
        return false;
    }
    // Arm 2 is ALREADY truncated -- it wanted 1097 tokens and got 1000 -- and
    // the check does not flag it, because a 9% shortfall is inside the
    // tolerance that exists for tokenisation not being exactly linear in
    // trades. The check finds where truncation becomes large, not where it
    // begins. That is a real limitation and it is asserted here rather than
    // tuned away, so that tightening the tolerance later is a deliberate act
    // with a failing test attached.
    if (g.first_bad_arm != 3) {
        std::fprintf(stderr,
            "FAIL: blamed arm %zu, expected 3 (arm 2 is mildly truncated and\n"
            "      falls inside the tolerance on purpose)\n", g.first_bad_arm);
        return false;
    }
    if (g.reason.empty()) {
        std::fprintf(stderr, "FAIL: truncation carried no reason\n");
        return false;
    }
    return true;
}

/// @brief Too few arms to fit a line is UNCHECKED, which is not the same as passing.
bool test_growth_check_refuses_with_too_few_arms() {
    const std::vector<std::size_t> ctx{8, 32};
    const std::vector<double> tokens{257.0, 425.0};
    const ContextGrowth g = check_context_growth(ctx, tokens);
    if (g.checked) {
        std::fprintf(stderr, "FAIL: two arms were reported as checked\n");
        return false;
    }
    if (g.truncated) {
        std::fprintf(stderr, "FAIL: an unchecked sweep was called truncated\n");
        return false;
    }
    if (g.reason.empty()) {
        std::fprintf(stderr, "FAIL: refusal carried no reason; unchecked and clean\n"
                             "      are indistinguishable without one\n");
        return false;
    }
    return true;
}

/// @brief A window so small that even the second arm is cut.
bool test_growth_check_catches_truncation_at_the_second_arm() {
    const std::vector<std::size_t> ctx{8, 32, 128};
    const std::vector<double> tokens{300.0, 300.0, 300.0};
    const ContextGrowth g = check_context_growth(ctx, tokens);
    if (!g.truncated || !g.checked) {
        std::fprintf(stderr,
            "FAIL: a prompt that never grew at all was not caught (checked=%d)\n",
            g.checked);
        return false;
    }
    return true;
}

struct Case {
    const char* name;
    bool (*fn)();
};

const Case kCases[] = {
    {"zero delay reproduces the undelayed evaluator", test_zero_delay_reproduces_the_undelayed_evaluator},
    {"delivery is strictly after its own trade", test_delivery_is_strictly_after_its_own_trade},
    {"delay moves the target forward", test_delay_moves_the_target_forward},
    {"unscorable decisions are counted", test_unscorable_decisions_are_counted},
    {"growth check tolerates a fixed overhead", test_growth_check_tolerates_a_fixed_overhead},
    {"growth check catches a plateau", test_growth_check_catches_a_plateau},
    {"growth check refuses with too few arms", test_growth_check_refuses_with_too_few_arms},
    {"growth check catches truncation at arm two", test_growth_check_catches_truncation_at_the_second_arm},
};

}  // namespace

bool run_delivered_tests() {
    bool all = true;
    for (const auto& c : kCases) {
        const bool ok = c.fn();
        std::printf("  [%s] %s\n", ok ? "pass" : "FAIL", c.name);
        if (!ok) all = false;
    }
    return all;
}
