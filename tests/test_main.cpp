/**
 * @file test_main.cpp
 * @brief Test suite entry point
 */

#include <iostream>
#include <cassert>
#include <functional>
#include <vector>
#include <string>

// Simple test framework (if GTest not available)
struct TestCase {
    std::string name;
    std::function<bool()> test;
};

std::vector<TestCase> g_tests;

#define TEST(name) \
    bool test_##name(); \
    static bool _reg_##name = (g_tests.push_back({#name, test_##name}), true); \
    bool test_##name()

#define ASSERT_TRUE(x) do { if (!(x)) { std::cerr << "FAIL: " << #x << std::endl; return false; } } while(0)
#define ASSERT_FALSE(x) ASSERT_TRUE(!(x))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b) ASSERT_TRUE((a) != (b))
#define ASSERT_LT(a, b) ASSERT_TRUE((a) < (b))
#define ASSERT_GT(a, b) ASSERT_TRUE((a) > (b))

// External test functions
extern bool run_spsc_queue_tests();
extern bool run_memory_pool_tests();
extern bool run_event_bus_tests();
extern bool run_order_book_tests();
extern bool run_versioned_entity_tests();
extern bool run_contamination_tests();
extern bool run_json_tests();
extern bool run_lane_isolation_tests();
extern bool run_task_design_tests();
extern bool run_statistics_tests();
extern bool run_binance_dataset_tests();
extern bool run_context_contamination_tests();

int main(int argc, char* argv[]) {
    std::cout << R"(
 _____ _ _                  _____         _
|_   _(_) |_ __ _ _ __  ___|_   _|__  ___| |_ ___
  | | | | __/ _` | '_ \/ __| | |/ _ \/ __| __/ __|
  | | | | || (_| | | | \__ \ | |  __/\__ \ |_\__ \
  |_| |_|\__\__,_|_| |_|___/ |_|\___||___/\__|___/

)";

    int passed = 0;
    int failed = 0;

    // Run module tests
    std::vector<std::pair<std::string, std::function<bool()>>> modules = {
        {"SPSC Queue", run_spsc_queue_tests},
        {"Memory Pool", run_memory_pool_tests},
        {"Event Bus", run_event_bus_tests},
        {"Order Book", run_order_book_tests},
        {"Versioned Entity", run_versioned_entity_tests},
        {"Contamination", run_contamination_tests},
        {"JSON", run_json_tests},
        {"Lane Isolation", run_lane_isolation_tests},
        {"Task Design", run_task_design_tests},
        {"Statistics", run_statistics_tests},
        {"Binance Dataset", run_binance_dataset_tests},
        {"Context Contamination", run_context_contamination_tests}
    };

    for (const auto& [name, test_func] : modules) {
        std::cout << "\n=== Testing " << name << " ===\n";
        if (test_func()) {
            std::cout << "✓ " << name << " tests passed\n";
            ++passed;
        } else {
            std::cout << "✗ " << name << " tests FAILED\n";
            ++failed;
        }
    }

    // Summary
    std::cout << "\n=== Test Summary ===\n";
    std::cout << "Passed: " << passed << "\n";
    std::cout << "Failed: " << failed << "\n";

    return failed > 0 ? 1 : 0;
}
