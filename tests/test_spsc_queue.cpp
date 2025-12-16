/**
 * @file test_spsc_queue.cpp
 * @brief SPSC Queue tests
 */

#include "titans/core/spsc_queue.hpp"
#include <iostream>
#include <thread>
#include <atomic>

using namespace titans;

bool test_basic_push_pop() {
    SPSCQueue<int, 16> queue;

    // Push some items
    for (int i = 0; i < 10; ++i) {
        if (!queue.try_push(i)) {
            std::cerr << "Push failed at " << i << std::endl;
            return false;
        }
    }

    // Pop and verify
    for (int i = 0; i < 10; ++i) {
        int val;
        if (!queue.try_pop(val)) {
            std::cerr << "Pop failed at " << i << std::endl;
            return false;
        }
        if (val != i) {
            std::cerr << "Value mismatch: expected " << i << ", got " << val << std::endl;
            return false;
        }
    }

    return true;
}

bool test_empty_pop() {
    SPSCQueue<int, 16> queue;

    int val;
    if (queue.try_pop(val)) {
        std::cerr << "Pop should fail on empty queue" << std::endl;
        return false;
    }

    return true;
}

bool test_full_queue() {
    SPSCQueue<int, 4> queue;  // Capacity 4

    // Fill queue
    for (int i = 0; i < 4; ++i) {
        if (!queue.try_push(i)) {
            std::cerr << "Should be able to push " << i << std::endl;
            return false;
        }
    }

    // Should fail to push when full
    if (queue.try_push(999)) {
        std::cerr << "Push should fail on full queue" << std::endl;
        return false;
    }

    return true;
}

bool test_concurrent_access() {
    SPSCQueue<int64_t, 1024> queue;
    std::atomic<bool> done{false};
    std::atomic<int64_t> sum_pushed{0};
    std::atomic<int64_t> sum_popped{0};
    const int count = 100000;

    // Producer thread
    std::thread producer([&]() {
        for (int64_t i = 0; i < count; ++i) {
            while (!queue.try_push(i)) {
                std::this_thread::yield();
            }
            sum_pushed += i;
        }
        done = true;
    });

    // Consumer thread
    std::thread consumer([&]() {
        int64_t val;
        while (!done || !queue.empty()) {
            if (queue.try_pop(val)) {
                sum_popped += val;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    if (sum_pushed != sum_popped) {
        std::cerr << "Sum mismatch: pushed=" << sum_pushed
                  << ", popped=" << sum_popped << std::endl;
        return false;
    }

    return true;
}

bool run_spsc_queue_tests() {
    std::cout << "  test_basic_push_pop... ";
    if (!test_basic_push_pop()) return false;
    std::cout << "OK\n";

    std::cout << "  test_empty_pop... ";
    if (!test_empty_pop()) return false;
    std::cout << "OK\n";

    std::cout << "  test_full_queue... ";
    if (!test_full_queue()) return false;
    std::cout << "OK\n";

    std::cout << "  test_concurrent_access... ";
    if (!test_concurrent_access()) return false;
    std::cout << "OK\n";

    return true;
}
