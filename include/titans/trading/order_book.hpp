/**
 * @file order_book.hpp
 * @brief High-Performance Order Book Implementation
 *
 * A cache-optimized limit order book supporting:
 * - O(1) best bid/ask access
 * - O(log n) order operations
 * - Efficient memory layout for L2/L3 cache
 *
 * Designed for reconstructing exchange order books from
 * market data feeds with sub-microsecond latency.
 */

#pragma once

#include "titans/core/types.hpp"
#include "titans/core/memory_pool.hpp"

#include <map>
#include <unordered_map>
#include <vector>
#include <optional>
#include <functional>
#include <algorithm>

namespace titans {

/**
 * @brief Order entry in the book
 */
struct BookOrder {
    OrderId     id;
    Price       price;
    Quantity    quantity;
    Quantity    filled;
    Timestamp   timestamp;
    uint32_t    client_id;

    BookOrder() : id(0), price(0), quantity(0), filled(0),
                  timestamp(0), client_id(0) {}

    Quantity remaining() const { return quantity - filled; }
};

/**
 * @brief Price level containing multiple orders (FIFO)
 */
class PriceLevelOrders {
public:
    PriceLevelOrders() : total_qty_(0), order_count_(0) {}

    void add_order(const BookOrder& order) {
        orders_.push_back(order);
        total_qty_ += order.remaining();
        ++order_count_;
    }

    bool remove_order(OrderId id) {
        for (auto it = orders_.begin(); it != orders_.end(); ++it) {
            if (it->id == id) {
                total_qty_ -= it->remaining();
                --order_count_;
                orders_.erase(it);
                return true;
            }
        }
        return false;
    }

    bool modify_order(OrderId id, Quantity new_qty) {
        for (auto& order : orders_) {
            if (order.id == id) {
                total_qty_ -= order.remaining();
                order.quantity = new_qty;
                total_qty_ += order.remaining();
                return true;
            }
        }
        return false;
    }

    // Fill orders at this level, returns total filled
    Quantity fill(Quantity qty, std::vector<std::pair<OrderId, Quantity>>& fills) {
        Quantity total_filled = 0;

        while (qty > 0 && !orders_.empty()) {
            BookOrder& front = orders_.front();
            Quantity fill_qty = std::min(qty, front.remaining());

            front.filled += fill_qty;
            qty -= fill_qty;
            total_filled += fill_qty;
            total_qty_ -= fill_qty;

            fills.push_back({front.id, fill_qty});

            if (front.remaining() == 0) {
                orders_.erase(orders_.begin());
                --order_count_;
            }
        }

        return total_filled;
    }

    Quantity total_quantity() const { return total_qty_; }
    uint32_t order_count() const { return order_count_; }
    bool empty() const { return orders_.empty(); }

    const std::vector<BookOrder>& orders() const { return orders_; }

    BookOrder* front_order() {
        return orders_.empty() ? nullptr : &orders_.front();
    }

private:
    std::vector<BookOrder> orders_;
    Quantity total_qty_;
    uint32_t order_count_;
};

/**
 * @brief L2 Order Book (aggregated price levels)
 *
 * Simpler than full order book, stores only aggregated
 * quantities at each price level. Suitable for most
 * market data feeds.
 */
class L2OrderBook {
public:
    explicit L2OrderBook(const Symbol& symbol) : symbol_(symbol),
        last_update_time_(0), seq_num_(0) {}

    /**
     * @brief Update a price level
     */
    void update_level(Side side, Price price, Quantity quantity) {
        auto& levels = (side == Side::Buy) ? bids_ : asks_;

        if (quantity == 0) {
            levels.erase(price);
        } else {
            levels[price] = PriceLevel(price, quantity);
        }

        last_update_time_ = now_ns();
        ++seq_num_;
    }

    /**
     * @brief Set full snapshot
     */
    void set_snapshot(const std::vector<PriceLevel>& bids,
                      const std::vector<PriceLevel>& asks) {
        bids_.clear();
        asks_.clear();

        for (const auto& level : bids) {
            bids_[level.price] = level;
        }
        for (const auto& level : asks) {
            asks_[level.price] = level;
        }

        last_update_time_ = now_ns();
        ++seq_num_;
    }

    /**
     * @brief Get best bid price and quantity
     */
    std::optional<PriceLevel> best_bid() const {
        if (bids_.empty()) return std::nullopt;
        return bids_.rbegin()->second;  // Highest price
    }

    /**
     * @brief Get best ask price and quantity
     */
    std::optional<PriceLevel> best_ask() const {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->second;   // Lowest price
    }

    /**
     * @brief Get mid price
     */
    std::optional<Price> mid_price() const {
        auto bid = best_bid();
        auto ask = best_ask();
        if (!bid || !ask) return std::nullopt;
        return (bid->price + ask->price) / 2;
    }

    /**
     * @brief Get spread
     */
    std::optional<Price> spread() const {
        auto bid = best_bid();
        auto ask = best_ask();
        if (!bid || !ask) return std::nullopt;
        return ask->price - bid->price;
    }

