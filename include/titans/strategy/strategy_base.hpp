/**
 * @file strategy_base.hpp
 * @brief Base Classes for Trading Strategies
 *
 * Provides the foundation for implementing trading strategies:
 * - Strategy lifecycle management
 * - Event handling interface
 * - Position and order management
 * - Performance tracking
 */

#pragma once

#include "titans/core/types.hpp"
#include "titans/core/event_bus.hpp"
#include "titans/trading/order_book.hpp"
#include "titans/trading/risk_manager.hpp"

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>

namespace titans {

// Forward declarations
class StrategyEngine;

/**
 * @brief Strategy configuration
 */
struct StrategyConfig {
    std::string name;
    uint32_t id;
    std::vector<Symbol> symbols;
    double initial_capital = 100000.0;
    double max_position_pct = 0.1;  // Max position as % of capital
    bool enabled = true;

    // Timing parameters
    Duration min_order_interval_ns = 1000000;  // 1ms
    Duration signal_decay_ns = 5000000000;     // 5s

    // Risk parameters
    double max_daily_loss_pct = 2.0;
    double stop_loss_pct = 1.0;
    double take_profit_pct = 2.0;
};

/**
 * @brief Strategy state
 */
enum class StrategyState {
    Initializing,
    Running,
    Paused,
    Stopped,
    Error
};

/**
 * @brief Strategy performance metrics
 */
struct StrategyMetrics {
    // PnL
    double realized_pnl = 0;
    double unrealized_pnl = 0;
    double total_pnl = 0;
    double max_drawdown = 0;
    double peak_equity = 0;

    // Trading
    uint64_t num_trades = 0;
    uint64_t winning_trades = 0;
    uint64_t losing_trades = 0;
    double win_rate = 0;
    double avg_win = 0;
    double avg_loss = 0;
    double profit_factor = 0;

    // Risk
    double sharpe_ratio = 0;
    double sortino_ratio = 0;
    double calmar_ratio = 0;

    // Activity
    uint64_t num_signals = 0;
    uint64_t num_orders = 0;
    uint64_t num_fills = 0;
    uint64_t num_cancels = 0;

    void update_pnl(double realized, double unrealized) {
        realized_pnl = realized;
        unrealized_pnl = unrealized;
        total_pnl = realized + unrealized;

        double equity = total_pnl;
        if (equity > peak_equity) {
            peak_equity = equity;
        }
        if (peak_equity > 0) {
            double drawdown = (peak_equity - equity) / peak_equity;
            if (drawdown > max_drawdown) {
                max_drawdown = drawdown;
            }
        }
    }

    void record_trade(double pnl) {
        ++num_trades;
        if (pnl > 0) {
            ++winning_trades;
            avg_win = (avg_win * (winning_trades - 1) + pnl) / winning_trades;
        } else if (pnl < 0) {
            ++losing_trades;
            avg_loss = (avg_loss * (losing_trades - 1) + std::abs(pnl)) / losing_trades;
        }

        if (num_trades > 0) {
            win_rate = static_cast<double>(winning_trades) / num_trades;
        }
        if (avg_loss > 0) {
            profit_factor = (avg_win * winning_trades) / (avg_loss * losing_trades);
        }
    }
};

/**
 * @brief Trading signal
 */
struct Signal {
    Symbol      symbol;
    Side        side;
    double      strength;     // -1.0 to 1.0
    double      confidence;   // 0.0 to 1.0
    Price       target_price;
    Quantity    suggested_qty;
    Timestamp   generated_at;
    Duration    valid_for;
    std::string reason;

    bool is_valid() const {
        return now_ns() - generated_at < valid_for;
    }

    bool is_buy() const { return strength > 0; }
    bool is_sell() const { return strength < 0; }
};

/**
 * @brief Base class for all trading strategies
 */
class IStrategy {
public:
    virtual ~IStrategy() = default;

    // Lifecycle
    virtual void initialize() = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;

    // Event handlers
    virtual void on_market_data(const MarketDataEvent& event) = 0;
    virtual void on_book_update(const BookUpdateEvent& event) = 0;
    virtual void on_trade(const TradeEvent& event) = 0;
    virtual void on_order_update(const OrderEvent& event) = 0;
    virtual void on_timer(uint64_t timer_id) = 0;

    // Accessors
    virtual const StrategyConfig& config() const = 0;
    virtual StrategyState state() const = 0;
    virtual const StrategyMetrics& metrics() const = 0;
};

/**
 * @brief Strategy base class with common functionality
 */
class StrategyBase : public IStrategy, public EventPublisher {
public:
    StrategyBase(EventBus& bus, RiskManager& risk, const StrategyConfig& config)
        : EventPublisher(bus), risk_(risk), config_(config),
          state_(StrategyState::Initializing) {
        set_source_id(config.id);
    }

