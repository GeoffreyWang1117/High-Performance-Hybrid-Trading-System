/**
 * @file test_memory_pool.cpp
 * @brief Memory Pool tests
 */

#include "titans/core/memory_pool.hpp"
#include "titans/core/types.hpp"
#include <iostream>
#include <vector>

using namespace titans;

bool test_object_pool_basic() {
    ObjectPool<Order, 64> pool;

    // Allocate some objects
    std::vector<Order*> orders;
    for (int i = 0; i < 10; ++i) {
        Order* order = pool.allocate();
        if (!order) {
            std::cerr << "Allocation failed at " << i << std::endl;
            return false;
        }
        order->id = i;
        orders.push_back(order);
    }

    // Verify and deallocate
    for (int i = 0; i < 10; ++i) {
        if (orders[i]->id != static_cast<OrderId>(i)) {
            std::cerr << "Order ID mismatch" << std::endl;
            return false;
        }
        pool.deallocate(orders[i]);
    }

    return true;
}

bool test_object_pool_reuse() {
    ObjectPool<int, 4> pool;

    // Allocate
    int* a = pool.allocate();
    int* b = pool.allocate();
    *a = 42;
    *b = 100;

    // Deallocate
    pool.deallocate(a);
    pool.deallocate(b);

    // Reallocate - should reuse
    int* c = pool.allocate();
    int* d = pool.allocate();

    if (c != b && c != a) {
        std::cerr << "Expected memory reuse" << std::endl;
        return false;
    }

    pool.deallocate(c);
    pool.deallocate(d);

    return true;
}

bool test_arena_allocator() {
    Arena arena(4096);

    // Allocate various sizes
    int* a = arena.create<int>(42);
    double* b = arena.create<double>(3.14);
    char* str = arena.allocate_array<char>(100);

    if (*a != 42) {
        std::cerr << "Int value mismatch" << std::endl;
        return false;
    }

    if (*b != 3.14) {
        std::cerr << "Double value mismatch" << std::endl;
        return false;
    }

    // Reset and reuse
    arena.reset();

    int* c = arena.create<int>(100);
    if (*c != 100) {
        std::cerr << "Post-reset value mismatch" << std::endl;
        return false;
    }

    return true;
}

bool test_ring_buffer() {
    RingBuffer<int, 8> buffer;

    // Push items
    for (int i = 0; i < 7; ++i) {
        if (!buffer.push(i)) {
            std::cerr << "Push failed at " << i << std::endl;
            return false;
        }
    }

    // Should fail when full
    if (buffer.push(999)) {
        std::cerr << "Push should fail on full buffer" << std::endl;
        return false;
    }

    // Pop and verify
    for (int i = 0; i < 7; ++i) {
        int val;
        if (!buffer.pop(val) || val != i) {
            std::cerr << "Pop failed or value mismatch at " << i << std::endl;
            return false;
        }
    }

    return true;
}

bool run_memory_pool_tests() {
    std::cout << "  test_object_pool_basic... ";
    if (!test_object_pool_basic()) return false;
    std::cout << "OK\n";

    std::cout << "  test_object_pool_reuse... ";
    if (!test_object_pool_reuse()) return false;
    std::cout << "OK\n";

    std::cout << "  test_arena_allocator... ";
    if (!test_arena_allocator()) return false;
    std::cout << "OK\n";

    std::cout << "  test_ring_buffer... ";
    if (!test_ring_buffer()) return false;
    std::cout << "OK\n";

    return true;
}
