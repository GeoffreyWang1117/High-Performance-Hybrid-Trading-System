/**
 * @file shadow_engine.hpp
 * @brief Shadow Trading Engine for Strategy Validation
 *
 * Implements a shadow trading system that:
 * - Receives real market data via WebSocket
 * - Simulates order execution without real trading
 * - Tracks hypothetical PnL and positions
 * - Enables A/B testing of strategies
 *
 * This is critical for validating strategies before live deployment.
 */

#pragma once

#include "titans/core/types.hpp"
#include "titans/core/event_bus.hpp"
#include "titans/core/event_loop.hpp"
#include "order_book.hpp"
#include "matching_engine.hpp"
#include "risk_manager.hpp"
#include "titans/strategy/strategy_base.hpp"
#include "titans/market_data/binary_logger.hpp"

#include <memory>
#include <vector>
#include <atomic>
#include <chrono>

namespace titans {

/**
 * @brief Shadow trading mode
 */
enum class ShadowMode {
    Live,       // Connected to live market data
    Replay,     // Replaying historical data
    Backtest    // Fast-forward backtest
};

/**
 * @brief Shadow trading configuration
 */
struct ShadowConfig {
    ShadowMode mode = ShadowMode::Live;
    std::vector<Symbol> symbols;

    // Simulation parameters
    Duration simulated_latency_ns = 10000000;  // 10ms
    double slippage_bps = 5.0;                 // 5 basis points
    double commission_bps = 10.0;              // 10 basis points

    // Logging
    bool log_trades = true;
    bool log_book_updates = true;
    std::string log_path = "./data/shadow";

    // Performance
    bool record_latency = true;
    Duration stats_interval_ns = 60000000000;  // 1 minute
};

/**
 * @brief Shadow trading statistics
 */
struct ShadowStats {
    // Timing
    Timestamp start_time = 0;
    Timestamp end_time = 0;
    Duration total_duration = 0;

    // Data
    uint64_t market_data_events = 0;
    uint64_t trade_events = 0;
    uint64_t book_updates = 0;

    // Execution
    uint64_t orders_submitted = 0;
    uint64_t orders_filled = 0;
    uint64_t orders_cancelled = 0;
    uint64_t orders_rejected = 0;

    // Latency (nanoseconds)
    Duration avg_event_latency = 0;
    Duration max_event_latency = 0;
    Duration p99_event_latency = 0;

    Duration avg_order_latency = 0;
    Duration max_order_latency = 0;

    // PnL
    double total_pnl = 0;
    double realized_pnl = 0;
    double unrealized_pnl = 0;
    double max_drawdown = 0;
    double sharpe_ratio = 0;

    void print() const {
        printf("\n=== Shadow Trading Statistics ===\n");
        printf("Duration: %.2f seconds\n", total_duration / 1e9);
        printf("\nData Events:\n");
        printf("  Market Data: %lu\n", market_data_events);
        printf("  Trades: %lu\n", trade_events);
        printf("  Book Updates: %lu\n", book_updates);
        printf("\nOrders:\n");
        printf("  Submitted: %lu\n", orders_submitted);
        printf("  Filled: %lu (%.1f%%)\n", orders_filled,
               orders_submitted > 0 ? 100.0 * orders_filled / orders_submitted : 0);
        printf("  Cancelled: %lu\n", orders_cancelled);
        printf("  Rejected: %lu\n", orders_rejected);
        printf("\nLatency:\n");
        printf("  Avg Event: %.2f us\n", avg_event_latency / 1000.0);
        printf("  Max Event: %.2f us\n", max_event_latency / 1000.0);
        printf("  P99 Event: %.2f us\n", p99_event_latency / 1000.0);
        printf("  Avg Order: %.2f us\n", avg_order_latency / 1000.0);
        printf("\nPnL:\n");
        printf("  Total: $%.2f\n", total_pnl);
        printf("  Realized: $%.2f\n", realized_pnl);
        printf("  Unrealized: $%.2f\n", unrealized_pnl);
        printf("  Max Drawdown: %.2f%%\n", max_drawdown * 100);
        printf("  Sharpe Ratio: %.2f\n", sharpe_ratio);
        printf("=================================\n");
    }
};

/**
 * @brief A/B Test variant
 */
struct ABVariant {
    std::string name;
    std::unique_ptr<IStrategy> strategy;
    std::unique_ptr<RiskManager> risk_manager;
    ShadowStats stats;
    bool is_control = false;
};

/**
 * @brief Shadow Trading Engine
 *
 * Main engine for running shadow trading simulations.
 * Manages the event loop, order books, matching engine,
 * and strategy execution.
 */
class ShadowEngine {
public:
    ShadowEngine(const ShadowConfig& config = {})
        : config_(config), bus_(), loop_(bus_),
          running_(false) {
        // Initialize components
        for (const auto& symbol : config_.symbols) {
            books_.get_l3(symbol);  // Create order books
        }

        if (config_.log_trades || config_.log_book_updates) {
            logger_ = std::make_unique<BinaryLogger>(LoggerConfig{
                .base_path = config_.log_path
            });
        }
    }

