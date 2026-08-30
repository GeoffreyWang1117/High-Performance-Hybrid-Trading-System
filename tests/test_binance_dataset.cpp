/**
 * @file test_binance_dataset.cpp
 * @brief Verifies aggTrades parsing and forward-label construction.
 *
 * The forward "toxic flow" label is easy to get quietly wrong in ways that
 * invert or flatten every downstream result:
 *
 *   - Binance's `is_buyer_maker` is the PASSIVE side. `true` means the buyer
 *     was the maker, so the SELLER was the aggressor. Reading it as "buyer was
 *     the aggressor" inverts every label while leaving the code plausible.
 *   - Trades in the final `horizon_ms` have no future to look at. Labelling
 *     them negative appends a block of guaranteed normals to every run.
 *   - The drift correction must move both sides toward the same expectation,
 *     not just shift a threshold.
 *
 * None of these produce a crash or an obviously wrong number, so they are
 * checked here against hand-computed cases rather than trusted.
 */

#include "titans/context/binance_dataset.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace titans;
using namespace titans::context;

namespace {

/// @brief Write a CSV in Binance aggTrades layout and return its path.
std::string write_fixture(const std::string& name, const std::string& body) {
    const char* tmp = std::getenv("TMPDIR");
    const std::string path =
        std::string(tmp ? tmp : "/tmp") + "/titans_" + name + ".csv";
    std::ofstream f(path);
    f << body;
    return path;
}

/// Columns: agg_id, price, qty, first_id, last_id, time_ms, is_buyer_maker, best
std::string row(uint64_t id, double price, double qty, long long t, bool buyer_maker) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%llu,%.8f,%.8f,%llu,%llu,%lld,%s,True\n",
                  static_cast<unsigned long long>(id), price, qty,
                  static_cast<unsigned long long>(id), static_cast<unsigned long long>(id),
                  t, buyer_maker ? "True" : "False");
    return buf;
}

// --------------------------------------------------------------------------

bool test_parsing() {
    const std::string path = write_fixture("parse",
        row(100, 41732.35, 0.00099, 1705276800002, true) +
        row(101, 41733.00, 0.50000, 1705276800500, false) +
        row(102, 41734.00, 1.25000, 1705276801000, true));

    BinanceToxicFlowDataset ds;
    if (!ds.load(path)) {
        std::fprintf(stderr, "FAIL: load: %s\n", ds.error().c_str());
        return false;
    }
    const auto& t = ds.trades();
    if (t.size() != 3) {
        std::fprintf(stderr, "FAIL: expected 3 trades, got %zu\n", t.size());
        return false;
    }
    if (t[0].agg_trade_id != 100 || t[0].price != 41732.35 ||
        t[0].transact_time_ms != 1705276800002) {
        std::fprintf(stderr, "FAIL: field mismatch on first trade\n");
        return false;
    }
    if (std::abs(t[1].quantity - 0.5) > 1e-9) {
        std::fprintf(stderr, "FAIL: quantity parsed as %.8f\n", t[1].quantity);
        return false;
    }
    std::printf("      parsed 3 trades with correct ids, prices, times\n");
    return true;
}

/**
 * @brief The aggressor is the side that was NOT the maker.
 *
 * This convention decides the sign of every label, so it is asserted directly.
 */
bool test_aggressor_convention() {
    AggTrade buyer_maker;
    buyer_maker.is_buyer_maker = true;    // buyer passive => SELLER aggressed
    AggTrade seller_maker;
    seller_maker.is_buyer_maker = false;  // buyer aggressed

    if (buyer_maker.aggressor_sign() != -1) {
        std::fprintf(stderr,
                     "FAIL: is_buyer_maker=true means the seller was the "
                     "aggressor, so the sign must be -1; got %d. Every label "
                     "would be inverted.\n", buyer_maker.aggressor_sign());
        return false;
    }
    if (seller_maker.aggressor_sign() != +1) {
        std::fprintf(stderr, "FAIL: is_buyer_maker=false should give +1, got %d\n",
                     seller_maker.aggressor_sign());
        return false;
    }
    std::printf("      is_buyer_maker=true -> -1 (seller aggressed); "
                "false -> +1\n");
    return true;
}

/**
 * @brief Forward label on a hand-built price path, drift correction off.
 *
 * Path: 100.00 at t=0, then 100.10 at t=1000 (a +10 bps move).
 * An aggressive BUY at t=0 is followed by a favourable +10 bps -> toxic at a
 * 5 bps threshold. An aggressive SELL at the same instant sees -10 bps in its
 * own frame -> not toxic.
 */
bool test_forward_label_direction() {
    const std::string path = write_fixture("fwd",
        row(1, 100.00, 1.0, 0,    false) +   // aggressive BUY
        row(2, 100.00, 1.0, 0,    true)  +   // aggressive SELL, same instant
        row(3, 100.10, 1.0, 1000, false) +   // +10 bps later
        row(4, 100.10, 1.0, 3000, false));   // tail, gives the first three a horizon

    ToxicFlowLabelConfig cfg;
    cfg.horizon_ms = 1000;
    cfg.threshold_bps = 5.0;
    cfg.drift_adjust = false;   // isolate direction from the correction

    BinanceToxicFlowDataset ds(cfg);
    if (!ds.load(path)) {
        std::fprintf(stderr, "FAIL: load: %s\n", ds.error().c_str());
        return false;
    }
    const auto events = ds.build_events();

    bool buy_toxic = false, sell_toxic = false, found_buy = false, found_sell = false;
    for (const auto& e : events) {
        if (e.event_id == "agg_1") { buy_toxic = e.is_anomaly;  found_buy = true; }
        if (e.event_id == "agg_2") { sell_toxic = e.is_anomaly; found_sell = true; }
    }
    if (!found_buy || !found_sell) {
        std::fprintf(stderr, "FAIL: the two trades under test were not labelled "
                             "(buy=%d sell=%d)\n", found_buy, found_sell);
        return false;
    }
    if (!buy_toxic) {
        std::fprintf(stderr, "FAIL: an aggressive buy followed by +10 bps within "
                             "the horizon must be toxic at a 5 bps threshold\n");
        return false;
    }
    if (sell_toxic) {
        std::fprintf(stderr, "FAIL: an aggressive sell followed by +10 bps saw the "
                             "price move AGAINST it; it must not be toxic\n");
        return false;
    }
    std::printf("      +10 bps move: aggressive buy toxic, aggressive sell not\n");
    return true;
}

