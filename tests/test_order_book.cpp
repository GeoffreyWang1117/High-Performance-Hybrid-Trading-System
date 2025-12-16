/**
 * @file test_order_book.cpp
 * @brief Order Book tests
 */

#include "titans/trading/order_book.hpp"
#include <iostream>

using namespace titans;

bool test_l2_book_updates() {
    L2OrderBook book(Symbol("BTCUSDT"));

    // Add bid levels
    book.update_level(Side::Buy, to_price(50000), to_quantity(1.0));
    book.update_level(Side::Buy, to_price(49900), to_quantity(2.0));

    // Add ask levels
    book.update_level(Side::Sell, to_price(50100), to_quantity(1.5));
    book.update_level(Side::Sell, to_price(50200), to_quantity(2.5));

    // Check best bid/ask
    auto best_bid = book.best_bid();
    auto best_ask = book.best_ask();

    if (!best_bid || best_bid->price != to_price(50000)) {
        std::cerr << "Best bid mismatch" << std::endl;
        return false;
    }

    if (!best_ask || best_ask->price != to_price(50100)) {
        std::cerr << "Best ask mismatch" << std::endl;
        return false;
    }

    return true;
}

bool test_l2_book_mid_spread() {
    L2OrderBook book(Symbol("BTCUSDT"));

    book.update_level(Side::Buy, to_price(100), to_quantity(1.0));
    book.update_level(Side::Sell, to_price(102), to_quantity(1.0));

    auto mid = book.mid_price();
    auto spread = book.spread();

    if (!mid || *mid != to_price(101)) {
        std::cerr << "Mid price mismatch" << std::endl;
        return false;
    }

    if (!spread || *spread != to_price(2)) {
        std::cerr << "Spread mismatch" << std::endl;
        return false;
    }

    return true;
}

bool test_l2_book_imbalance() {
    L2OrderBook book(Symbol("BTCUSDT"));

    // Heavy bid side
    book.update_level(Side::Buy, to_price(100), to_quantity(10.0));
    book.update_level(Side::Sell, to_price(101), to_quantity(2.0));

    double imb = book.imbalance(1);

    // (10 - 2) / (10 + 2) = 8/12 = 0.666...
    if (imb < 0.6 || imb > 0.7) {
        std::cerr << "Imbalance calculation incorrect: " << imb << std::endl;
        return false;
    }

    return true;
}

bool test_l3_book_add_cancel() {
    L3OrderBook book(Symbol("BTCUSDT"));

    // Add orders
    OrderId id1 = book.add_order(Side::Buy, to_price(50000), to_quantity(1.0));
    OrderId id2 = book.add_order(Side::Buy, to_price(50000), to_quantity(2.0));
    OrderId id3 = book.add_order(Side::Sell, to_price(50100), to_quantity(1.5));

    // Check best bid
    auto best_bid = book.best_bid();
    if (!best_bid || best_bid->quantity != to_quantity(3.0)) {
        std::cerr << "Best bid quantity should be 3.0" << std::endl;
        return false;
    }

    // Cancel one order
    book.cancel_order(id1);

    best_bid = book.best_bid();
    if (!best_bid || best_bid->quantity != to_quantity(2.0)) {
        std::cerr << "Best bid quantity should be 2.0 after cancel" << std::endl;
        return false;
    }

    return true;
}

bool test_l3_book_execute() {
    L3OrderBook book(Symbol("BTCUSDT"));

    // Add resting orders
    book.add_order(Side::Sell, to_price(50100), to_quantity(1.0));
    book.add_order(Side::Sell, to_price(50100), to_quantity(2.0));
    book.add_order(Side::Sell, to_price(50200), to_quantity(3.0));

    // Execute market buy
    auto fills = book.execute_market_order(Side::Buy, to_quantity(2.5));

    if (fills.size() != 2) {
        std::cerr << "Expected 2 fills, got " << fills.size() << std::endl;
        return false;
    }

    // Check remaining ask
    auto best_ask = book.best_ask();
    if (!best_ask || best_ask->quantity != to_quantity(0.5)) {
        std::cerr << "Remaining ask quantity incorrect" << std::endl;
        return false;
    }

    return true;
}

bool test_order_book_manager() {
    OrderBookManager manager;

    // Get books (creates if not exists)
    auto& btc_l2 = manager.get_l2(Symbol("BTCUSDT"));
    auto& eth_l2 = manager.get_l2(Symbol("ETHUSDT"));

    btc_l2.update_level(Side::Buy, to_price(50000), to_quantity(1.0));
    eth_l2.update_level(Side::Buy, to_price(2500), to_quantity(10.0));

    if (manager.symbol_count() != 2) {
        std::cerr << "Expected 2 symbols" << std::endl;
        return false;
    }

    return true;
}

bool run_order_book_tests() {
    std::cout << "  test_l2_book_updates... ";
    if (!test_l2_book_updates()) return false;
    std::cout << "OK\n";

    std::cout << "  test_l2_book_mid_spread... ";
    if (!test_l2_book_mid_spread()) return false;
    std::cout << "OK\n";

    std::cout << "  test_l2_book_imbalance... ";
    if (!test_l2_book_imbalance()) return false;
    std::cout << "OK\n";

    std::cout << "  test_l3_book_add_cancel... ";
    if (!test_l3_book_add_cancel()) return false;
    std::cout << "OK\n";

    std::cout << "  test_l3_book_execute... ";
    if (!test_l3_book_execute()) return false;
    std::cout << "OK\n";

    std::cout << "  test_order_book_manager... ";
    if (!test_order_book_manager()) return false;
    std::cout << "OK\n";

    return true;
}