    /**
     * @brief Get top N price levels
     */
    void get_top_levels(size_t n,
                        std::vector<PriceLevel>& out_bids,
                        std::vector<PriceLevel>& out_asks) const {
        out_bids.clear();
        out_asks.clear();

        // Bids: descending price order
        size_t count = 0;
        for (auto it = bids_.rbegin(); it != bids_.rend() && count < n; ++it, ++count) {
            out_bids.push_back(it->second);
        }

        // Asks: ascending price order
        count = 0;
        for (auto it = asks_.begin(); it != asks_.end() && count < n; ++it, ++count) {
            out_asks.push_back(it->second);
        }
    }

    /**
     * @brief Calculate volume-weighted average price
     */
    std::optional<double> vwap(Side side, Quantity target_qty) const {
        Quantity remaining = target_qty;
        double total_value = 0;
        Quantity total_qty = 0;

        if (side == Side::Sell) {
            // For sells, iterate bids in reverse
            for (auto rit = bids_.rbegin(); rit != bids_.rend() && remaining > 0; ++rit) {
                Quantity fill = std::min(remaining, rit->second.quantity);
                total_value += from_price(rit->second.price) * from_quantity(fill);
                total_qty += fill;
                remaining -= fill;
            }
        } else {
            // For buys, iterate asks forward
            for (auto fit = asks_.begin(); fit != asks_.end() && remaining > 0; ++fit) {
                Quantity fill = std::min(remaining, fit->second.quantity);
                total_value += from_price(fit->second.price) * from_quantity(fill);
                total_qty += fill;
                remaining -= fill;
            }
        }

        if (total_qty == 0) return std::nullopt;
        return total_value / from_quantity(total_qty);
    }

    /**
     * @brief Get market depth (total quantity) up to price
     */
    Quantity depth_to_price(Side side, Price limit_price) const {
        Quantity total = 0;

        if (side == Side::Buy) {
            for (const auto& [price, level] : asks_) {
                if (price > limit_price) break;
                total += level.quantity;
            }
        } else {
            for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
                if (it->first < limit_price) break;
                total += it->second.quantity;
            }
        }

        return total;
    }

    /**
     * @brief Calculate book imbalance
     * @return Value between -1 (ask heavy) and 1 (bid heavy)
     */
    double imbalance(size_t levels = 5) const {
        Quantity bid_qty = 0, ask_qty = 0;
        size_t count = 0;

        for (auto it = bids_.rbegin(); it != bids_.rend() && count < levels; ++it, ++count) {
            bid_qty += it->second.quantity;
        }

        count = 0;
        for (auto it = asks_.begin(); it != asks_.end() && count < levels; ++it, ++count) {
            ask_qty += it->second.quantity;
        }

        if (bid_qty + ask_qty == 0) return 0;
        return static_cast<double>(bid_qty - ask_qty) / (bid_qty + ask_qty);
    }

    const Symbol& symbol() const { return symbol_; }
    Timestamp last_update_time() const { return last_update_time_; }
    SequenceNum seq_num() const { return seq_num_; }

    size_t bid_levels() const { return bids_.size(); }
    size_t ask_levels() const { return asks_.size(); }

    void clear() {
        bids_.clear();
        asks_.clear();
    }

private:
    Symbol symbol_;
    std::map<Price, PriceLevel> bids_;  // Price -> Level (descending)
    std::map<Price, PriceLevel> asks_;  // Price -> Level (ascending)
    Timestamp last_update_time_;
    SequenceNum seq_num_;
};

/**
 * @brief Full L3 Order Book with individual orders
 *
 * Maintains all individual orders for matching engine
 * simulation and precise market impact analysis.
 */
class L3OrderBook {
public:
    explicit L3OrderBook(const Symbol& symbol) : symbol_(symbol),
        next_order_id_(1), last_update_time_(0), seq_num_(0) {}

    /**
     * @brief Add a new order to the book
     * @return Order ID
     */
    OrderId add_order(Side side, Price price, Quantity quantity,
                      uint32_t client_id = 0) {
        OrderId id = next_order_id_++;

        BookOrder order;
        order.id = id;
        order.price = price;
        order.quantity = quantity;
        order.filled = 0;
        order.timestamp = now_ns();
        order.client_id = client_id;

        auto& levels = (side == Side::Buy) ? bid_levels_ : ask_levels_;
        levels[price].add_order(order);

        orders_[id] = {side, price};

        last_update_time_ = order.timestamp;
        ++seq_num_;

        return id;
    }

    /**
     * @brief Cancel an order
     * @return true if order was found and cancelled
     */
    bool cancel_order(OrderId id) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return false;

        auto& [side, price] = it->second;
        auto& levels = (side == Side::Buy) ? bid_levels_ : ask_levels_;

        auto level_it = levels.find(price);
        if (level_it != levels.end()) {
            level_it->second.remove_order(id);
            if (level_it->second.empty()) {
                levels.erase(level_it);
            }
        }