/// @brief Trades without a full horizon are dropped, never labelled negative.
bool test_tail_is_dropped() {
    const std::string path = write_fixture("tail",
        row(1, 100.0, 1.0, 0,    false) +
        row(2, 100.0, 1.0, 500,  false) +
        row(3, 100.0, 1.0, 1500, false) +
        row(4, 100.0, 1.0, 1900, false));   // within horizon of the last trade

    ToxicFlowLabelConfig cfg;
    cfg.horizon_ms = 1000;
    BinanceToxicFlowDataset ds(cfg);
    if (!ds.load(path)) return false;
    const auto events = ds.build_events();

    // Last time is 1900; cutoff is 900. Only trades at 0 and 500 qualify.
    if (events.size() != 2) {
        std::fprintf(stderr,
                     "FAIL: expected 2 labelled events (cutoff 900 ms), got %zu. "
                     "Trades inside the final horizon must be dropped, not "
                     "labelled normal.\n", events.size());
        return false;
    }
    if (ds.stats().total_trades != 4 || ds.stats().labelled != 2) {
        std::fprintf(stderr, "FAIL: stats say %zu total / %zu labelled\n",
                     ds.stats().total_trades, ds.stats().labelled);
        return false;
    }
    std::printf("      4 trades, horizon 1000 ms -> 2 labelled, 2 dropped\n");
    return true;
}

/**
 * @brief A trending market must not make one side systematically toxic.
 *
 * Builds a pure uptrend with alternating aggressor sides and no trade-specific
 * information. Without drift correction every buy is toxic and no sell is,
 * which is leakage: the aggressor side alone predicts the label. With the
 * correction the two sides must come out balanced.
 */
bool test_drift_adjustment_balances_sides() {
    std::string body;
    double price = 100.0;
    long long t = 0;
    for (int i = 0; i < 400; ++i) {
        body += row(static_cast<uint64_t>(i + 1), price, 1.0, t, (i % 2) == 0);
        price *= 1.0002;   // steady +2 bps per trade: pure drift
        t += 100;
    }

    auto toxic_by_side = [&](bool adjust) {
        ToxicFlowLabelConfig cfg;
        cfg.horizon_ms = 1000;
        cfg.threshold_bps = 5.0;
        cfg.drift_adjust = adjust;
        BinanceToxicFlowDataset ds(cfg);
        const std::string path =
            write_fixture(adjust ? "drift_on" : "drift_off", body);
        ds.load(path);
        const auto events = ds.build_events();
        int buy_toxic = 0, sell_toxic = 0;
        for (const auto& e : events) {
            if (!e.is_anomaly) continue;
            if (e.event_type == "aggressive_buy") ++buy_toxic; else ++sell_toxic;
        }
        return std::pair<int, int>{buy_toxic, sell_toxic};
    };

    const auto [b_off, s_off] = toxic_by_side(false);
    const auto [b_on, s_on] = toxic_by_side(true);

    std::printf("      pure +2 bps/trade uptrend, alternating sides:\n");
    std::printf("        drift_adjust=off -> %d toxic buys, %d toxic sells\n",
                b_off, s_off);
    std::printf("        drift_adjust=on  -> %d toxic buys, %d toxic sells\n",
                b_on, s_on);

    if (b_off == s_off) {
        std::fprintf(stderr,
                     "FAIL: without correction a pure uptrend should make buys "
                     "toxic and sells not; the fixture is not exercising drift.\n");
        return false;
    }
    if (b_on != s_on) {
        std::fprintf(stderr,
                     "FAIL: with drift correction a pure trend carries no "
                     "trade-specific information, so neither side should be "
                     "systematically toxic; got %d vs %d.\n", b_on, s_on);
        return false;
    }
    return true;
}

/// @brief Out-of-order input must be rejected, not silently mislabelled.
bool test_rejects_unordered_input() {
    const std::string path = write_fixture("unordered",
        row(1, 100.0, 1.0, 1000, false) +
        row(2, 100.0, 1.0,  500, false));   // goes backwards

    BinanceToxicFlowDataset ds;
    if (ds.load(path)) {
        std::fprintf(stderr,
                     "FAIL: accepted a file whose timestamps go backwards. "
                     "Forward labels would be computed against the wrong "
                     "future.\n");
        return false;
    }
    std::printf("      rejected: %s\n", ds.error().c_str());
    return true;
}

}  // namespace

bool run_binance_dataset_tests() {
    struct Case { const char* name; bool (*fn)(); };
    const Case cases[] = {
        {"aggTrades CSV parsing",                    test_parsing},
        {"aggressor side convention",                test_aggressor_convention},
        {"forward label follows the aggressor",      test_forward_label_direction},
        {"trades without a full horizon are dropped", test_tail_is_dropped},
        {"drift correction balances the two sides",  test_drift_adjustment_balances_sides},
        {"unordered input is rejected",              test_rejects_unordered_input},
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