    /**
     * @brief Add a strategy to run in shadow mode
     */
    void add_strategy(std::unique_ptr<IStrategy> strategy,
                      std::unique_ptr<RiskManager> risk_manager,
                      const std::string& name = "default",
                      bool is_control = false) {
        ABVariant variant;
        variant.name = name;
        variant.strategy = std::move(strategy);
        variant.risk_manager = std::move(risk_manager);
        variant.is_control = is_control;
        variants_.push_back(std::move(variant));
    }

    /**
     * @brief Start shadow trading
     */
    void start() {
        if (running_) return;

        running_ = true;
        stats_.start_time = now_ns();

        // Initialize strategies
        for (auto& variant : variants_) {
            variant.strategy->initialize();
            variant.strategy->start();
        }

        // Start logger
        if (logger_ && !config_.symbols.empty()) {
            logger_->open(config_.symbols[0]);
        }

        // Subscribe to events
        subscribe_events();

        // Run event loop
        if (config_.mode == ShadowMode::Live) {
            run_live();
        } else if (config_.mode == ShadowMode::Replay) {
            // Replay handled externally
        }
    }

    /**
     * @brief Stop shadow trading
     */
    void stop() {
        if (!running_) return;

        running_ = false;

        // Stop strategies
        for (auto& variant : variants_) {
            variant.strategy->stop();
        }

        // Finalize stats
        stats_.end_time = now_ns();
        stats_.total_duration = stats_.end_time - stats_.start_time;

        // Close logger
        if (logger_) {
            logger_->close();
        }
    }

    /**
     * @brief Process a market data event (for replay mode)
     */
    void process_market_data(const MarketDataEvent& event) {
        Timestamp start = now_ns();

        // Update order book
        update_book_from_market_data(event);

        // Forward to strategies
        for (auto& variant : variants_) {
            variant.strategy->on_market_data(event);
        }

        // Log
        if (logger_ && config_.log_book_updates) {
            std::vector<PriceLevel> bids, asks;
            for (int i = 0; i < event.bid_levels; ++i) {
                bids.emplace_back(event.bid_prices[i], event.bid_sizes[i]);
            }
            for (int i = 0; i < event.ask_levels; ++i) {
                asks.emplace_back(event.ask_prices[i], event.ask_sizes[i]);
            }
            logger_->log_book_snapshot(event.symbol, event.seq_num, bids, asks);
        }

        // Stats
        ++stats_.market_data_events;
        record_latency(now_ns() - start);
    }

    /**
     * @brief Process a trade event
     */
    void process_trade(const TradeEvent& event) {
        ++stats_.trade_events;

        // Log trade
        if (logger_ && config_.log_trades) {
            logger_->log_trade(event.symbol, event.trade_id,
                              event.price, event.quantity, event.aggressor_side);
        }

        // Forward to strategies
        for (auto& variant : variants_) {
            variant.strategy->on_trade(event);
        }
    }

    /**
     * @brief Process an order from a strategy
     */
    void process_order(const OrderEvent& event, size_t variant_idx) {
        if (variant_idx >= variants_.size()) return;

        auto& variant = variants_[variant_idx];
        ++variant.stats.orders_submitted;
        ++stats_.orders_submitted;

        // Simulate latency
        if (config_.simulated_latency_ns > 0) {
            // In real implementation, would delay execution
        }

        // Get matching engine for the symbol
        L3OrderBook& book = books_.get_l3(event.order.symbol);

        // Create matching engine on demand
        auto me = std::make_unique<MatchingEngine>(bus_, book);

        // Apply slippage
        Order order = event.order;
        if (config_.slippage_bps > 0) {
            double slippage = from_price(order.price) * config_.slippage_bps / 10000;
            if (order.side == Side::Buy) {
                order.price = to_price(from_price(order.price) + slippage);
            } else {
                order.price = to_price(from_price(order.price) - slippage);
            }
        }

        // Execute
        ExecutionReport report = me->submit_order(order);

        // Apply commission
        if (report.filled_qty > 0 && config_.commission_bps > 0) {
            double commission = from_price(report.avg_price) *
                               from_quantity(report.filled_qty) *
                               config_.commission_bps / 10000;
            variant.stats.realized_pnl -= commission;
        }

        // Update stats
        switch (report.status) {
            case OrderStatus::Filled:
            case OrderStatus::PartiallyFilled:
                ++variant.stats.orders_filled;
                ++stats_.orders_filled;
                break;
            case OrderStatus::Cancelled:
                ++variant.stats.orders_cancelled;
                ++stats_.orders_cancelled;
                break;
            case OrderStatus::Rejected:
                ++variant.stats.orders_rejected;
                ++stats_.orders_rejected;
                break;
            default:
                break;
        }

        // Notify strategy
        OrderEvent response(report.status == OrderStatus::Filled ?
                           EventType::OrderFilled : EventType::OrderRejected);
        response.order = order;
        response.fill_price = report.avg_price;
        response.fill_qty = report.filled_qty;
        variant.strategy->on_order_update(response);
    }