    // Lifecycle implementation
    void initialize() override {
        on_initialize();
        state_ = StrategyState::Stopped;
    }

    void start() override {
        if (state_ == StrategyState::Stopped ||
            state_ == StrategyState::Paused) {
            on_start();
            state_ = StrategyState::Running;
        }
    }

    void stop() override {
        if (state_ == StrategyState::Running ||
            state_ == StrategyState::Paused) {
            cancel_all_orders();
            on_stop();
            state_ = StrategyState::Stopped;
        }
    }

    void pause() override {
        if (state_ == StrategyState::Running) {
            on_pause();
            state_ = StrategyState::Paused;
        }
    }

    void resume() override {
        if (state_ == StrategyState::Paused) {
            on_resume();
            state_ = StrategyState::Running;
        }
    }

    // Accessors
    const StrategyConfig& config() const override { return config_; }
    StrategyState state() const override { return state_; }
    const StrategyMetrics& metrics() const override { return metrics_; }

protected:
    // Override these in derived classes
    virtual void on_initialize() {}
    virtual void on_start() {}
    virtual void on_stop() {}
    virtual void on_pause() {}
    virtual void on_resume() {}

    /**
     * @brief Generate a signal
     */
    void generate_signal(const Signal& signal) {
        ++metrics_.num_signals;
        last_signals_[signal.symbol] = signal;

        SignalEvent event;
        event.symbol = signal.symbol;
        event.signal_value = signal.strength;
        event.confidence = signal.confidence;
        event.strategy_id = config_.id;
        publish(event);

        on_signal(signal);
    }

    /**
     * @brief Called when a signal is generated
     */
    virtual void on_signal(const Signal& signal) {
        // Default: convert signal to order
        if (signal.confidence > 0.5 && std::abs(signal.strength) > 0.3) {
            Side side = signal.is_buy() ? Side::Buy : Side::Sell;
            submit_order(signal.symbol, side, signal.suggested_qty, signal.target_price);
        }
    }

    /**
     * @brief Submit an order
     */
    OrderId submit_order(const Symbol& symbol, Side side, Quantity qty, Price price,
                         OrderType type = OrderType::Limit) {
        // Create order
        Order order;
        order.id = next_order_id_++;
        order.symbol = symbol;
        order.side = side;
        order.type = type;
        order.quantity = qty;
        order.price = price;
        order.strategy_id = config_.id;
        order.created_at = now_ns();

        // Risk check
        auto risk_result = risk_.check_order(order);
        if (!risk_result.passed) {
            // Log rejection
            return INVALID_ORDER_ID;
        }

        // Rate limiting
        Timestamp now = now_ns();
        if (now - last_order_time_ < config_.min_order_interval_ns) {
            return INVALID_ORDER_ID;
        }
        last_order_time_ = now;

        // Track order
        pending_orders_[order.id] = order;
        ++metrics_.num_orders;

        // Publish order event
        OrderEvent event(EventType::OrderNew);
        event.order = order;
        publish(event);

        return order.id;
    }

    /**
     * @brief Cancel an order
     */
    bool cancel_order(OrderId order_id) {
        auto it = pending_orders_.find(order_id);
        if (it == pending_orders_.end()) {
            return false;
        }

        OrderEvent event(EventType::OrderCancelled);
        event.order = it->second;
        publish(event);

        pending_orders_.erase(it);
        ++metrics_.num_cancels;

        return true;
    }

    /**
     * @brief Cancel all pending orders
     */
    void cancel_all_orders() {
        for (auto& [id, order] : pending_orders_) {
            OrderEvent event(EventType::OrderCancelled);
            event.order = order;
            publish(event);
            ++metrics_.num_cancels;
        }
        pending_orders_.clear();
    }

    /**
     * @brief Get current position for a symbol
     */
    const Position* get_position(const Symbol& symbol) const {
        return risk_.get_position(symbol);
    }

    /**
     * @brief Get last signal for a symbol
     */
    const Signal* get_last_signal(const Symbol& symbol) const {
        auto it = last_signals_.find(symbol);
        if (it != last_signals_.end() && it->second.is_valid()) {
            return &it->second;
        }
        return nullptr;
    }

