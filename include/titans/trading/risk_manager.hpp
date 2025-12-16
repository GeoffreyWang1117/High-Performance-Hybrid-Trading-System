/**
 * @file risk_manager.hpp
 * @brief Real-time Risk Management System
 *
 * Implements pre-trade and post-trade risk checks with
 * sub-millisecond latency. Critical for preventing
 * catastrophic losses in automated trading.
 */

#pragma once

#include "titans/core/types.hpp"
#include "titans/core/event_bus.hpp"
#include "order_book.hpp"

#include <unordered_map>
#include <atomic>
#include <mutex>

namespace titans {

/**
 * @brief Risk limits configuration
 */
struct RiskLimits {
    // Position limits
    Quantity max_position_size = to_quantity(1000.0);     // Per symbol
    Quantity max_total_position = to_quantity(10000.0);   // Total across symbols
    double max_position_value_usd = 100000.0;              // USD value

    // Order limits
    Quantity max_order_size = to_quantity(100.0);
    int max_orders_per_second = 10;
    int max_orders_per_minute = 100;

    // Loss limits
    double max_daily_loss_usd = 1000.0;
    double max_drawdown_pct = 5.0;   // From peak equity

    // Price limits
    double max_price_deviation_pct = 1.0;  // From reference price

    // Exposure limits
    double max_gross_exposure_usd = 200000.0;
    double max_net_exposure_usd = 50000.0;
};

/**
 * @brief Position tracking for risk management
 */
struct Position {
    Symbol      symbol;
    Quantity    quantity;       // Signed: positive=long, negative=short
    Price       avg_price;
    Price       current_price;
    double      unrealized_pnl;
    double      realized_pnl;
    Timestamp   last_update;

    Position() : quantity(0), avg_price(0), current_price(0),
                 unrealized_pnl(0), realized_pnl(0), last_update(0) {}

    double notional_value() const {
        return std::abs(from_quantity(quantity)) * from_price(current_price);
    }

    bool is_long() const { return quantity > 0; }
    bool is_short() const { return quantity < 0; }
    bool is_flat() const { return quantity == 0; }
};

/**
 * @brief Risk check result
 */
struct RiskCheckResult {
    bool passed = true;
    std::string reject_reason;
    double risk_score = 0.0;  // 0-100, higher = riskier

    static RiskCheckResult pass() { return {true, "", 0.0}; }
    static RiskCheckResult fail(const std::string& reason, double score = 100.0) {
        return {false, reason, score};
    }
};

/**
 * @brief Real-time Risk Manager
 *
 * Performs pre-trade validation and monitors positions
 * for risk limit breaches.
 */
class RiskManager : public EventPublisher {
public:
    explicit RiskManager(EventBus& bus, const RiskLimits& limits = {})
        : EventPublisher(bus), limits_(limits),
          daily_pnl_(0), peak_equity_(0), order_count_second_(0),
          order_count_minute_(0), last_second_(0), last_minute_(0),
          is_trading_enabled_(true) {}

    /**
     * @brief Pre-trade risk check
     */
    RiskCheckResult check_order(const Order& order) {
        if (!is_trading_enabled_) {
            return RiskCheckResult::fail("Trading disabled");
        }

        // Rate limiting
        auto rate_check = check_rate_limits();
        if (!rate_check.passed) return rate_check;

        // Order size check
        if (order.quantity > limits_.max_order_size) {
            return RiskCheckResult::fail("Order size exceeds limit");
        }

        // Position size check
        auto& pos = positions_[order.symbol];
        Quantity new_position = pos.quantity +
            (order.side == Side::Buy ? order.quantity : -order.quantity);

        if (std::abs(new_position) > limits_.max_position_size) {
            return RiskCheckResult::fail("Would exceed position limit");
        }

        // Price deviation check (if limit order)
        if (order.type == OrderType::Limit) {
            auto price_check = check_price_deviation(order);
            if (!price_check.passed) return price_check;
        }

        // Daily loss check
        if (daily_pnl_ < -limits_.max_daily_loss_usd) {
            return RiskCheckResult::fail("Daily loss limit reached");
        }

        // Exposure check
        auto exposure_check = check_exposure(order);
        if (!exposure_check.passed) return exposure_check;

        // Record order for rate limiting
        record_order();

        return RiskCheckResult::pass();
    }