    /**
     * @brief Get statistics
     */
    const ShadowStats& stats() const { return stats_; }

    /**
     * @brief Get variant statistics
     */
    const ShadowStats& variant_stats(size_t idx) const {
        return variants_.at(idx).stats;
    }

    /**
     * @brief Run A/B test comparison
     */
    void compare_variants() const {
        printf("\n=== A/B Test Results ===\n");

        const ABVariant* control = nullptr;
        for (const auto& v : variants_) {
            if (v.is_control) {
                control = &v;
                break;
            }
        }

        for (const auto& v : variants_) {
            printf("\nVariant: %s%s\n", v.name.c_str(),
                   v.is_control ? " (CONTROL)" : "");
            printf("  Orders: %lu submitted, %lu filled\n",
                   v.stats.orders_submitted, v.stats.orders_filled);
            printf("  PnL: $%.2f (realized) + $%.2f (unrealized) = $%.2f\n",
                   v.stats.realized_pnl, v.stats.unrealized_pnl,
                   v.stats.realized_pnl + v.stats.unrealized_pnl);

            if (control && &v != control) {
                double pnl_diff = (v.stats.realized_pnl + v.stats.unrealized_pnl) -
                                 (control->stats.realized_pnl + control->stats.unrealized_pnl);
                printf("  vs Control: %+.2f (%.1f%%)\n",
                       pnl_diff,
                       control->stats.total_pnl != 0 ?
                           100.0 * pnl_diff / std::abs(control->stats.total_pnl) : 0);
            }
        }
        printf("========================\n");
    }

    EventBus& bus() { return bus_; }
    TradingEventLoop& loop() { return loop_; }

private:
    void subscribe_events() {
        // Subscribe to market data events
        bus_.subscribe<MarketDataEvent>(EventType::MarketDataSnapshot,
            [this](const MarketDataEvent& e) {
                process_market_data(e);
            });

        bus_.subscribe<TradeEvent>(EventType::TradeEvent,
            [this](const TradeEvent& e) {
                process_trade(e);
            });

        // Subscribe to order events from strategies
        bus_.subscribe<OrderEvent>(EventType::OrderNew,
            [this](const OrderEvent& e) {
                // Find which strategy submitted this order
                for (size_t i = 0; i < variants_.size(); ++i) {
                    if (variants_[i].strategy->config().id == e.order.strategy_id) {
                        process_order(e, i);
                        break;
                    }
                }
            });
    }

    void run_live() {
        while (running_) {
            loop_.run_once(100);  // 100ms timeout

            // Periodic stats
            if (config_.record_latency) {
                // Print stats periodically
            }
        }
    }

    void update_book_from_market_data(const MarketDataEvent& event) {
        L2OrderBook& book = books_.get_l2(event.symbol);

        std::vector<PriceLevel> bids, asks;
        for (int i = 0; i < event.bid_levels; ++i) {
            bids.emplace_back(event.bid_prices[i], event.bid_sizes[i]);
        }
        for (int i = 0; i < event.ask_levels; ++i) {
            asks.emplace_back(event.ask_prices[i], event.ask_sizes[i]);
        }

        book.set_snapshot(bids, asks);
    }

    void record_latency(Duration latency) {
        latency_samples_.push_back(latency);

        // Calculate stats periodically
        if (latency_samples_.size() >= 1000) {
            Duration sum = 0, max_lat = 0;
            for (auto l : latency_samples_) {
                sum += l;
                max_lat = std::max(max_lat, l);
            }

            stats_.avg_event_latency = sum / latency_samples_.size();
            stats_.max_event_latency = max_lat;

            // P99
            std::sort(latency_samples_.begin(), latency_samples_.end());
            stats_.p99_event_latency = latency_samples_[latency_samples_.size() * 99 / 100];

            latency_samples_.clear();
        }
    }

    ShadowConfig config_;
    EventBus bus_;
    TradingEventLoop loop_;
    OrderBookManager books_;

    std::vector<ABVariant> variants_;
    std::unique_ptr<BinaryLogger> logger_;

    ShadowStats stats_;
    std::vector<Duration> latency_samples_;

    std::atomic<bool> running_;
};

}  // namespace titans
