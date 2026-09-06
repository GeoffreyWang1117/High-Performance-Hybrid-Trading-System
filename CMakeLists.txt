cmake_minimum_required(VERSION 3.20)
project(Titans VERSION 1.0.0 LANGUAGES CXX)

# ============================================================================
# Titans: High-Performance Hybrid Trading System
# An Event-Driven C++ Framework Integrating Low-Latency Execution
# with GPU-Accelerated Signals and LLM Analytics
# ============================================================================

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG -fsanitize=address,undefined")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -flto")

string(TOUPPER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_UPPER)

# Options
option(TITANS_ENABLE_CUDA "Enable CUDA support" ON)
option(TITANS_ENABLE_TESTS "Enable unit tests" ON)
option(TITANS_ENABLE_BENCHMARKS "Enable benchmarks" ON)
option(TITANS_USE_IO_URING "Use io_uring instead of epoll" OFF)

# Find required packages
find_package(Threads REQUIRED)

# Optional CUDA support
if(TITANS_ENABLE_CUDA)
    include(CheckLanguage)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        set(CMAKE_CUDA_STANDARD 17)
        set(CMAKE_CUDA_STANDARD_REQUIRED ON)
        add_compile_definitions(TITANS_CUDA_ENABLED)
        message(STATUS "CUDA support enabled")
    else()
        message(WARNING "CUDA compiler not found, building without GPU support")
        set(TITANS_ENABLE_CUDA OFF)
    endif()
endif()

# io_uring support (Linux 5.1+)
if(TITANS_USE_IO_URING)
    find_library(LIBURING_LIB uring)
    if(LIBURING_LIB)
        add_compile_definitions(TITANS_USE_IO_URING)
        message(STATUS "io_uring support enabled")
    else()
        message(WARNING "liburing not found, falling back to epoll")
        set(TITANS_USE_IO_URING OFF)
    endif()
endif()

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# Core library
add_library(titans_core STATIC
    src/core/event_bus.cpp
    src/core/memory_pool.cpp
    src/core/spsc_queue.cpp
    src/core/event_loop.cpp
    src/core/timer.cpp
)
target_link_libraries(titans_core PUBLIC Threads::Threads)

# Trading engine library
add_library(titans_trading STATIC
    src/trading/order_book.cpp
    src/trading/matching_engine.cpp
    src/trading/order_manager.cpp
    src/trading/risk_manager.cpp
    src/trading/position_manager.cpp
)
target_link_libraries(titans_trading PUBLIC titans_core)

# Market data library
add_library(titans_market_data STATIC
    src/market_data/feed_handler.cpp
    src/market_data/websocket_client.cpp
    src/market_data/binary_logger.cpp
    src/market_data/replay_engine.cpp
)
target_link_libraries(titans_market_data PUBLIC titans_core)

# Strategy library
add_library(titans_strategy STATIC
    src/strategy/strategy_base.cpp
    src/strategy/signal_generator.cpp
    src/strategy/alpha_factors.cpp
)
target_link_libraries(titans_strategy PUBLIC titans_trading titans_market_data)

# CUDA signal processing library
if(TITANS_ENABLE_CUDA)
    add_library(titans_cuda STATIC
        src/cuda/cuda_utils.cu
        src/cuda/rolling_statistics.cu
        src/cuda/alpha_kernels.cu
        src/cuda/batch_inference.cu
    )
    set_target_properties(titans_cuda PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        CUDA_ARCHITECTURES "70;75;80;86;89"
    )
    target_link_libraries(titans_strategy PUBLIC titans_cuda)
endif()

# Main executable
add_executable(titans_engine
    src/main.cpp
)
target_link_libraries(titans_engine PRIVATE
    titans_core
    titans_trading
    titans_market_data
    titans_strategy
)

# Replay tool
add_executable(titans_replay
    src/tools/replay_main.cpp
)
target_link_libraries(titans_replay PRIVATE titans_market_data titans_trading)

# Dataset labelling and leakage audit
add_executable(titans_dataset
    src/tools/dataset_main.cpp
)
target_link_libraries(titans_dataset PRIVATE titans_core)

# End-to-end fast/slow lane demonstration over real market data
add_executable(titans_lanes
    src/tools/lanes_demo_main.cpp
)
target_link_libraries(titans_lanes PRIVATE titans_core Threads::Threads)

# Walk-forward: out-of-sample policy evaluation across many days
add_executable(titans_walkforward
    src/tools/walkforward_main.cpp
)
target_link_libraries(titans_walkforward PRIVATE titans_core)

# Tick-to-trade: per-stage and end-to-end latency over the real path
add_executable(titans_ticktotrade
    src/tools/pipeline_main.cpp
)
target_link_libraries(titans_ticktotrade PRIVATE titans_core titans_trading Threads::Threads)

# Benchmark executable
if(TITANS_ENABLE_BENCHMARKS)
    add_executable(titans_benchmark
        src/benchmark/benchmark_main.cpp
    )
    target_link_libraries(titans_benchmark PRIVATE titans_core titans_trading)
    # Record the exact flags used, so every results file states how it was built.
    target_compile_definitions(titans_benchmark PRIVATE
        TITANS_BUILD_FLAGS="${CMAKE_CXX_FLAGS_${CMAKE_BUILD_TYPE_UPPER}}")
endif()

# Research experiment executables
add_executable(titans_experiment
    examples/run_contamination_experiment.cpp
)
target_link_libraries(titans_experiment PRIVATE titans_core)

add_executable(titans_llm_experiment
    examples/run_llm_experiment.cpp
)
target_link_libraries(titans_llm_experiment PRIVATE titans_core)

add_executable(titans_distributed_experiment
    examples/run_distributed_experiment.cpp
)
target_link_libraries(titans_distributed_experiment PRIVATE titans_core Threads::Threads)

add_executable(titans_config_experiment
    examples/run_config_experiment.cpp
)
target_link_libraries(titans_config_experiment PRIVATE titans_core Threads::Threads)

# Tests
if(TITANS_ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Installation
install(TARGETS titans_engine titans_replay
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include
)

# Print configuration summary
message(STATUS "")
message(STATUS "=== Titans Configuration ===")
message(STATUS "Build type:      ${CMAKE_BUILD_TYPE}")
message(STATUS "CUDA support:    ${TITANS_ENABLE_CUDA}")
message(STATUS "io_uring:        ${TITANS_USE_IO_URING}")
message(STATUS "Tests:           ${TITANS_ENABLE_TESTS}")
message(STATUS "Benchmarks:      ${TITANS_ENABLE_BENCHMARKS}")
message(STATUS "============================")
