/**
 * @file matching_engine.hpp
 * @brief Simulated Matching Engine for Shadow Trading
 *
 * Implements a realistic order matching simulation that mirrors
 * exchange behavior. Used for backtesting and shadow trading
 * to validate strategies before live deployment.
 */

#pragma once

#include "titans/core/types.hpp"
#include "titans/core/event_bus.hpp"
#include "order_book.hpp"

#include <queue>
#include <functional>

namespace titans {

/**
 * @brief Match result for a single fill
 */
struct MatchResult {
    OrderId     maker_order_id;
    OrderId     taker_order_id;
    Price       price;
    Quantity    quantity;
    Side        taker_side;
    Timestamp   timestamp;
};

/**
 * @brief Order execution report
 */
struct ExecutionReport {
    OrderId     order_id;
    OrderStatus status;
    Price       avg_price;
    Quantity    filled_qty;
    Quantity    remaining_qty;
    Timestamp   timestamp;
    std::string reject_reason;

    std::vector<MatchResult> fills;
};

/**
 * @brief Matching Engine Configuration
 */
struct MatchingConfig {
    bool enable_partial_fills = true;
    bool price_time_priority = true;  // FIFO at same price
    Duration latency_simulation_ns = 0;  // Simulated exchange latency
    double maker_fee = 0.0001;  // 0.01%
    double taker_fee = 0.0002;  // 0.02%
};

/**
 * @brief Simulated Matching Engine
 *
 * Processes orders against an order book, generating fills
 * and execution reports. Supports both market and limit orders.
 */
class MatchingEngine : public EventPublisher {
public:
    using ExecutionCallback = std::function<void(const ExecutionReport&)>;
    using TradeCallback = std::function<void(const MatchResult&)>;

    MatchingEngine(EventBus& bus, L3OrderBook& book,
                   const MatchingConfig& config = {})
        : EventPublisher(bus), book_(book), config_(config),
          next_trade_id_(1) {}

    /**
     * @brief Submit a new order
     * @return Execution report
     */
    ExecutionReport submit_order(const Order& order) {
        ExecutionReport report;
        report.order_id = order.id;
        report.timestamp = now_ns();
        report.remaining_qty = order.quantity;
        report.filled_qty = 0;
        report.avg_price = 0;

        // Validate order
        if (order.quantity <= 0) {
            report.status = OrderStatus::Rejected;
            report.reject_reason = "Invalid quantity";
            publish_execution(report);
            return report;
        }

        if (order.type == OrderType::Limit && order.price <= 0) {
            report.status = OrderStatus::Rejected;
            report.reject_reason = "Invalid price for limit order";
            publish_execution(report);
            return report;
        }

        // Process based on order type
        switch (order.type) {
            case OrderType::Market:
                process_market_order(order, report);
                break;
            case OrderType::Limit:
                process_limit_order(order, report);
                break;
            case OrderType::IOC:
                process_ioc_order(order, report);
                break;
            case OrderType::FOK:
                process_fok_order(order, report);
                break;
            default:
                report.status = OrderStatus::Rejected;
                report.reject_reason = "Unsupported order type";
                break;
        }

        publish_execution(report);
        return report;
    }

    /**
     * @brief Cancel an existing order
     */
    ExecutionReport cancel_order(OrderId order_id) {
        ExecutionReport report;
        report.order_id = order_id;
        report.timestamp = now_ns();

        if (book_.cancel_order(order_id)) {
            report.status = OrderStatus::Cancelled;
        } else {
            report.status = OrderStatus::Rejected;
            report.reject_reason = "Order not found";
        }

        publish_execution(report);
        return report;
    }

    /**
     * @brief Process incoming market data (for market-making simulation)
     */
    void on_market_data(const MarketDataEvent& md) {
        // Update reference prices for validation
        if (md.bid_levels > 0) {
            reference_bid_ = md.bid_prices[0];
        }
        if (md.ask_levels > 0) {
            reference_ask_ = md.ask_prices[0];
        }
    }

    /**
     * @brief Set callback for execution reports
     */
    void set_execution_callback(ExecutionCallback cb) {
        execution_callback_ = std::move(cb);
    }

    /**
     * @brief Set callback for trades
     */
    void set_trade_callback(TradeCallback cb) {
        trade_callback_ = std::move(cb);
    }

    const MatchingConfig& config() const { return config_; }
    void set_config(const MatchingConfig& config) { config_ = config; }

private:
    void process_market_order(const Order& order, ExecutionReport& report) {
        auto fills = book_.execute_market_order(order.side, order.quantity);

        process_fills(order, fills, report);

        if (report.remaining_qty > 0) {
            // Partial fill - rest cancelled for market orders
            report.status = report.filled_qty > 0 ?
                OrderStatus::PartiallyFilled : OrderStatus::Rejected;
            if (report.filled_qty == 0) {
                report.reject_reason = "No liquidity";
            }
        } else {
            report.status = OrderStatus::Filled;
        }
    }

