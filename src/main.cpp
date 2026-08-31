/**
 * @file main.cpp
 * @brief Titans Trading System Entry Point
 *
 * Main entry point for the Titans high-performance hybrid trading system.
 * Supports multiple modes:
 * - Live trading (shadow mode)
 * - Historical replay
 * - Backtesting
 */

#include "titans/core/types.hpp"
#include "titans/core/event_bus.hpp"
#include "titans/core/event_loop.hpp"
#include "titans/core/memory_pool.hpp"
#include "titans/trading/order_book.hpp"
#include "titans/trading/risk_manager.hpp"
#include "titans/trading/shadow_engine.hpp"
#include "titans/strategy/strategy_base.hpp"
#include "titans/market_data/binary_logger.hpp"
#include "titans/core/json.hpp"

#include <fstream>
#include <sstream>

#include <iostream>
#include <csignal>
#include <string>
#include <memory>
#include <thread>
#include <chrono>

using namespace titans;

// Global shutdown flag
std::atomic<bool> g_shutdown{false};

void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down...\n";
    g_shutdown = true;
}

void print_banner() {
    std::cout << R"(
 _____ _ _
|_   _(_) |_ __ _ _ __  ___
  | | | | __/ _` | '_ \/ __|
  | | | | || (_| | | | \__ \
  |_| |_|\__\__,_|_| |_|___/

High-Performance Hybrid Trading System
Version 1.0.0

)" << std::endl;
}

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [options]\n\n"
              << "Options:\n"
              << "  --mode <mode>       Operating mode: shadow, replay, backtest (default: shadow)\n"
              << "  --config <file>     Configuration file path\n"
              << "  --symbols <list>    Comma-separated list of symbols (e.g., BTCUSDT,ETHUSDT)\n"
              << "  --data <path>       Path to historical data for replay/backtest\n"
              << "  --log-path <path>   Path for log files (default: ./data/logs)\n"
              << "  --benchmark         Run performance benchmarks\n"
              << "  --help              Show this help message\n"
              << std::endl;
}

struct Config {
    std::string mode = "shadow";
    std::string config_file;
    std::vector<Symbol> symbols;
    std::string data_path;
    std::string log_path = "./data/logs";
    bool benchmark = false;

    // Risk and strategy parameters, defaulted here and overridable from
    // --config. These used to be hardcoded at the call site while
    // config/engine.yaml and config/strategy.yaml sat unread beside them --
    // the project has no YAML parser, so those files described behaviour that
    // did not exist.
    double max_position_size = 10.0;
    double max_order_size = 1.0;
    double max_daily_loss_usd = 1000.0;
    double max_drawdown_pct = 5.0;

    std::string strategy_name = "Momentum_20";
    int strategy_lookback = 20;
    double strategy_threshold = 0.02;
    double initial_capital = 100000.0;
    double max_position_pct = 0.1;
};

/**
 * @brief Overlay settings from a JSON config file onto @p config.
 *
 * Command-line flags are applied AFTER this, so an explicit flag always wins
 * over the file. Returns false and explains itself on any problem; the caller
 * treats that as fatal rather than proceeding with a config the operator
 * believes is in effect but is not.
 */
bool load_config_file(const std::string& path, Config& config) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "config: cannot open " << path << "\n";
        return false;
    }
    std::stringstream buf;
    buf << f.rdbuf();

    auto parsed = json::try_parse(buf.str());
    if (!parsed) {
        std::cerr << "config: " << path << " is not valid JSON\n";
        return false;
    }
    const json::Value& root = *parsed;
    if (!root.is_object()) {
        std::cerr << "config: " << path << " must contain a JSON object\n";
        return false;
    }

    if (root.contains("mode"))     config.mode = root["mode"].as_string(config.mode);
    if (root.contains("log_path")) config.log_path = root["log_path"].as_string(config.log_path);

    if (root.contains("symbols")) {
        const auto& arr = root["symbols"].as_array();
        if (!arr.empty()) {
            config.symbols.clear();
            for (const auto& v : arr) config.symbols.push_back(Symbol(v.as_string()));
        }
    }

    if (root.contains("risk")) {
        const json::Value& r = root["risk"];
        config.max_position_size  = r["max_position_size"].as_number(config.max_position_size);
        config.max_order_size     = r["max_order_size"].as_number(config.max_order_size);
        config.max_daily_loss_usd = r["max_daily_loss_usd"].as_number(config.max_daily_loss_usd);
        config.max_drawdown_pct   = r["max_drawdown_pct"].as_number(config.max_drawdown_pct);
    }

    if (root.contains("strategy")) {
        const json::Value& st = root["strategy"];
        config.strategy_name      = st["name"].as_string(config.strategy_name);
        config.strategy_lookback  = st["lookback"].as_int(config.strategy_lookback);
        config.strategy_threshold = st["threshold"].as_number(config.strategy_threshold);
        config.initial_capital    = st["initial_capital"].as_number(config.initial_capital);
        config.max_position_pct   = st["max_position_pct"].as_number(config.max_position_pct);
    }

    std::cout << "Loaded configuration from " << path << "\n";
    return true;
}

Config parse_args(int argc, char* argv[], bool* config_error = nullptr) {
    Config config;

    // Pre-scan for --config so the file is applied FIRST and command-line flags
    // can then override it. Processing them in argv order would make the result
    // depend on flag position, which is a surprise nobody wants from a config
    // file.
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--config") {
            config.config_file = argv[i + 1];
            if (!load_config_file(config.config_file, config)) {
                if (config_error) *config_error = true;
            }
            break;
        }
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            exit(0);
        } else if (arg == "--mode" && i + 1 < argc) {
            config.mode = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            config.config_file = argv[++i];
        } else if (arg == "--symbols" && i + 1 < argc) {
            std::string symbols_str = argv[++i];
            // Replace, do not append. A --config file may already have set
            // symbols, and an explicit flag has to win outright: appending
            // would leave the operator trading instruments they thought they
            // had deselected.
            config.symbols.clear();
            size_t pos = 0;
            while ((pos = symbols_str.find(',')) != std::string::npos) {
                config.symbols.push_back(Symbol(symbols_str.substr(0, pos)));
                symbols_str.erase(0, pos + 1);
            }
            if (!symbols_str.empty()) {
                config.symbols.push_back(Symbol(symbols_str));
            }
        } else if (arg == "--data" && i + 1 < argc) {
            config.data_path = argv[++i];
        } else if (arg == "--log-path" && i + 1 < argc) {
            config.log_path = argv[++i];
        } else if (arg == "--benchmark") {
            config.benchmark = true;
        }
    }

    // Default symbols if none specified
    if (config.symbols.empty()) {
        config.symbols.push_back(Symbol("BTCUSDT"));
    }

    return config;
}

void run_benchmark() {
    std::cout << "Running performance benchmarks...\n\n";

    // Benchmark SPSC Queue
    {
        std::cout << "=== SPSC Queue Benchmark ===\n";
        SPSCQueue<int64_t, 65536> queue;
        const int iterations = 10000000;

        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < iterations; ++i) {
            queue.try_push(i);
        }
        auto mid = std::chrono::high_resolution_clock::now();

        int64_t val;
        int count = 0;
        while (queue.try_pop(val)) {
            ++count;
        }
        auto end = std::chrono::high_resolution_clock::now();

        auto push_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(mid - start).count();
        auto pop_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - mid).count();

        std::cout << "Push: " << iterations << " ops in " << push_ns / 1e6 << " ms\n";
        std::cout << "      " << push_ns / iterations << " ns/op\n";
        std::cout << "Pop:  " << count << " ops in " << pop_ns / 1e6 << " ms\n";
        std::cout << "      " << pop_ns / count << " ns/op\n\n";
    }

    // Benchmark Order Book
    {
        std::cout << "=== Order Book Benchmark ===\n";
        L3OrderBook book(Symbol("BTCUSDT"));
        const int iterations = 100000;

        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < iterations; ++i) {
            book.add_order(Side::Buy, to_price(50000.0 - (i % 100)), to_quantity(1.0));
            book.add_order(Side::Sell, to_price(50100.0 + (i % 100)), to_quantity(1.0));
        }
        auto end = std::chrono::high_resolution_clock::now();

        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        std::cout << "Add orders: " << iterations * 2 << " ops in " << ns / 1e6 << " ms\n";
        std::cout << "            " << ns / (iterations * 2) << " ns/op\n\n";
    }

    // Benchmark Event Bus
    {
        std::cout << "=== Event Bus Benchmark ===\n";
        EventBus bus;
        int received = 0;

        bus.subscribe<MarketDataEvent>(EventType::MarketDataSnapshot,
            [&received](const MarketDataEvent&) {
                ++received;
            });

        const int iterations = 1000000;
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; ++i) {
            MarketDataEvent event;
            bus.publish(event);
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

        std::cout << "Publish: " << iterations << " events in " << ns / 1e6 << " ms\n";
        std::cout << "         " << ns / iterations << " ns/event\n";
        std::cout << "         Received: " << received << "\n\n";
    }

    // Benchmark Memory Pool
    {
        std::cout << "=== Memory Pool Benchmark ===\n";
        ObjectPool<Order, 1024> pool;
        const int iterations = 100000;
        std::vector<Order*> orders;
        orders.reserve(iterations);

        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < iterations; ++i) {
            Order* order = pool.allocate();
            orders.push_back(order);
        }
        auto mid = std::chrono::high_resolution_clock::now();

        for (Order* order : orders) {
            pool.deallocate(order);
        }
        auto end = std::chrono::high_resolution_clock::now();

        auto alloc_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(mid - start).count();
        auto dealloc_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - mid).count();

        std::cout << "Allocate:   " << iterations << " ops in " << alloc_ns / 1e6 << " ms\n";
        std::cout << "            " << alloc_ns / iterations << " ns/op\n";
        std::cout << "Deallocate: " << iterations << " ops in " << dealloc_ns / 1e6 << " ms\n";
        std::cout << "            " << dealloc_ns / iterations << " ns/op\n\n";
    }

    std::cout << "Benchmarks complete.\n";
}

void run_shadow_mode(const Config& config) {
    std::cout << "Starting shadow trading mode...\n";
    std::cout << "Symbols: ";
    for (const auto& sym : config.symbols) {
        std::cout << sym.view() << " ";
    }
    std::cout << "\n\n";

    // Create shadow engine config
    ShadowConfig shadow_config;
    shadow_config.mode = ShadowMode::Live;
    shadow_config.symbols = config.symbols;
    shadow_config.log_path = config.log_path;
    shadow_config.log_trades = true;
    shadow_config.log_book_updates = true;

    // Create shadow engine
    ShadowEngine engine(shadow_config);

    // Create event bus and risk manager
    EventBus& bus = engine.bus();

    // Create and add strategies
    RiskLimits risk_limits;
    risk_limits.max_position_size = to_quantity(config.max_position_size);
    risk_limits.max_order_size = to_quantity(config.max_order_size);
    risk_limits.max_daily_loss_usd = config.max_daily_loss_usd;
    risk_limits.max_drawdown_pct = config.max_drawdown_pct;

    auto risk_manager = std::make_unique<RiskManager>(bus, risk_limits);

    StrategyConfig strategy_config;
    strategy_config.name = config.strategy_name;
    strategy_config.id = 1;
    strategy_config.symbols = config.symbols;
    strategy_config.initial_capital = config.initial_capital;
    strategy_config.max_position_pct = config.max_position_pct;

    auto strategy = std::make_unique<MomentumStrategy>(
        bus, *risk_manager, strategy_config,
        config.strategy_lookback, config.strategy_threshold);

    engine.add_strategy(std::move(strategy), std::move(risk_manager),
                        config.strategy_name, true);

    // Start the engine
    engine.start();

    std::cout << "Shadow trading started. Press Ctrl+C to stop.\n";

    // Simulate market data for demo
    uint64_t seq = 0;
    double price = 50000.0;

    while (!g_shutdown) {
        // Generate simulated market data
        MarketDataEvent event;
        event.symbol = config.symbols[0];
        event.seq_num = ++seq;
        event.timestamp = now_ns();

        // Random walk price
        price += (rand() % 100 - 50) * 0.1;

        event.last_price = to_price(price);
        event.last_size = to_quantity(0.1 + (rand() % 100) * 0.01);

        // Generate book
        event.bid_levels = 5;
        event.ask_levels = 5;
        for (int i = 0; i < 5; ++i) {
            event.bid_prices[i] = to_price(price - (i + 1) * 0.1);
            event.bid_sizes[i] = to_quantity(1.0 + rand() % 10);
            event.ask_prices[i] = to_price(price + (i + 1) * 0.1);
            event.ask_sizes[i] = to_quantity(1.0 + rand() % 10);
        }

        // Publish event
        bus.publish(event);

        // Run event loop
        engine.loop().run_once(10);

        // Rate limit
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Stop and print stats
    engine.stop();
    engine.stats().print();
}

void run_replay_mode(const Config& config) {
    std::cout << "Starting replay mode...\n";
    std::cout << "Data path: " << config.data_path << "\n\n";

    if (config.data_path.empty()) {
        std::cerr << "Error: --data path required for replay mode\n";
        return;
    }

    // Open replay engine
    ReplayEngine replay;
    if (!replay.open(config.data_path)) {
        std::cerr << "Error: Failed to open data file: " << config.data_path << "\n";
        return;
    }

    std::cout << "Loaded " << replay.num_records() << " records\n";

    // Create trading components
    EventBus bus;
    RiskLimits risk_limits;
    RiskManager risk_manager(bus, risk_limits);

    StrategyConfig strategy_config;
    strategy_config.name = "Momentum_20";
    strategy_config.id = 1;
    strategy_config.symbols = config.symbols;

    MomentumStrategy strategy(bus, risk_manager, strategy_config, 20, 0.02);
    strategy.initialize();
    strategy.start();

    // Create visitor to process records
    class ReplayVisitor : public IRecordVisitor {
    public:
        ReplayVisitor(EventBus& bus, IStrategy& strategy)
            : bus_(bus), strategy_(strategy) {}

        void on_book_snapshot(const BookSnapshotRecord& record,
                             const PriceLevel* bids,
                             const PriceLevel* asks) override {
            MarketDataEvent event;
            event.symbol = record.symbol;
            event.seq_num = record.seq_num;
            event.timestamp = record.header.timestamp;

            event.bid_levels = record.bid_levels;
            event.ask_levels = record.ask_levels;

            for (int i = 0; i < record.bid_levels && i < MAX_BOOK_LEVELS; ++i) {
                event.bid_prices[i] = bids[i].price;
                event.bid_sizes[i] = bids[i].quantity;
            }
            for (int i = 0; i < record.ask_levels && i < MAX_BOOK_LEVELS; ++i) {
                event.ask_prices[i] = asks[i].price;
                event.ask_sizes[i] = asks[i].quantity;
            }

            strategy_.on_market_data(event);
            ++events_processed_;
        }

        void on_book_update(const BookUpdateRecord& record) override {
            ++events_processed_;
        }

        void on_trade(const TradeRecord& record) override {
            TradeEvent event;
            event.symbol = record.symbol;
            event.trade_id = record.trade_id;
            event.price = record.price;
            event.quantity = record.quantity;
            event.aggressor_side = record.aggressor_side;
            event.timestamp = record.header.timestamp;

            strategy_.on_trade(event);
            ++events_processed_;
        }

        void on_quote(const QuoteRecord& record) override {
            ++events_processed_;
        }

        uint64_t events_processed_ = 0;

    private:
        EventBus& bus_;
        IStrategy& strategy_;
    };

    ReplayVisitor visitor(bus, strategy);

    // Replay
    auto start = std::chrono::high_resolution_clock::now();
    replay.replay(visitor);
    auto end = std::chrono::high_resolution_clock::now();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    strategy.stop();

    std::cout << "\nReplay complete.\n";
    std::cout << "Events processed: " << visitor.events_processed_ << "\n";
    std::cout << "Time: " << ms << " ms\n";
    std::cout << "Throughput: " << (visitor.events_processed_ * 1000 / std::max(1L, ms))
              << " events/sec\n";

    // Print strategy metrics
    const auto& metrics = strategy.metrics();
    std::cout << "\nStrategy Performance:\n";
    std::cout << "  Signals: " << metrics.num_signals << "\n";
    std::cout << "  Orders: " << metrics.num_orders << "\n";
    std::cout << "  Fills: " << metrics.num_fills << "\n";
    std::cout << "  PnL: $" << metrics.total_pnl << "\n";
}

int main(int argc, char* argv[]) {
    // Set up signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Print banner
    print_banner();

    // Parse command line arguments
    bool config_error = false;
    Config config = parse_args(argc, argv, &config_error);
    if (config_error) {
        // Refuse to start on a config the operator believes is in effect but
        // is not. Silently falling back to defaults is how a risk limit ends
        // up ten times larger than anyone intended.
        std::cerr << "Refusing to start with an unusable --config file.\n";
        return 2;
    }

    // Run requested mode
    if (config.benchmark) {
        run_benchmark();
    } else if (config.mode == "shadow") {
        run_shadow_mode(config);
    } else if (config.mode == "replay") {
        run_replay_mode(config);
    } else if (config.mode == "backtest") {
        std::cout << "Backtest mode not yet implemented.\n";
    } else {
        std::cerr << "Unknown mode: " << config.mode << "\n";
        print_usage(argv[0]);
        return 1;
    }

    std::cout << "\nTitans shutdown complete.\n";
    return 0;
}