        orders_.erase(it);
        last_update_time_ = now_ns();
        ++seq_num_;

        return true;
    }

    /**
     * @brief Modify order quantity
     */
    bool modify_order(OrderId id, Quantity new_quantity) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return false;

        auto& [side, price] = it->second;
        auto& levels = (side == Side::Buy) ? bid_levels_ : ask_levels_;

        auto level_it = levels.find(price);
        if (level_it != levels.end()) {
            level_it->second.modify_order(id, new_quantity);
            last_update_time_ = now_ns();
            ++seq_num_;
            return true;
        }

        return false;
    }

    /**
     * @brief Execute a market order against the book
     * @return List of fills (order_id, fill_qty)
     */
    std::vector<std::pair<OrderId, Quantity>> execute_market_order(
            Side side, Quantity quantity) {
        std::vector<std::pair<OrderId, Quantity>> fills;

        // Market buy hits asks, market sell hits bids
        auto& levels = (side == Side::Buy) ? ask_levels_ : bid_levels_;

        Quantity remaining = quantity;

        while (remaining > 0 && !levels.empty()) {
            auto it = (side == Side::Buy) ? levels.begin()
                                          : std::prev(levels.end());

            Quantity filled = it->second.fill(remaining, fills);
            remaining -= filled;

            if (it->second.empty()) {
                levels.erase(it);
            }
        }

        // Clean up order index
        for (const auto& [order_id, fill_qty] : fills) {
            auto order_it = orders_.find(order_id);
            if (order_it != orders_.end()) {
                auto& [ord_side, ord_price] = order_it->second;
                auto& ord_levels = (ord_side == Side::Buy) ? bid_levels_ : ask_levels_;
                auto level_it = ord_levels.find(ord_price);
                if (level_it != ord_levels.end()) {
                    // Check if order fully filled
                    bool found = false;
                    for (const auto& ord : level_it->second.orders()) {
                        if (ord.id == order_id) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        orders_.erase(order_it);
                    }
                }
            }
        }

        last_update_time_ = now_ns();
        ++seq_num_;

        return fills;
    }

    /**
     * @brief Get best bid
     */
    std::optional<PriceLevel> best_bid() const {
        if (bid_levels_.empty()) return std::nullopt;
        auto it = bid_levels_.rbegin();
        return PriceLevel(it->first, it->second.total_quantity(),
                          it->second.order_count());
    }

    /**
     * @brief Get best ask
     */
    std::optional<PriceLevel> best_ask() const {
        if (ask_levels_.empty()) return std::nullopt;
        auto it = ask_levels_.begin();
        return PriceLevel(it->first, it->second.total_quantity(),
                          it->second.order_count());
    }

    /**
     * @brief Convert to L2 book
     */
    void to_l2(L2OrderBook& l2) const {
        l2.clear();
        for (const auto& [price, level] : bid_levels_) {
            l2.update_level(Side::Buy, price, level.total_quantity());
        }
        for (const auto& [price, level] : ask_levels_) {
            l2.update_level(Side::Sell, price, level.total_quantity());
        }
    }

    const Symbol& symbol() const { return symbol_; }
    Timestamp last_update_time() const { return last_update_time_; }
    SequenceNum seq_num() const { return seq_num_; }

    size_t total_orders() const { return orders_.size(); }

    void clear() {
        bid_levels_.clear();
        ask_levels_.clear();
        orders_.clear();
    }

private:
    Symbol symbol_;
    std::map<Price, PriceLevelOrders> bid_levels_;
    std::map<Price, PriceLevelOrders> ask_levels_;
    std::unordered_map<OrderId, std::pair<Side, Price>> orders_;
    OrderId next_order_id_;
    Timestamp last_update_time_;
    SequenceNum seq_num_;
};

/**
 * @brief Order Book Manager for multiple symbols
 */
class OrderBookManager {
public:
    L2OrderBook& get_l2(const Symbol& symbol) {
        auto it = l2_books_.find(symbol);
        if (it == l2_books_.end()) {
            it = l2_books_.emplace(symbol, L2OrderBook(symbol)).first;
        }
        return it->second;
    }

    L3OrderBook& get_l3(const Symbol& symbol) {
        auto it = l3_books_.find(symbol);
        if (it == l3_books_.end()) {
            it = l3_books_.emplace(symbol, L3OrderBook(symbol)).first;
        }
        return it->second;
    }

    bool has_l2(const Symbol& symbol) const {
        return l2_books_.find(symbol) != l2_books_.end();
    }

    bool has_l3(const Symbol& symbol) const {
        return l3_books_.find(symbol) != l3_books_.end();
    }

    size_t symbol_count() const { return l2_books_.size(); }

private:
    std::unordered_map<Symbol, L2OrderBook, SymbolHash> l2_books_;
    std::unordered_map<Symbol, L3OrderBook, SymbolHash> l3_books_;
};

}  // namespace titans
