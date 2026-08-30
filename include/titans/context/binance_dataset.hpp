/**
 * @file binance_dataset.hpp
 * @brief Real Binance aggTrades with a forward-looking, non-leakable label.
 *
 * THE LABEL PROBLEM ON REAL DATA
 * ------------------------------
 * Real market data comes with no "anomaly" column, so a label has to be
 * defined. The previous adapter defined it as
 *
 *     is_anomaly = quantity > 10.0 || |dP| / P > 0.001
 *
 * which is a deterministic function of two fields carried by the event itself.
 * Any model shown the event can recover the label by arithmetic; no context is
 * required, and no context strategy can outperform another. It is the same
 * leakage the synthetic generator had, wearing a market costume.
 *
 * This file labels trades by what happens AFTERWARDS, which no field of the
 * event can encode:
 *
 *     A trade is TOXIC if, within `horizon_ms` after it, the mid price moves
 *     at least `threshold_bps` in the aggressor's favour.
 *
 * That is the standard adverse-selection notion a market maker cares about:
 * the flow you just filled was informed, and the price ran away from you. It
 * is economically meaningful, it is derived from the data rather than asserted,
 * and it is genuinely hard -- predicting it requires reading recent order flow,
 * which is exactly the context whose management this project studies.
 *
 * Binance aggTrades convention: `is_buyer_maker == true` means the buyer was
 * the passive side, so the SELLER was the aggressor. Getting this backwards
 * inverts every label, so it is asserted in tests rather than trusted to a
 * comment.
 */

#pragma once

#include "experiment_harness.hpp"
#include "../core/types.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace titans {
namespace context {

/// @brief One aggregated trade as published by Binance.
struct AggTrade {
    uint64_t agg_trade_id = 0;
    double price = 0.0;
    double quantity = 0.0;
    Timestamp transact_time_ms = 0;
    /// True => buyer was the maker => the SELLER lifted the bid (aggressive sell).
    bool is_buyer_maker = false;

    /// @return +1 for an aggressive buy, -1 for an aggressive sell.
    int aggressor_sign() const { return is_buyer_maker ? -1 : +1; }
};

/// @brief How a trade stream is turned into labelled events.
struct ToxicFlowLabelConfig {
    /// How far ahead to look for the adverse move.
    int64_t horizon_ms = 1000;
    /// Move, in basis points, that counts as adverse selection.
    double threshold_bps = 5.0;
    /// Bucket trades into entities by this many milliseconds, so the "entity"
    /// is a time slice of the book rather than a single symbol. Set to 0 to
    /// treat the whole file as one entity.
    int64_t entity_bucket_ms = 0;

    /**
     * @brief Remove unconditional price drift before thresholding.
     *
     * Without this the label inherits the session's direction. On BTCUSDT
     * 2024-01-15 the price rose from 41718 to 42769, so an aggressive BUY was
     * followed by a favourable move far more often than an aggressive SELL --
     * purely because the market went up, not because the flow was informed.
     * The aggressor side alone then reaches AUC 0.604 against the label, which
     * is leakage: a model can guess above chance from one field of the event
     * and never consult context.
     *
     * Subtracting the sample-mean forward return centres both sides on the
     * same expectation, leaving only the trade-specific component that
     * informed flow is supposed to explain. This is a label-construction step,
     * so using whole-sample information is legitimate; the constraint is that
     * the label must not be recoverable from the event, not that it must avoid
     * the future.
     */
    bool drift_adjust = true;
};

struct DatasetStats {
    size_t total_trades = 0;
    size_t labelled = 0;         ///< Trades with a full horizon ahead of them.
    size_t toxic = 0;
    double toxic_rate = 0.0;
    Timestamp first_time_ms = 0;
    Timestamp last_time_ms = 0;
    double min_price = 0.0;
    double max_price = 0.0;
    /// Mean forward return over the session, in bps. Subtracted from every
    /// move when drift_adjust is on; reported so the size of the correction
    /// is visible rather than hidden.
    double drift_bps = 0.0;