    /**
     * @brief Update position on fill
     */
    void on_fill(const Symbol& symbol, Side side, Quantity qty, Price price) {
        auto& pos = positions_[symbol];
        pos.symbol = symbol;

        Quantity signed_qty = (side == Side::Buy) ? qty : -qty;
        Quantity old_qty = pos.quantity;
        Quantity new_qty = old_qty + signed_qty;

        // Calculate realized PnL if reducing position
        if ((old_qty > 0 && new_qty < old_qty) || (old_qty < 0 && new_qty > old_qty)) {
            Quantity closed_qty = std::min(std::abs(signed_qty), std::abs(old_qty));
            double pnl_per_unit = from_price(price) - from_price(pos.avg_price);
            if (old_qty < 0) pnl_per_unit = -pnl_per_unit;  // Short position

            pos.realized_pnl += from_quantity(closed_qty) * pnl_per_unit;
            daily_pnl_ += from_quantity(closed_qty) * pnl_per_unit;
        }

        // Update average price if adding to position
        if ((old_qty >= 0 && signed_qty > 0) || (old_qty <= 0 && signed_qty < 0)) {
            if (old_qty == 0) {
                pos.avg_price = price;
            } else {
                // Weighted average
                Price total_cost = pos.avg_price * std::abs(old_qty) +
                                   price * std::abs(signed_qty);
                pos.avg_price = total_cost / std::abs(new_qty);
            }
        }

        pos.quantity = new_qty;
        pos.last_update = now_ns();

        update_peak_equity();
        check_risk_limits();
    }

    /**
     * @brief Update mark-to-market prices
     */
    void on_price_update(const Symbol& symbol, Price bid, Price ask) {
        auto it = positions_.find(symbol);
        if (it == positions_.end()) return;

        auto& pos = it->second;
        Price mark = (bid + ask) / 2;
        pos.current_price = mark;

        // Update unrealized PnL
        if (pos.quantity != 0) {
            double pnl_per_unit = from_price(mark) - from_price(pos.avg_price);
            if (pos.quantity < 0) pnl_per_unit = -pnl_per_unit;
            pos.unrealized_pnl = from_quantity(std::abs(pos.quantity)) * pnl_per_unit;
        }

        reference_prices_[symbol] = {bid, ask};
    }

    /**
     * @brief Get current position
     */
    const Position* get_position(const Symbol& symbol) const {
        auto it = positions_.find(symbol);
        return it != positions_.end() ? &it->second : nullptr;
    }

    /**
     * @brief Get all positions
     */
    const std::unordered_map<Symbol, Position, SymbolHash>& positions() const {
        return positions_;
    }

    /**
     * @brief Calculate total unrealized PnL
     */
    double total_unrealized_pnl() const {
        double total = 0;
        for (const auto& [_, pos] : positions_) {
            total += pos.unrealized_pnl;
        }
        return total;
    }

    /**
     * @brief Calculate total realized PnL
     */
    double total_realized_pnl() const {
        double total = 0;
        for (const auto& [_, pos] : positions_) {
            total += pos.realized_pnl;
        }
        return total;
    }

    /**
     * @brief Calculate gross exposure (sum of absolute values)
     */
    double gross_exposure() const {
        double total = 0;
        for (const auto& [_, pos] : positions_) {
            total += pos.notional_value();
        }
        return total;
    }

    /**
     * @brief Calculate net exposure (long - short)
     */
    double net_exposure() const {
        double total = 0;
        for (const auto& [_, pos] : positions_) {
            total += from_quantity(pos.quantity) * from_price(pos.current_price);
        }
        return total;
    }

    /**
     * @brief Emergency stop - disable all trading
     */
    void emergency_stop(const std::string& reason) {
        is_trading_enabled_ = false;
        // TODO: Cancel all open orders
        // TODO: Publish alert event
    }

    /**
     * @brief Re-enable trading
     */
    void enable_trading() {
        is_trading_enabled_ = true;
    }

    /**
     * @brief Reset daily counters (call at start of trading day)
     */
    void reset_daily() {
        daily_pnl_ = 0;
        order_count_second_ = 0;
        order_count_minute_ = 0;
    }