    /**
     * @brief Update metrics on fill
     */
    void on_fill_internal(const Order& order, Price fill_price, Quantity fill_qty) {
        ++metrics_.num_fills;

        // Update risk manager
        risk_.on_fill(order.symbol, order.side, fill_qty, fill_price);

        // Update PnL metrics
        const Position* pos = get_position(order.symbol);
        if (pos) {
            metrics_.update_pnl(risk_.total_realized_pnl(),
                               risk_.total_unrealized_pnl());
        }

        // Remove from pending if fully filled
        auto it = pending_orders_.find(order.id);
        if (it != pending_orders_.end()) {
            it->second.filled_qty += fill_qty;
            if (it->second.remaining() == 0) {
                pending_orders_.erase(it);
            }
        }
    }

    RiskManager& risk_;
    StrategyConfig config_;
    StrategyState state_;
    StrategyMetrics metrics_;

    std::unordered_map<OrderId, Order> pending_orders_;
    std::unordered_map<Symbol, Signal, SymbolHash> last_signals_;

    OrderId next_order_id_ = 1;
    Timestamp last_order_time_ = 0;
};

/**
 * @brief Simple momentum strategy example
 */
class MomentumStrategy : public StrategyBase {
public:
    MomentumStrategy(EventBus& bus, RiskManager& risk, const StrategyConfig& config,
                     int lookback = 20, double threshold = 0.02)
        : StrategyBase(bus, risk, config),
          lookback_(lookback), threshold_(threshold) {}

    void on_market_data(const MarketDataEvent& event) override {
        if (state_ != StrategyState::Running) return;

        // Store price
        auto& prices = price_history_[event.symbol];
        prices.push_back(from_price(event.last_price));

        if (prices.size() > static_cast<size_t>(lookback_ * 2)) {
            prices.erase(prices.begin());
        }

        // Need enough history
        if (prices.size() < static_cast<size_t>(lookback_)) return;

        // Calculate momentum
        double current = prices.back();
        double past = prices[prices.size() - lookback_];
        double momentum = (current - past) / past;

        // Generate signal
        if (std::abs(momentum) > threshold_) {
            Signal signal;
            signal.symbol = event.symbol;
            signal.side = momentum > 0 ? Side::Buy : Side::Sell;
            signal.strength = std::min(1.0, std::abs(momentum) / threshold_);
            if (momentum < 0) signal.strength = -signal.strength;
            signal.confidence = 0.6;
            signal.target_price = event.last_price;
            signal.suggested_qty = to_quantity(config_.initial_capital *
                                               config_.max_position_pct /
                                               from_price(event.last_price));
            signal.generated_at = now_ns();
            signal.valid_for = config_.signal_decay_ns;
            signal.reason = "Momentum: " + std::to_string(momentum * 100) + "%";

            generate_signal(signal);
        }
    }

    void on_book_update(const BookUpdateEvent& event) override {}
    void on_trade(const TradeEvent& event) override {}

    void on_order_update(const OrderEvent& event) override {
        if (event.order.strategy_id != config_.id) return;

        if (event.type == EventType::OrderFilled ||
            event.type == EventType::OrderPartialFill) {
            on_fill_internal(event.order, event.fill_price, event.fill_qty);
        }
    }

    void on_timer(uint64_t timer_id) override {}

private:
    int lookback_;
    double threshold_;
    std::unordered_map<Symbol, std::vector<double>, SymbolHash> price_history_;
};

/**
 * @brief Strategy engine for managing multiple strategies
 */
class StrategyEngine {
public:
    explicit StrategyEngine(EventBus& bus) : bus_(bus) {}

    /**
     * @brief Add a strategy
     */
    void add_strategy(std::unique_ptr<IStrategy> strategy) {
        strategies_.push_back(std::move(strategy));
    }

    /**
     * @brief Initialize all strategies
     */
    void initialize() {
        for (auto& strategy : strategies_) {
            strategy->initialize();
        }
    }

    /**
     * @brief Start all strategies
     */
    void start() {
        for (auto& strategy : strategies_) {
            strategy->start();
        }
    }

    /**
     * @brief Stop all strategies
     */
    void stop() {
        for (auto& strategy : strategies_) {
            strategy->stop();
        }
    }

    /**
     * @brief Dispatch market data to strategies
     */
    void on_market_data(const MarketDataEvent& event) {
        for (auto& strategy : strategies_) {
            if (strategy->state() == StrategyState::Running) {
                strategy->on_market_data(event);
            }
        }
    }

    /**
     * @brief Get strategy by ID
     */
    IStrategy* get_strategy(uint32_t id) {
        for (auto& strategy : strategies_) {
            if (strategy->config().id == id) {
                return strategy.get();
            }
        }
        return nullptr;
    }

    size_t strategy_count() const { return strategies_.size(); }

private:
    EventBus& bus_;
    std::vector<std::unique_ptr<IStrategy>> strategies_;
};

}  // namespace titans