    void print() const {
        std::printf("  trades:        %zu\n", total_trades);
        std::printf("  labelled:      %zu (%.1f%% -- the tail lacks a full "
                    "horizon and is dropped)\n",
                    labelled, total_trades ? 100.0 * labelled / total_trades : 0.0);
        std::printf("  toxic:         %zu (%.2f%% of labelled)\n",
                    toxic, toxic_rate * 100.0);
        std::printf("  span:          %ld ms\n",
                    static_cast<long>(last_time_ms - first_time_ms));
        std::printf("  price range:   %.2f - %.2f\n", min_price, max_price);
        std::printf("  session drift: %+.3f bps per horizon (removed from the "
                    "label)\n", drift_bps);
    }
};

/**
 * @brief Loads aggTrades and labels them by forward adverse selection.
 *
 * Two-pass by necessity: the label depends on the future, so the whole file is
 * read before any event can be emitted. A day of BTCUSDT is ~1.4M trades, about
 * 60 MB in memory here, which is why this loads rather than streams.
 */
class BinanceToxicFlowDataset {
public:
    explicit BinanceToxicFlowDataset(ToxicFlowLabelConfig config = {})
        : config_(config) {}

    /**
     * @brief Read a Binance aggTrades CSV. Returns false on I/O or parse error.
     * @param path      CSV as extracted by python/data/fetch_binance.py.
     * @param max_rows  0 for all rows; otherwise stop early (for smoke tests).
     */
    bool load(const std::string& path, size_t max_rows = 0) {
        std::ifstream f(path);
        if (!f) {
            error_ = "cannot open " + path;
            return false;
        }

        trades_.clear();
        std::string line;
        size_t line_no = 0;
        while (std::getline(f, line)) {
            ++line_no;
            if (line.empty()) continue;
            // Binance ships these files headerless, but tolerate one if a
            // downstream tool added it.
            if (line_no == 1 && line.find("agg_trade_id") != std::string::npos) continue;

            AggTrade t;
            if (!parse_line(line, t)) {
                error_ = "parse error at line " + std::to_string(line_no) +
                         ": " + line.substr(0, 80);
                return false;
            }
            trades_.push_back(t);
            if (max_rows && trades_.size() >= max_rows) break;
        }

        if (trades_.empty()) {
            error_ = "no trades parsed from " + path;
            return false;
        }
        // Binance publishes in time order; assert rather than assume, because
        // an out-of-order file would silently corrupt every forward label.
        for (size_t i = 1; i < trades_.size(); ++i) {
            if (trades_[i].transact_time_ms < trades_[i - 1].transact_time_ms) {
                error_ = "trades are not in time order at index " +
                         std::to_string(i) +
                         "; forward labels would be meaningless";
                return false;
            }
        }
        source_path_ = path;
        return true;
    }

    /**
     * @brief Label every trade that has a full horizon ahead of it.
     *
     * A trade is toxic when the price `horizon_ms` later has moved at least
     * `threshold_bps` in the aggressor's favour. Trades in the final
     * `horizon_ms` of the file have no future to look at and are DROPPED, not
     * labelled false: labelling them normal would inject a block of guaranteed
     * negatives at the end of every run.
     */
    std::vector<SyntheticEvent> build_events() {
        std::vector<SyntheticEvent> events;
        events.reserve(trades_.size());

        stats_ = DatasetStats{};
        stats_.total_trades = trades_.size();
        stats_.first_time_ms = trades_.front().transact_time_ms;
        stats_.last_time_ms = trades_.back().transact_time_ms;
        stats_.min_price = trades_.front().price;
        stats_.max_price = trades_.front().price;

        const Timestamp cutoff = stats_.last_time_ms - config_.horizon_ms;

        // Pass 1: forward return for every trade that has a full horizon.
        const auto forward = compute_forward_returns(cutoff);

        // Drift: the mean forward return over the session. Subtracting it
        // centres aggressive buys and sells on the same expectation, so the
        // label reflects trade-specific information rather than which way the
        // market happened to go.
        double drift_bps = 0.0;
        if (config_.drift_adjust) {
            double sum = 0.0;
            size_t n = 0;
            for (const auto& fr : forward) {
                if (fr.valid) { sum += fr.return_bps; ++n; }
            }
            drift_bps = n ? sum / static_cast<double>(n) : 0.0;
            stats_.drift_bps = drift_bps;
        }

        // Pass 2: threshold the drift-adjusted, aggressor-signed move.
        for (size_t i = 0; i < trades_.size(); ++i) {
            const auto& t = trades_[i];
            stats_.min_price = std::min(stats_.min_price, t.price);
            stats_.max_price = std::max(stats_.max_price, t.price);

            if (!forward[i].valid) continue;   // no full horizon

            const double signed_move_bps =
                t.aggressor_sign() * (forward[i].return_bps - drift_bps);
            const bool toxic = signed_move_bps >= config_.threshold_bps;

            SyntheticEvent e;
            e.event_id = "agg_" + std::to_string(t.agg_trade_id);
            e.entity_id = entity_of(t);
            e.timestamp = t.transact_time_ms * 1000000LL;   // ms -> ns
            e.value = t.price;
            // Category carries the aggressor side, which is a real market fact
            // available at decision time. It is NOT the label: whether the
            // price subsequently runs is exactly what has to be predicted.
            e.event_type = t.is_buyer_maker ? "aggressive_sell" : "aggressive_buy";
            e.is_anomaly = toxic;

            events.push_back(std::move(e));
            ++stats_.labelled;
            if (toxic) ++stats_.toxic;
        }

        stats_.toxic_rate = stats_.labelled
            ? static_cast<double>(stats_.toxic) / stats_.labelled : 0.0;
        return events;
    }

