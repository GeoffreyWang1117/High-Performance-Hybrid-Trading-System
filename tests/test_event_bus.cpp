/**
 * @file test_event_bus.cpp
 * @brief Event Bus tests
 */

#include "titans/core/event_bus.hpp"
#include <iostream>

using namespace titans;

bool test_subscribe_publish() {
    EventBus bus;
    int received = 0;

    bus.subscribe<MarketDataEvent>(EventType::MarketDataSnapshot,
        [&received](const MarketDataEvent& event) {
            ++received;
        });

    MarketDataEvent event;
    bus.publish(event);
    bus.publish(event);
    bus.publish(event);

    if (received != 3) {
        std::cerr << "Expected 3 events, got " << received << std::endl;
        return false;
    }

    return true;
}

bool test_multiple_handlers() {
    EventBus bus;
    int handler1_count = 0;
    int handler2_count = 0;

    bus.subscribe<MarketDataEvent>(EventType::MarketDataSnapshot,
        [&handler1_count](const MarketDataEvent&) { ++handler1_count; });

    bus.subscribe<MarketDataEvent>(EventType::MarketDataSnapshot,
        [&handler2_count](const MarketDataEvent&) { ++handler2_count; });

    MarketDataEvent event;
    bus.publish(event);

    if (handler1_count != 1 || handler2_count != 1) {
        std::cerr << "Both handlers should receive the event" << std::endl;
        return false;
    }

    return true;
}

bool test_different_event_types() {
    EventBus bus;
    int market_data_count = 0;
    int trade_count = 0;

    bus.subscribe<MarketDataEvent>(EventType::MarketDataSnapshot,
        [&market_data_count](const MarketDataEvent&) { ++market_data_count; });

    bus.subscribe<TradeEvent>(EventType::TradeEvent,
        [&trade_count](const TradeEvent&) { ++trade_count; });

    MarketDataEvent md_event;
    TradeEvent trade_event;

    bus.publish(md_event);
    bus.publish(trade_event);
    bus.publish(trade_event);

    if (market_data_count != 1 || trade_count != 2) {
        std::cerr << "Event type routing failed" << std::endl;
        return false;
    }

    return true;
}

bool test_event_sequence_numbers() {
    EventBus bus;
    SequenceNum last_seq = 0;

    bus.subscribe<MarketDataEvent>(EventType::MarketDataSnapshot,
        [&last_seq](const MarketDataEvent& event) {
            last_seq = event.seq_num;
        });

    MarketDataEvent event;
    for (int i = 0; i < 100; ++i) {
        bus.publish(event);
    }

    if (last_seq != 100) {
        std::cerr << "Expected seq 100, got " << last_seq << std::endl;
        return false;
    }

    return true;
}

bool test_queued_events() {
    EventBus bus;
    int received = 0;

    bus.subscribe<MarketDataEvent>(EventType::MarketDataSnapshot,
        [&received](const MarketDataEvent&) { ++received; });

    // Queue events
    for (int i = 0; i < 10; ++i) {
        MarketDataEvent event;
        bus.queue(event);
    }

    // Should not have processed yet
    if (received != 0) {
        std::cerr << "Events should not be processed until process_queued" << std::endl;
        return false;
    }

    // Process queued events
    size_t processed = bus.process_queued();

    if (processed != 10 || received != 10) {
        std::cerr << "Expected 10 processed events" << std::endl;
        return false;
    }

    return true;
}

bool run_event_bus_tests() {
    std::cout << "  test_subscribe_publish... ";
    if (!test_subscribe_publish()) return false;
    std::cout << "OK\n";

    std::cout << "  test_multiple_handlers... ";
    if (!test_multiple_handlers()) return false;
    std::cout << "OK\n";

    std::cout << "  test_different_event_types... ";
    if (!test_different_event_types()) return false;
    std::cout << "OK\n";

    std::cout << "  test_event_sequence_numbers... ";
    if (!test_event_sequence_numbers()) return false;
    std::cout << "OK\n";

    std::cout << "  test_queued_events... ";
    if (!test_queued_events()) return false;
    std::cout << "OK\n";

    return true;
}
