/**
 * @file event_bus.hpp
 * @brief High-Performance Event Bus for Titans Trading System
 *
 * The central nervous system of the trading engine. All components
 * communicate through events, enabling loose coupling and testability.
 *
 * Features:
 * - Type-safe event dispatching
 * - Priority-based event handling
 * - Lock-free event queues
 * - Event batching for throughput optimization
 */

#pragma once

#include "types.hpp"
#include "spsc_queue.hpp"
#include "memory_pool.hpp"

#include <functional>
#include <unordered_map>
#include <vector>
#include <memory>
#include <typeindex>
#include <any>
#include <variant>

namespace titans {

// Forward declarations
class EventBus;

/**
 * @brief Base event structure
 *
 * All events inherit from this base class.
 * Uses 64-byte alignment for cache efficiency.
 */
struct alignas(64) Event {
    EventType   type;
    Timestamp   timestamp;
    SequenceNum seq_num;
    uint32_t    source_id;
    uint32_t    priority;    // Lower = higher priority

    Event() : type(EventType::None), timestamp(0), seq_num(0),
              source_id(0), priority(100) {}

    explicit Event(EventType t) : type(t), timestamp(now_ns()),
                                   seq_num(0), source_id(0), priority(100) {}

    virtual ~Event() = default;
};

/**
 * @brief Market data snapshot event
 */
struct MarketDataEvent : public Event {
    Symbol      symbol;
    Price       bid_prices[MAX_BOOK_LEVELS];
    Quantity    bid_sizes[MAX_BOOK_LEVELS];
    Price       ask_prices[MAX_BOOK_LEVELS];
    Quantity    ask_sizes[MAX_BOOK_LEVELS];
    uint8_t     bid_levels;
    uint8_t     ask_levels;
    Price       last_price;
    Quantity    last_size;
    Quantity    volume_24h;
    Price       high_24h;
    Price       low_24h;

    MarketDataEvent() : Event(EventType::MarketDataSnapshot),
                        bid_levels(0), ask_levels(0),
                        last_price(0), last_size(0),
                        volume_24h(0), high_24h(0), low_24h(0) {
        std::memset(bid_prices, 0, sizeof(bid_prices));
        std::memset(bid_sizes, 0, sizeof(bid_sizes));
        std::memset(ask_prices, 0, sizeof(ask_prices));
        std::memset(ask_sizes, 0, sizeof(ask_sizes));
    }
};

/**
 * @brief Order book update event (incremental)
 */
struct BookUpdateEvent : public Event {
    Symbol      symbol;
    Side        side;
    Price       price;
    Quantity    quantity;     // 0 means delete level
    bool        is_snapshot;

    BookUpdateEvent() : Event(EventType::BookUpdateEvent),
                        side(Side::Buy), price(0), quantity(0),
                        is_snapshot(false) {}
};

/**
 * @brief Trade event
 */
struct TradeEvent : public Event {
    Symbol      symbol;
    uint64_t    trade_id;
    Price       price;
    Quantity    quantity;
    Side        aggressor_side;

    TradeEvent() : Event(EventType::TradeEvent),
                   trade_id(0), price(0), quantity(0),
                   aggressor_side(Side::Buy) {}
};

/**
 * @brief Order event (new, cancel, fill, etc.)
 */
struct OrderEvent : public Event {
    Order       order;
    Price       fill_price;
    Quantity    fill_qty;
    std::array<char, 64> reject_reason;

    OrderEvent() : Event(EventType::OrderNew),
                   fill_price(0), fill_qty(0) {
        reject_reason.fill('\0');
    }

    explicit OrderEvent(EventType t) : Event(t),
                                        fill_price(0), fill_qty(0) {
        reject_reason.fill('\0');
    }
};

/**
 * @brief Signal event from strategy
 */
struct SignalEvent : public Event {
    Symbol      symbol;
    double      signal_value;    // -1.0 to 1.0
    double      confidence;      // 0.0 to 1.0
    uint32_t    strategy_id;
    std::array<char, 32> signal_name;

    SignalEvent() : Event(EventType::SignalGenerated),
                    signal_value(0), confidence(0), strategy_id(0) {
        signal_name.fill('\0');
    }
};

/**
 * @brief Timer event
 */
struct TimerEvent : public Event {
    uint64_t    timer_id;
    uint64_t    user_data;

    TimerEvent() : Event(EventType::TimerExpired),
                   timer_id(0), user_data(0) {}
};

/**
 * @brief Event handler interface
 */
class IEventHandler {
public:
    virtual ~IEventHandler() = default;
    virtual void on_event(const Event& event) = 0;
    virtual EventType handled_type() const = 0;
};

/**
 * @brief Type-safe event handler wrapper
 */
template <typename EventT>
class TypedEventHandler : public IEventHandler {
public:
    using HandlerFunc = std::function<void(const EventT&)>;

    explicit TypedEventHandler(HandlerFunc handler, EventType type)
        : handler_(std::move(handler)), type_(type) {}

    void on_event(const Event& event) override {
        handler_(static_cast<const EventT&>(event));
    }

