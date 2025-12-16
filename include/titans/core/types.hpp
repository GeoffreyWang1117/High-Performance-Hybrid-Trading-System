/**
 * @file types.hpp
 * @brief Core type definitions for Titans Trading System
 *
 * This header defines fundamental types used throughout the system,
 * optimized for low-latency trading operations.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <array>
#include <chrono>
#include <string_view>
#include <atomic>
#include <limits>

namespace titans {

// ============================================================================
// Time Types
// ============================================================================

using Timestamp = int64_t;  // Nanoseconds since epoch
using Duration = int64_t;   // Nanoseconds

inline Timestamp now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

inline Timestamp now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

// ============================================================================
// Price/Quantity Types (Fixed-point for precision)
// ============================================================================

using Price = int64_t;      // Price in fixed-point (8 decimals)
using Quantity = int64_t;   // Quantity in fixed-point (8 decimals)
using OrderId = uint64_t;
using SequenceNum = uint64_t;

constexpr int64_t PRICE_MULTIPLIER = 100000000LL;  // 10^8

inline Price to_price(double value) {
    return static_cast<Price>(value * PRICE_MULTIPLIER);
}

inline double from_price(Price value) {
    return static_cast<double>(value) / PRICE_MULTIPLIER;
}

inline Quantity to_quantity(double value) {
    return static_cast<Quantity>(value * PRICE_MULTIPLIER);
}

inline double from_quantity(Quantity value) {
    return static_cast<double>(value) / PRICE_MULTIPLIER;
}

// ============================================================================
// Symbol Type (Fixed-size for cache efficiency)
// ============================================================================

constexpr size_t SYMBOL_MAX_LEN = 16;

struct Symbol {
    std::array<char, SYMBOL_MAX_LEN> data{};

    Symbol() = default;

    explicit Symbol(std::string_view sv) {
        size_t len = std::min(sv.size(), SYMBOL_MAX_LEN - 1);
        std::memcpy(data.data(), sv.data(), len);
        data[len] = '\0';
    }

    std::string_view view() const {
        return std::string_view(data.data());
    }

    bool operator==(const Symbol& other) const {
        return data == other.data;
    }

    bool operator<(const Symbol& other) const {
        return std::memcmp(data.data(), other.data.data(), SYMBOL_MAX_LEN) < 0;
    }
};

// Hash for Symbol
struct SymbolHash {
    size_t operator()(const Symbol& s) const {
        size_t hash = 0;
        for (size_t i = 0; i < SYMBOL_MAX_LEN && s.data[i]; ++i) {
            hash = hash * 31 + static_cast<size_t>(s.data[i]);
        }
        return hash;
    }
};

// ============================================================================
// Order Types
// ============================================================================

enum class Side : uint8_t {
    Buy = 0,
    Sell = 1
};

inline Side opposite(Side side) {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

enum class OrderType : uint8_t {
    Market = 0,
    Limit = 1,
    StopLoss = 2,
    StopLimit = 3,
    IOC = 4,       // Immediate or Cancel
    FOK = 5,       // Fill or Kill
    GTC = 6        // Good Till Cancel
};

enum class OrderStatus : uint8_t {
    New = 0,
    PartiallyFilled = 1,
    Filled = 2,
    Cancelled = 3,
    Rejected = 4,
    Expired = 5
};

enum class TimeInForce : uint8_t {
    GTC = 0,       // Good Till Cancel
    IOC = 1,       // Immediate or Cancel
    FOK = 2,       // Fill or Kill
    GTD = 3,       // Good Till Date
    Day = 4        // Day order
};

// ============================================================================
// Order Structure (Cache-line aligned)
// ============================================================================

struct alignas(64) Order {
    OrderId     id;
    Symbol      symbol;
    Side        side;
    OrderType   type;
    OrderStatus status;
    TimeInForce tif;
    Price       price;
    Quantity    quantity;
    Quantity    filled_qty;
    Timestamp   created_at;
    Timestamp   updated_at;
    uint32_t    client_id;
    uint32_t    strategy_id;

    Order() : id(0), side(Side::Buy), type(OrderType::Limit),
              status(OrderStatus::New), tif(TimeInForce::GTC),
              price(0), quantity(0), filled_qty(0),
              created_at(0), updated_at(0),
              client_id(0), strategy_id(0) {}

    Quantity remaining() const { return quantity - filled_qty; }
    bool is_active() const {
        return status == OrderStatus::New || status == OrderStatus::PartiallyFilled;
    }
};

// ============================================================================
// Trade Structure
// ============================================================================

struct alignas(64) Trade {
    uint64_t    trade_id;
    Symbol      symbol;
    OrderId     maker_order_id;
    OrderId     taker_order_id;
    Side        taker_side;
    Price       price;
    Quantity    quantity;
    Timestamp   timestamp;

    Trade() : trade_id(0), maker_order_id(0), taker_order_id(0),
              taker_side(Side::Buy), price(0), quantity(0), timestamp(0) {}
};

// ============================================================================
// Market Data Types
// ============================================================================

struct PriceLevel {
    Price    price;
    Quantity quantity;
    uint32_t order_count;

    PriceLevel() : price(0), quantity(0), order_count(0) {}
    PriceLevel(Price p, Quantity q, uint32_t c = 1)
        : price(p), quantity(q), order_count(c) {}
};

struct alignas(64) Quote {
    Symbol      symbol;
    Price       bid_price;
    Price       ask_price;
    Quantity    bid_qty;
    Quantity    ask_qty;
    Timestamp   timestamp;
    SequenceNum seq_num;

    Quote() : bid_price(0), ask_price(0), bid_qty(0),
              ask_qty(0), timestamp(0), seq_num(0) {}

    Price mid_price() const { return (bid_price + ask_price) / 2; }
    Price spread() const { return ask_price - bid_price; }
};

// ============================================================================
// Event Types
// ============================================================================

enum class EventType : uint16_t {
    None = 0,

    // Market Data Events (100-199)
    MarketDataSnapshot = 100,
    MarketDataUpdate = 101,
    TradeEvent = 102,
    QuoteEvent = 103,
    BookUpdateEvent = 104,

    // Order Events (200-299)
    OrderNew = 200,
    OrderAccepted = 201,
    OrderRejected = 202,
    OrderCancelled = 203,
    OrderFilled = 204,
    OrderPartialFill = 205,
    OrderModified = 206,

    // Strategy Events (300-399)
    SignalGenerated = 300,
    PositionUpdate = 301,
    PnLUpdate = 302,

    // System Events (400-499)
    Heartbeat = 400,
    SystemStatus = 401,
    Error = 402,
    Shutdown = 403,

    // Timer Events (500-599)
    TimerExpired = 500
};

// ============================================================================
// Constants
// ============================================================================

constexpr size_t CACHE_LINE_SIZE = 64;
constexpr size_t MAX_BOOK_LEVELS = 20;
constexpr size_t MAX_ORDERS_PER_SIDE = 100000;

constexpr OrderId INVALID_ORDER_ID = std::numeric_limits<OrderId>::max();
constexpr Price INVALID_PRICE = std::numeric_limits<Price>::max();

}  // namespace titans
