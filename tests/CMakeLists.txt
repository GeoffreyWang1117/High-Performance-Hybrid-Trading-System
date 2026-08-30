# Test configuration
enable_testing()

# Find GTest (optional)
find_package(GTest QUIET)

if(GTest_FOUND)
    message(STATUS "Google Test found, building tests with GTest")

    add_executable(titans_tests
        test_main.cpp
        test_spsc_queue.cpp
        test_memory_pool.cpp
        test_event_bus.cpp
        test_order_book.cpp
        test_versioned_entity.cpp
        test_contamination.cpp
        test_json.cpp
        test_lane_isolation.cpp
        test_task_design.cpp
        test_statistics.cpp
        test_binance_dataset.cpp
        test_context_contamination.cpp
    )

    target_link_libraries(titans_tests
        PRIVATE
        GTest::gtest
        GTest::gtest_main
        titans_core
        titans_trading
    )

    include(GoogleTest)
    gtest_discover_tests(titans_tests)

else()
    message(STATUS "Google Test not found, building basic tests")

    add_executable(titans_tests
        test_main.cpp
        test_spsc_queue.cpp
        test_memory_pool.cpp
        test_event_bus.cpp
        test_order_book.cpp
        test_versioned_entity.cpp
        test_contamination.cpp
        test_json.cpp
        test_lane_isolation.cpp
        test_task_design.cpp
        test_statistics.cpp
        test_binance_dataset.cpp
        test_context_contamination.cpp
    )

    target_link_libraries(titans_tests
        PRIVATE
        titans_core
        titans_trading
    )

    add_test(NAME titans_tests COMMAND titans_tests)
endif()
