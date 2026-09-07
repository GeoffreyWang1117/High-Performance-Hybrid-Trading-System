/**
 * @file delivered.hpp
 * @brief Scoring advice at the moment it ARRIVES, not the moment it was formed.
 *
 * WHY THIS IS A DIFFERENT NUMBER
 * -----------------------------
 * Every accuracy figure in this repository so far scores a decision against the
 * trade immediately after the one that produced it. That is the right number
 * for "is the rule any good" and the wrong one for "is the rule any good once
 * something has to compute it", because the answer arrives after a delay and
 * the market has moved on. P0 established that a threshold crossing is a
 * moment; this is the machinery for putting a price on missing it.
 *
 * `run_policy_delivered(..., 0)` reproduces `run_policy_over_day` exactly, and
 * `test_zero_delay_reproduces_the_undelayed_evaluator` pins that. Without it
 * this header would be a second, subtly different evaluator, and any comparison
 * between the two would be measuring the difference between two pieces of code
 * rather than the cost of a delay.
 *
 * WHAT IT IS FOR
 * --------------
 * ROADMAP P4: prefill cost scales with context length, so a richer context buys
 * staleness. "More context is better" and "fresher advice is better" are in
 * direct tension, and the tension is only visible when accuracy is measured
 * against the delay rather than against the context size.
 */

#pragma once

#include "titans/context/binance_dataset.hpp"
#include "titans/eval/metrics.hpp"
#include "titans/lanes/flow_policy.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace titans {
namespace eval {

/**
 * @brief First trade strictly after @p from that is at least @p delay_ms later.
 *
 * Strictly after, so a zero delay lands on the next trade rather than on the
 * one that produced the decision -- which is the convention every other
 * evaluator here already uses. Binance stamps to the millisecond and trades
 * cluster inside one, so a non-strict search at zero delay would score a
 * decision against the trade it was derived from.
 *
 * Returns `trades.size()` when the delay runs off the end of the day.
 */
inline std::size_t index_after_delay(const std::vector<context::AggTrade>& trades,
                                     std::size_t from, std::int64_t delay_ms) {
    if (from + 1 >= trades.size()) return trades.size();
    const Timestamp due = trades[from].transact_time_ms + delay_ms;
    std::size_t j = from + 1;
    while (j < trades.size() && trades[j].transact_time_ms < due) ++j;
    return j;
}

/// @brief One delay's worth of outcome, plus what could not be scored.
struct DeliveredOutcome {
    PolicyOutcome outcome;
    /// Decisions whose delivery landed past the end of the day, or on a trade
    /// with no label. Counted rather than dropped silently: at a long enough
    /// delay this becomes most of the sample, and an accuracy computed from
    /// what survives would quietly be an accuracy on a different population.
    std::size_t unscorable = 0;
    std::size_t scored = 0;