    EventType handled_type() const override {
        return type_;
    }

private:
    HandlerFunc handler_;
    EventType type_;
};

/**
 * @brief Central Event Bus
 *
 * Routes events between components with minimal latency.
 * Uses SPSC queues for each handler to avoid contention.
 */
class EventBus {
public:
    static constexpr size_t EVENT_QUEUE_SIZE = 65536;  // Must be power of 2

    // The 64K-slot queue is ~8MB; keep it on the heap so an EventBus can
    // safely live on the stack.
    EventBus()
        : event_queue_(std::make_unique<MPSCQueue<Event, EVENT_QUEUE_SIZE>>()),
          seq_num_(0), running_(false) {}

    ~EventBus() = default;

    // Non-copyable
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    /**
     * @brief Subscribe to a specific event type
     */
    template <typename EventT>
    void subscribe(EventType type, std::function<void(const EventT&)> handler) {
        auto wrapper = std::make_unique<TypedEventHandler<EventT>>(
            std::move(handler), type);
        handlers_[type].push_back(std::move(wrapper));
    }

    /**
     * @brief Subscribe with lambda
     */
    template <typename EventT, typename Handler>
    void subscribe(EventType type, Handler&& handler) {
        subscribe<EventT>(type, std::function<void(const EventT&)>(
            std::forward<Handler>(handler)));
    }

    /**
     * @brief Publish an event (synchronous dispatch)
     */
    void publish(Event& event) {
        event.seq_num = ++seq_num_;
        if (event.timestamp == 0) {
            event.timestamp = now_ns();
        }

        auto it = handlers_.find(event.type);
        if (it != handlers_.end()) {
            for (auto& handler : it->second) {
                handler->on_event(event);
            }
        }

        // Also notify wildcard handlers
        auto wildcard_it = handlers_.find(EventType::None);
        if (wildcard_it != handlers_.end()) {
            for (auto& handler : wildcard_it->second) {
                handler->on_event(event);
            }
        }
    }

    /**
     * @brief Queue an event for later dispatch
     */
    bool queue(const Event& event) {
        return event_queue_->try_push(event);
    }

    /**
     * @brief Process all queued events
     * @return Number of events processed
     */
    size_t process_queued() {
        size_t count = 0;
        Event event;

        while (event_queue_->try_pop(event)) {
            publish(event);
            ++count;
        }

        return count;
    }

    /**
     * @brief Process up to max_events queued events
     */
    size_t process_queued(size_t max_events) {
        size_t count = 0;
        Event event;

        while (count < max_events && event_queue_->try_pop(event)) {
            publish(event);
            ++count;
        }

        return count;
    }

    /**
     * @brief Get number of handlers for an event type
     */
    size_t handler_count(EventType type) const {
        auto it = handlers_.find(type);
        return it != handlers_.end() ? it->second.size() : 0;
    }

    /**
     * @brief Get number of queued events
     */
    size_t queued_count() const {
        return event_queue_->size();
    }

    /**
     * @brief Get total events published
     */
    SequenceNum total_published() const {
        return seq_num_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Clear all handlers
     */
    void clear_handlers() {
        handlers_.clear();
    }

private:
    std::unordered_map<EventType, std::vector<std::unique_ptr<IEventHandler>>> handlers_;
    std::unique_ptr<MPSCQueue<Event, EVENT_QUEUE_SIZE>> event_queue_;
    std::atomic<SequenceNum> seq_num_;
    std::atomic<bool> running_;
};

/**
 * @brief Event publisher helper class
 *
 * Components can inherit from this to easily publish events.
 */
class EventPublisher {
public:
    explicit EventPublisher(EventBus& bus) : bus_(bus), source_id_(0) {}

    void set_source_id(uint32_t id) { source_id_ = id; }

protected:
    template <typename EventT>
    void publish(EventT& event) {
        event.source_id = source_id_;
        bus_.publish(event);
    }

    template <typename EventT>
    bool queue(EventT event) {
        event.source_id = source_id_;
        return bus_.queue(event);
    }

    EventBus& bus_;
    uint32_t source_id_;
};

/**
 * @brief Event statistics collector
 */
class EventStats {
public:
    void record(const Event& event, Duration processing_time) {
        auto& stats = stats_[event.type];
        stats.count++;
        stats.total_latency += processing_time;
        stats.max_latency = std::max(stats.max_latency, processing_time);
        stats.min_latency = std::min(stats.min_latency, processing_time);
    }

    struct TypeStats {
        uint64_t count = 0;
        Duration total_latency = 0;
        Duration max_latency = 0;
        Duration min_latency = std::numeric_limits<Duration>::max();

        double avg_latency() const {
            return count > 0 ? static_cast<double>(total_latency) / count : 0;
        }
    };

    const TypeStats& get_stats(EventType type) const {
        static TypeStats empty;
        auto it = stats_.find(type);
        return it != stats_.end() ? it->second : empty;
    }

    void reset() { stats_.clear(); }

private:
    std::unordered_map<EventType, TypeStats> stats_;
};

}  // namespace titans