    void process_limit_order(const Order& order, ExecutionReport& report) {
        // Try to match against opposite side
        Quantity remaining = order.quantity;

        if (order.side == Side::Buy) {
            // Buy order matches asks at or below limit price
            while (remaining > 0) {
                auto best = book_.best_ask();
                if (!best || best->price > order.price) break;

                Quantity to_fill = std::min(remaining, best->quantity);
                auto fills = book_.execute_market_order(Side::Buy, to_fill);
                process_fills(order, fills, report);
                remaining = report.remaining_qty;
            }
        } else {
            // Sell order matches bids at or above limit price
            while (remaining > 0) {
                auto best = book_.best_bid();
                if (!best || best->price < order.price) break;

                Quantity to_fill = std::min(remaining, best->quantity);
                auto fills = book_.execute_market_order(Side::Sell, to_fill);
                process_fills(order, fills, report);
                remaining = report.remaining_qty;
            }
        }

        // Rest of order goes to book
        if (remaining > 0) {
            OrderId book_id = book_.add_order(order.side, order.price, remaining);
            resting_orders_[order.id] = book_id;

            report.status = report.filled_qty > 0 ?
                OrderStatus::PartiallyFilled : OrderStatus::New;
        } else {
            report.status = OrderStatus::Filled;
        }
    }

    void process_ioc_order(const Order& order, ExecutionReport& report) {
        // Immediate-or-Cancel: fill what's available, cancel rest
        auto fills = book_.execute_market_order(order.side,
            std::min(order.quantity, available_liquidity(order.side, order.price)));

        process_fills(order, fills, report);

        report.status = report.filled_qty > 0 ?
            (report.remaining_qty > 0 ? OrderStatus::PartiallyFilled : OrderStatus::Filled)
            : OrderStatus::Cancelled;
    }

    void process_fok_order(const Order& order, ExecutionReport& report) {
        // Fill-or-Kill: must fill entire quantity or reject
        Quantity available = available_liquidity(order.side, order.price);

        if (available >= order.quantity) {
            auto fills = book_.execute_market_order(order.side, order.quantity);
            process_fills(order, fills, report);
            report.status = OrderStatus::Filled;
        } else {
            report.status = OrderStatus::Rejected;
            report.reject_reason = "Insufficient liquidity for FOK";
        }
    }

    void process_fills(const Order& order,
                       const std::vector<std::pair<OrderId, Quantity>>& fills,
                       ExecutionReport& report) {
        Price total_value = 0;
        Quantity total_qty = 0;

        for (const auto& [maker_id, fill_qty] : fills) {
            MatchResult match;
            match.maker_order_id = maker_id;
            match.taker_order_id = order.id;
            match.quantity = fill_qty;
            match.taker_side = order.side;
            match.timestamp = now_ns();

            // Get fill price (assume at maker's price)
            // In real impl, look up maker order price
            match.price = order.price;  // Simplified

            report.fills.push_back(match);
            total_value += match.price * fill_qty;
            total_qty += fill_qty;

            // Publish trade event
            if (trade_callback_) {
                trade_callback_(match);
            }

            // Publish to event bus
            TradeEvent trade_event;
            trade_event.symbol = order.symbol;
            trade_event.trade_id = next_trade_id_++;
            trade_event.price = match.price;
            trade_event.quantity = match.quantity;
            trade_event.aggressor_side = order.side;
            publish(trade_event);
        }

        report.filled_qty += total_qty;
        report.remaining_qty = order.quantity - report.filled_qty;

        if (total_qty > 0) {
            report.avg_price = total_value / total_qty;
        }
    }

    void publish_execution(const ExecutionReport& report) {
        if (execution_callback_) {
            execution_callback_(report);
        }

        // Publish order event
        OrderEvent event;
        event.order.id = report.order_id;
        event.order.status = report.status;
        event.fill_price = report.avg_price;
        event.fill_qty = report.filled_qty;

        switch (report.status) {
            case OrderStatus::New:
                event.type = EventType::OrderAccepted;
                break;
            case OrderStatus::Filled:
            case OrderStatus::PartiallyFilled:
                event.type = EventType::OrderFilled;
                break;
            case OrderStatus::Cancelled:
                event.type = EventType::OrderCancelled;
                break;
            case OrderStatus::Rejected:
                event.type = EventType::OrderRejected;
                break;
            default:
                break;
        }

        publish(event);
    }

    Quantity available_liquidity(Side taker_side, Price limit_price) const {
        Quantity total = 0;

        if (taker_side == Side::Buy) {
            // Sum ask liquidity at or below limit
            // Simplified - should iterate book
            auto best = book_.best_ask();
            if (best && best->price <= limit_price) {
                total = best->quantity;
            }
        } else {
            // Sum bid liquidity at or above limit
            auto best = book_.best_bid();
            if (best && best->price >= limit_price) {
                total = best->quantity;
            }
        }

        return total;
    }

    L3OrderBook& book_;
    MatchingConfig config_;
    uint64_t next_trade_id_;
    Price reference_bid_ = 0;
    Price reference_ask_ = 0;

    std::unordered_map<OrderId, OrderId> resting_orders_;  // Our ID -> Book ID
    ExecutionCallback execution_callback_;
    TradeCallback trade_callback_;
};

}  // namespace titans