    double scorable_fraction() const {
        const std::size_t total = scored + unscorable;
        return total ? static_cast<double>(scored) / static_cast<double>(total) : 0.0;
    }
};

/**
 * @brief Run the flow policy over a day and score each decision on arrival.
 *
 * The decision formed after observing trade *i* becomes available at
 * `t[i] + delay_ms` and is scored against whichever trade is current then --
 * including its aggressor side, because an advisory that does not say which way
 * the risk lies fires on both and cancels itself out (see
 * `Advisory::risk_direction`).
 *
 * The delivery pointer only moves forward, so this is one pass regardless of
 * the delay.
 */
inline DeliveredOutcome run_policy_delivered(
        const std::vector<context::AggTrade>& trades,
        const std::vector<std::int8_t>& labels,
        lanes::FlowPolicy::Config cfg,
        std::int64_t delay_ms) {
    DeliveredOutcome out;
    lanes::FlowPolicy policy(cfg);

    std::size_t deliver = 0;   // monotone: never rewinds
    for (std::size_t i = 0; i < trades.size(); ++i) {
        policy.observe(trades[i].aggressor_sign() * trades[i].quantity);
        if (!policy.warm()) continue;

        const lanes::FlowPolicy::Decision d = policy.decide();

        const Timestamp due = trades[i].transact_time_ms + delay_ms;
        if (deliver < i + 1) deliver = i + 1;
        while (deliver < trades.size() && trades[deliver].transact_time_ms < due) ++deliver;

        if (deliver >= trades.size() || labels[deliver] < 0) {
            ++out.unscorable;
            continue;
        }
        ++out.scored;

        const bool sized_down =
            d.risk_off &&
            d.direction == static_cast<std::int8_t>(trades[deliver].aggressor_sign());
        const bool toxic = labels[deliver] > 0;
        if (sized_down) {
            if (toxic) ++out.outcome.avoided_toxic;
            else       ++out.outcome.forgone_benign;
        } else {
            if (toxic) ++out.outcome.missed_toxic;
            else       ++out.outcome.kept_benign;
        }
    }
    return out;
}

// ============================================================================
// Did the context actually reach the model?
// ============================================================================

/**
 * @brief Whether a context-length sweep's prompts really grew.
 *
 * A serving stack asked for more context than it is configured to hold does not
 * complain. It truncates, answers normally, and the sweep then measures the
 * truncation: prefill time plateaus, the answer stops changing, and the curve
 * reads "more context does not help". That conclusion would be about the
 * server's settings and would look exactly like a finding about the model.
 *
 * The check is on tokens actually prefilled, reported by the backend, against
 * the context the caller asked for. An arm whose prompt did not grow in
 * proportion is refused rather than plotted.
 *
 * A NOTE ON WHAT IS COMPARED
 * --------------------------
 * Not the raw token counts. A prompt is a fixed system message plus a variable
 * body, and at small context sizes the fixed part dominates: eight trades and
 * sixty-four trades differed by 8x in content and only 2.5x in total tokens on
 * the first run of this check, which flagged a perfectly healthy sweep as
 * truncated. The overhead is estimated from the two smallest arms and the rest
 * are tested against the line it implies. Fewer than three arms cannot test
 * anything and are reported as unchecked rather than as passing.
 *
 * @param context_units what was asked for, per arm, strictly increasing
 * @param prompt_tokens what the backend said it prefilled, per arm
 * @param min_ratio how far below the predicted token count an arm may fall
 *        before it is called truncated
 *
 * WHAT IT WILL MISS
 * -----------------
 * Mild truncation. The tolerance exists because tokenisation is not exactly
 * linear in trades, so an arm that lost a tenth of its prompt passes. The check
 * finds where truncation becomes large, not where it begins, and
 * `test_growth_check_catches_a_plateau` asserts exactly that rather than
 * pretending otherwise.
 */
struct ContextGrowth {
    bool checked = false;
    bool truncated = false;
    /// First arm whose prompt fell short. Meaningful only when truncated.
    std::size_t first_bad_arm = 0;
    /// Fitted fixed overhead and per-unit cost, in tokens.
    double overhead_tokens = 0.0;
    double tokens_per_unit = 0.0;
    double predicted_at_bad = 0.0;
    std::string reason;
};

inline ContextGrowth check_context_growth(const std::vector<std::size_t>& context_units,
                                          const std::vector<double>& prompt_tokens,
                                          double min_ratio = 0.85) {
    ContextGrowth g;
    const std::size_t n = std::min(context_units.size(), prompt_tokens.size());
    if (n < 3) {
        g.reason = "need at least three arms to fit an overhead and test against it";
        return g;
    }
    if (context_units[1] <= context_units[0]) {
        g.reason = "context sizes are not increasing";
        return g;
    }
    g.tokens_per_unit = (prompt_tokens[1] - prompt_tokens[0]) /
                        static_cast<double>(context_units[1] - context_units[0]);
    g.overhead_tokens = prompt_tokens[0] -
                        g.tokens_per_unit * static_cast<double>(context_units[0]);
    if (!(g.tokens_per_unit > 0.0)) {
        g.truncated = true;
        g.checked = true;
        g.first_bad_arm = 1;
        g.reason = "the second arm prefilled no more tokens than the first, so the "
                   "prompt was already being cut at the smallest context tested";
        return g;
    }
    g.checked = true;

    for (std::size_t i = 2; i < n; ++i) {
        const double predicted = g.overhead_tokens +
            g.tokens_per_unit * static_cast<double>(context_units[i]);
        if (prompt_tokens[i] < min_ratio * predicted) {
            g.truncated = true;
            g.first_bad_arm = i;
            g.predicted_at_bad = predicted;
            g.reason = "arm " + std::to_string(i) + " should have prefilled about " +
                       std::to_string(static_cast<long long>(predicted)) +
                       " tokens and prefilled " +
                       std::to_string(static_cast<long long>(prompt_tokens[i])) +
                       "; the prompt was cut before it reached the model";
            return g;
        }
    }
    return g;
}

}  // namespace eval
}  // namespace titans