    bool is_trading_enabled() const { return is_trading_enabled_; }
    double daily_pnl() const { return daily_pnl_; }
    const RiskLimits& limits() const { return limits_; }
    void set_limits(const RiskLimits& limits) { limits_ = limits; }

private:
    RiskCheckResult check_rate_limits() {
        Timestamp now = now_us();
        Timestamp second = now / 1000000;
        Timestamp minute = now / 60000000;

        if (second != last_second_) {
            last_second_ = second;
            order_count_second_ = 0;
        }
        if (minute != last_minute_) {
            last_minute_ = minute;
            order_count_minute_ = 0;
        }

        if (order_count_second_ >= limits_.max_orders_per_second) {
            return RiskCheckResult::fail("Order rate limit (per second)");
        }
        if (order_count_minute_ >= limits_.max_orders_per_minute) {
            return RiskCheckResult::fail("Order rate limit (per minute)");
        }

        return RiskCheckResult::pass();
    }

    RiskCheckResult check_price_deviation(const Order& order) {
        auto it = reference_prices_.find(order.symbol);
        if (it == reference_prices_.end()) {
            return RiskCheckResult::pass();  // No reference price
        }

        auto [bid, ask] = it->second;
        Price mid = (bid + ask) / 2;
        if (mid == 0) return RiskCheckResult::pass();

        double deviation = std::abs(from_price(order.price) - from_price(mid))
                          / from_price(mid) * 100;

        if (deviation > limits_.max_price_deviation_pct) {
            return RiskCheckResult::fail("Price deviation too high: " +
                std::to_string(deviation) + "%");
        }

        return RiskCheckResult::pass();
    }

    RiskCheckResult check_exposure(const Order& order) {
        // Simulate new exposure
        double current_gross = gross_exposure();
        double order_value = from_quantity(order.quantity) *
                             from_price(order.price);

        if (current_gross + order_value > limits_.max_gross_exposure_usd) {
            return RiskCheckResult::fail("Would exceed gross exposure limit");
        }

        return RiskCheckResult::pass();
    }

    void record_order() {
        ++order_count_second_;
        ++order_count_minute_;
    }

    void update_peak_equity() {
        double equity = total_realized_pnl() + total_unrealized_pnl();
        if (equity > peak_equity_) {
            peak_equity_ = equity;
        }
    }

    void check_risk_limits() {
        double equity = total_realized_pnl() + total_unrealized_pnl();
        double drawdown = (peak_equity_ > 0) ?
            (peak_equity_ - equity) / peak_equity_ * 100 : 0;

        if (drawdown > limits_.max_drawdown_pct) {
            emergency_stop("Drawdown limit breached: " + std::to_string(drawdown) + "%");
        }

        if (daily_pnl_ < -limits_.max_daily_loss_usd) {
            emergency_stop("Daily loss limit breached");
        }
    }

    RiskLimits limits_;
    std::unordered_map<Symbol, Position, SymbolHash> positions_;
    std::unordered_map<Symbol, std::pair<Price, Price>, SymbolHash> reference_prices_;

    double daily_pnl_;
    double peak_equity_;
    int order_count_second_;
    int order_count_minute_;
    Timestamp last_second_;
    Timestamp last_minute_;
    std::atomic<bool> is_trading_enabled_;
};

/**
 * @brief Position Manager for tracking and reporting
 */
class PositionManager : public EventPublisher {
public:
    explicit PositionManager(EventBus& bus, RiskManager& risk)
        : EventPublisher(bus), risk_(risk) {}

    /**
     * @brief Get position summary
     */
    struct PositionSummary {
        int num_positions;
        int num_long;
        int num_short;
        double total_long_value;
        double total_short_value;
        double net_value;
        double unrealized_pnl;
        double realized_pnl;
    };

    PositionSummary get_summary() const {
        PositionSummary summary{};

        for (const auto& [_, pos] : risk_.positions()) {
            if (pos.quantity == 0) continue;

            ++summary.num_positions;
            double value = pos.notional_value();

            if (pos.is_long()) {
                ++summary.num_long;
                summary.total_long_value += value;
                summary.net_value += value;
            } else {
                ++summary.num_short;
                summary.total_short_value += value;
                summary.net_value -= value;
            }

            summary.unrealized_pnl += pos.unrealized_pnl;
            summary.realized_pnl += pos.realized_pnl;
        }

        return summary;
    }

    /**
     * @brief Publish position update event
     */
    void publish_position_update(const Symbol& symbol) {
        const Position* pos = risk_.get_position(symbol);
        if (!pos) return;

        // TODO: Publish PositionUpdateEvent
    }

private:
    RiskManager& risk_;
};

}  // namespace titans
