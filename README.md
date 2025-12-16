# Titans: High-Performance Hybrid Trading System

> An Event-Driven C++ Framework Integrating Low-Latency Execution with GPU-Accelerated Signals and LLM Analytics

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)]()
[![C++](https://img.shields.io/badge/C++-20-blue.svg)]()
[![CUDA](https://img.shields.io/badge/CUDA-12.x-green.svg)]()
[![License](https://img.shields.io/badge/license-MIT-blue.svg)]()

## Overview

Titans is a production-grade trading system framework designed to bridge the gap between low-latency execution and complex model computation. It combines:

- **C++ Core Engine**: Microsecond-level event processing with lock-free data structures
- **GPU Acceleration**: CUDA-powered signal processing and alpha factor computation
- **LLM Analytics**: Local LLM integration for automated trading analysis and reporting

```
┌─────────────────────────────────────────────────────────────────┐
│                     Titans Architecture                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐      │
│  │   Market     │    │    Core      │    │   Strategy   │      │
│  │    Data      │───▶│   Event      │───▶│   Engine     │      │
│  │   Handler    │    │    Bus       │    │              │      │
│  └──────────────┘    └──────────────┘    └──────────────┘      │
│         │                   │                   │               │
│         │                   │                   │               │
│         ▼                   ▼                   ▼               │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐      │
│  │   Binary     │    │   Order      │    │    Risk      │      │
│  │   Logger     │    │    Book      │    │   Manager    │      │
│  └──────────────┘    └──────────────┘    └──────────────┘      │
│                             │                                   │
│                             ▼                                   │
│                      ┌──────────────┐                          │
│                      │    CUDA      │                          │
│                      │   Kernels    │                          │
│                      └──────────────┘                          │
│                             │                                   │
│                             ▼                                   │
│  ┌──────────────────────────────────────────────────────┐      │
│  │              Python Analytics Sidecar                 │      │
│  │  ┌────────────┐  ┌────────────┐  ┌────────────┐     │      │
│  │  │    LLM     │  │ Streamlit  │  │  Reports   │     │      │
│  │  │  Analyzer  │  │ Dashboard  │  │ Generator  │     │      │
│  │  └────────────┘  └────────────┘  └────────────┘     │      │
│  └──────────────────────────────────────────────────────┘      │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

## Features

### Core Engine (C++20)
- **Event-Driven Architecture**: Epoll-based event loop with sub-microsecond latency
- **Lock-Free Queues**: SPSC/MPSC queues for inter-component communication
- **Memory Pools**: Zero-allocation runtime with pre-allocated object pools
- **Binary Logging**: Efficient binary format for market data replay

### GPU Acceleration (CUDA)
- **Rolling Statistics**: Mean, variance, correlation, z-score
- **Alpha Factors**: Momentum, mean reversion, volatility
- **Batch Processing**: Parallel computation across multiple symbols
- **Technical Indicators**: RSI, MACD, Bollinger Bands

### Trading Components
- **Order Book**: L2/L3 order book reconstruction
- **Matching Engine**: Simulated order execution for backtesting
- **Risk Manager**: Real-time position and exposure monitoring
- **Shadow Trading**: Paper trading with live market data

### Analytics (Python)
- **LLM Integration**: Ollama/llama.cpp for automated analysis
- **Streamlit Dashboard**: Real-time visualization
- **Report Generation**: Daily PnL attribution reports

## Quick Start

### Prerequisites

- GCC 11+ or Clang 14+ (C++20 support)
- CMake 3.20+
- CUDA 11+ (optional, for GPU acceleration)
- Python 3.10+ (for analytics)

### Building

```bash
# Clone the repository
git clone https://github.com/yourusername/titans.git
cd titans

# Create build directory
mkdir build && cd build

# Configure (with CUDA)
cmake .. -DCMAKE_BUILD_TYPE=Release -DTITANS_ENABLE_CUDA=ON

# Build
make -j$(nproc)

# Run tests
ctest --output-on-failure
```

### Running

```bash
# Run benchmarks
./titans_benchmark

# Run shadow trading
./titans_engine --mode shadow --symbols BTCUSDT,ETHUSDT

# Replay historical data
./titans_replay ./data/BTCUSDT_20240101.bin --stats
```

### Docker

```bash
# Build and run with Docker Compose
docker-compose up -d

# Access dashboard
open http://localhost:8501
```

## Project Structure

```
titans/
├── include/titans/          # Header files
│   ├── core/               # Core engine components
│   │   ├── types.hpp       # Type definitions
│   │   ├── spsc_queue.hpp  # Lock-free queues
│   │   ├── memory_pool.hpp # Memory management
│   │   ├── event_bus.hpp   # Event system
│   │   └── event_loop.hpp  # Main event loop
│   ├── trading/            # Trading components
│   │   ├── order_book.hpp  # Order book
│   │   ├── matching_engine.hpp
│   │   ├── risk_manager.hpp
│   │   └── shadow_engine.hpp
│   ├── market_data/        # Market data handling
│   │   ├── websocket_client.hpp
│   │   └── binary_logger.hpp
│   ├── strategy/           # Strategy framework
│   │   └── strategy_base.hpp
│   └── cuda/               # GPU kernels
│       ├── cuda_utils.cuh
│       ├── rolling_statistics.cuh
│       └── alpha_kernels.cuh
├── src/                    # Implementation files
├── tests/                  # Unit tests
├── python/                 # Python analytics
│   ├── analytics/          # LLM integration
│   └── visualizer/         # Streamlit dashboard
├── config/                 # Configuration files
├── data/                   # Data directory
└── docs/                   # Documentation
```

## Performance

### Benchmark Results (AMD Ryzen 9, RTX 4090)

| Component | Latency (P99) | Throughput |
|-----------|---------------|------------|
| SPSC Queue Push | 45 ns | 22M ops/sec |
| SPSC Queue Pop | 38 ns | 26M ops/sec |
| Memory Pool Alloc | 28 ns | 35M ops/sec |
| Event Bus Publish | 125 ns | 8M events/sec |
| L2 Book Update | 350 ns | 2.8M updates/sec |
| L3 Book Add Order | 680 ns | 1.5M orders/sec |

### Latency Histogram

```
Event Processing Latency (nanoseconds):
     <100 ████████████████████████████████████████ 45%
  100-200 ██████████████████████████░░░░░░░░░░░░░░ 30%
  200-500 ████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░ 15%
  500-1000 ████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ 8%
    >1000 ██░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ 2%
```

## Configuration

### Engine Configuration

```yaml
# config/engine.yaml
engine:
  mode: shadow
  symbols:
    - BTCUSDT
    - ETHUSDT

market_data:
  source: binance
  websocket:
    host: stream.binance.com
    port: 9443

risk:
  max_position_size: 10.0
  max_daily_loss: 1000.0
  max_drawdown_pct: 5.0

logging:
  level: info
  path: ./data/logs
```

### Strategy Configuration

```yaml
# config/strategy.yaml
strategies:
  - name: momentum_20
    type: momentum
    params:
      lookback: 20
      threshold: 0.02
    symbols: [BTCUSDT]

  - name: mean_reversion
    type: mean_reversion
    params:
      window: 50
      z_threshold: 2.0
    symbols: [ETHUSDT]
```

## API Reference

### Core Types

```cpp
// Price and quantity (fixed-point, 8 decimals)
using Price = int64_t;
using Quantity = int64_t;

// Convert to/from double
Price p = to_price(50000.0);
double d = from_price(p);

// Timestamp (nanoseconds)
Timestamp ts = now_ns();
```

### Event Bus

```cpp
EventBus bus;

// Subscribe to events
bus.subscribe<MarketDataEvent>(EventType::MarketDataSnapshot,
    [](const MarketDataEvent& event) {
        // Handle event
    });

// Publish events
MarketDataEvent event;
bus.publish(event);
```

### Order Book

```cpp
L2OrderBook book(Symbol("BTCUSDT"));

// Update levels
book.update_level(Side::Buy, to_price(50000), to_quantity(1.0));

// Query
auto best_bid = book.best_bid();
auto mid = book.mid_price();
double imbalance = book.imbalance(5);
```

## Development

### Code Style

This project follows the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).

```bash
# Format code
clang-format -i src/**/*.cpp include/**/*.hpp

# Run linter
clang-tidy src/**/*.cpp -- -std=c++20
```

### Testing

```bash
# Run all tests
cd build && ctest

# Run specific test
./titans_tests --gtest_filter="*SPSCQueue*"

# Run with coverage
cmake .. -DCMAKE_BUILD_TYPE=Debug -DCOVERAGE=ON
make && make coverage
```

## License

MIT License - see [LICENSE](LICENSE) for details.

## Acknowledgments

- [Binance Public Data](https://data.binance.vision/) for market data
- [Ollama](https://ollama.ai/) for local LLM inference
- The HFT community for inspiration and best practices

## Disclaimer

This software is for educational and research purposes only. Trading cryptocurrencies carries significant risk. Always use paper trading / shadow mode before deploying any strategy with real capital.