    const DatasetStats& stats() const { return stats_; }
    const std::string& error() const { return error_; }
    const std::string& source_path() const { return source_path_; }
    const std::vector<AggTrade>& trades() const { return trades_; }
    const ToxicFlowLabelConfig& config() const { return config_; }

private:
    struct ForwardReturn {
        double return_bps = 0.0;
        bool valid = false;
    };

    /**
     * @brief Unsigned forward return for each trade, in bps.
     *
     * Uses a monotone pointer into the future, so the whole pass is O(n) rather
     * than O(n log n) per trade. Trades within `horizon_ms` of the end of the
     * file have no future to measure and are marked invalid; they are dropped
     * rather than labelled negative, which would append a block of guaranteed
     * negatives to every run.
     */
    std::vector<ForwardReturn> compute_forward_returns(Timestamp cutoff) const {
        std::vector<ForwardReturn> out(trades_.size());
        size_t ahead = 0;
        for (size_t i = 0; i < trades_.size(); ++i) {
            const auto& t = trades_[i];
            if (t.transact_time_ms > cutoff) continue;

            const Timestamp target = t.transact_time_ms + config_.horizon_ms;
            if (ahead < i) ahead = i;
            while (ahead + 1 < trades_.size() &&
                   trades_[ahead + 1].transact_time_ms <= target) {
                ++ahead;
            }
            out[i].return_bps =
                (trades_[ahead].price - t.price) / t.price * 10000.0;
            out[i].valid = true;
        }
        return out;
    }

    std::string entity_of(const AggTrade& t) const {
        if (config_.entity_bucket_ms <= 0) return "BTCUSDT";
        const long long bucket = t.transact_time_ms / config_.entity_bucket_ms;
        return "slice_" + std::to_string(bucket);
    }

    static bool parse_line(const std::string& line, AggTrade& out) {
        std::istringstream ss(line);
        std::string tok;
        try {
            if (!std::getline(ss, tok, ',')) return false;
            out.agg_trade_id = std::stoull(tok);
            if (!std::getline(ss, tok, ',')) return false;
            out.price = std::stod(tok);
            if (!std::getline(ss, tok, ',')) return false;
            out.quantity = std::stod(tok);
            if (!std::getline(ss, tok, ',')) return false;   // first_trade_id
            if (!std::getline(ss, tok, ',')) return false;   // last_trade_id
            if (!std::getline(ss, tok, ',')) return false;
            out.transact_time_ms = static_cast<Timestamp>(std::stoull(tok));
            if (!std::getline(ss, tok, ',')) return false;
            out.is_buyer_maker = (tok == "True" || tok == "true" || tok == "1");
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

    ToxicFlowLabelConfig config_;
    std::vector<AggTrade> trades_;
    DatasetStats stats_;
    std::string error_;
    std::string source_path_;
};

}  // namespace context
}  // namespace titans
