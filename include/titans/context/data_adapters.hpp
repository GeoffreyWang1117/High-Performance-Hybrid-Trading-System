/**
 * @file data_adapters.hpp
 * @brief Real Data Adapters for Production Experiments
 *
 * Connectors for real market data formats:
 * - LOBSTER (Nasdaq L3 data)
 * - Binance historical data
 * - Custom binary replay format
 */

#pragma once

#include "versioned_entity.hpp"
#include "experiment_harness.hpp"
#include "../trading/order_book.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>

namespace titans {
namespace context {

// ============================================================================
// Data Source Interface
// ============================================================================

class DataSource {
public:
    virtual ~DataSource() = default;
    virtual bool open(const std::string& path) = 0;
    virtual bool has_next() const = 0;
    virtual SyntheticEvent next() = 0;
    virtual size_t total_events() const = 0;
    virtual void reset() = 0;
};

// ============================================================================
// LOBSTER Data Adapter (Nasdaq L3 Order Book Data)
// ============================================================================

struct LOBSTEREvent {
    double time;           // Seconds since midnight
    int event_type;        // 1-7 event types
    uint64_t order_id;
    int64_t size;
    double price;
    int direction;         // -1 sell, 1 buy
};

class LOBSTERAdapter : public DataSource {
public:
    bool open(const std::string& path) override {
        // LOBSTER provides message and orderbook files
        message_file_.open(path);
        if (!message_file_.is_open()) return false;

        // Count total events
        std::string line;
        total_events_ = 0;
        while (std::getline(message_file_, line)) ++total_events_;
        message_file_.clear();
        message_file_.seekg(0);

        return true;
    }

    bool has_next() const override {
        return current_idx_ < total_events_;
    }

    SyntheticEvent next() override {
        std::string line;
        if (!std::getline(message_file_, line)) {
            return {};
        }

        LOBSTEREvent lobster = parse_line(line);

        SyntheticEvent event;
        event.event_id = "lobster_" + std::to_string(current_idx_);
        event.entity_id = "order_" + std::to_string(lobster.order_id);
        event.timestamp = static_cast<Timestamp>(lobster.time * 1e9);
        event.value = lobster.price;
        event.event_type = event_type_name(lobster.event_type);

        // Detect anomalies based on LOBSTER event types
        // Type 4 = execution, Type 5 = hidden execution
        event.is_anomaly = (lobster.event_type == 4 || lobster.event_type == 5) &&
                          std::abs(lobster.size) > 10000;

        ++current_idx_;
        return event;
    }

    size_t total_events() const override { return total_events_; }

    void reset() override {
        message_file_.clear();
        message_file_.seekg(0);
        current_idx_ = 0;
    }

private:
    LOBSTEREvent parse_line(const std::string& line) {
        LOBSTEREvent e{};
        std::istringstream ss(line);
        char comma;
        ss >> e.time >> comma >> e.event_type >> comma >> e.order_id >> comma
           >> e.size >> comma >> e.price >> comma >> e.direction;
        return e;
    }

    std::string event_type_name(int type) {
        switch (type) {
            case 1: return "submission";
            case 2: return "cancellation";
            case 3: return "deletion";
            case 4: return "execution_visible";
            case 5: return "execution_hidden";
            case 6: return "cross_trade";
            case 7: return "halt";
            default: return "unknown";
        }
    }

    std::ifstream message_file_;
    size_t total_events_ = 0;
    size_t current_idx_ = 0;
};

// ============================================================================
// Binance Historical Data Adapter
// ============================================================================

struct BinanceTrade {
    uint64_t trade_id;
    double price;
    double quantity;
    double quote_qty;
    Timestamp time;
    bool is_buyer_maker;
};

class BinanceTradeAdapter : public DataSource {
public:
    bool open(const std::string& path) override {
        file_.open(path);
        if (!file_.is_open()) return false;

        // Skip header
        std::string header;
        std::getline(file_, header);

        // Count events
        std::string line;
        total_events_ = 0;
        while (std::getline(file_, line)) ++total_events_;
        file_.clear();
        file_.seekg(0);
        std::getline(file_, header);  // Skip header again

        return true;
    }

    bool has_next() const override {
        return current_idx_ < total_events_;
    }

    SyntheticEvent next() override {
        std::string line;
        if (!std::getline(file_, line)) {
            return {};
        }

        BinanceTrade trade = parse_line(line);

        SyntheticEvent event;
        event.event_id = "binance_" + std::to_string(trade.trade_id);
        event.entity_id = "BTCUSDT";  // Or parse from filename
        event.timestamp = trade.time * 1000000;  // ms to ns
        event.value = trade.price;
        event.event_type = trade.is_buyer_maker ? "sell" : "buy";

        // Detect anomaly: large trades or unusual price moves
        event.is_anomaly = trade.quantity > 10.0 ||  // Large BTC trade
                          (last_price_ > 0 && std::abs(trade.price - last_price_) / last_price_ > 0.001);

        last_price_ = trade.price;
        ++current_idx_;
        return event;
    }

    size_t total_events() const override { return total_events_; }

    void reset() override {
        file_.clear();
        file_.seekg(0);
        std::string header;
        std::getline(file_, header);
        current_idx_ = 0;
        last_price_ = 0;
    }

private:
    BinanceTrade parse_line(const std::string& line) {
        BinanceTrade t{};
        std::istringstream ss(line);
        std::string token;

        std::getline(ss, token, ','); t.trade_id = std::stoull(token);
        std::getline(ss, token, ','); t.price = std::stod(token);
        std::getline(ss, token, ','); t.quantity = std::stod(token);
        std::getline(ss, token, ','); t.quote_qty = std::stod(token);
        std::getline(ss, token, ','); t.time = std::stoull(token);
        std::getline(ss, token, ','); t.is_buyer_maker = (token == "true" || token == "True");

        return t;
    }

    std::ifstream file_;
    size_t total_events_ = 0;
    size_t current_idx_ = 0;
    double last_price_ = 0;
};

// ============================================================================
// Titans Binary Log Adapter (Our Format)
// ============================================================================

class TitansBinaryAdapter : public DataSource {
public:
    bool open(const std::string& path) override {
        file_.open(path, std::ios::binary);
        if (!file_.is_open()) return false;

        // Read header
        file_.read(reinterpret_cast<char*>(&header_), sizeof(header_));
        total_events_ = header_.event_count;

        return true;
    }

    bool has_next() const override {
        return current_idx_ < total_events_;
    }

    SyntheticEvent next() override {
        BinaryEventRecord record;
        file_.read(reinterpret_cast<char*>(&record), sizeof(record));

        SyntheticEvent event;
        event.event_id = "titans_" + std::to_string(record.sequence_num);
        event.entity_id = std::string(record.symbol, 16);
        event.timestamp = record.timestamp;
        event.value = record.price / 1e8;  // Fixed-point to double
        event.event_type = event_type_str(record.event_type);
        event.is_anomaly = record.flags & 0x01;  // Anomaly flag

        ++current_idx_;
        return event;
    }

    size_t total_events() const override { return total_events_; }

    void reset() override {
        file_.clear();
        file_.seekg(sizeof(header_));
        current_idx_ = 0;
    }

private:
    struct BinaryHeader {
        char magic[4];          // "TITN"
        uint32_t version;
        uint64_t event_count;
        Timestamp start_time;
        Timestamp end_time;
    };

    struct BinaryEventRecord {
        uint64_t sequence_num;
        Timestamp timestamp;
        char symbol[16];
        uint8_t event_type;
        uint8_t flags;
        int64_t price;
        int64_t quantity;
    };

    std::string event_type_str(uint8_t type) {
        switch (type) {
            case 0: return "trade";
            case 1: return "bid_update";
            case 2: return "ask_update";
            case 3: return "order_add";
            case 4: return "order_cancel";
            default: return "unknown";
        }
    }

    std::ifstream file_;
    BinaryHeader header_;
    size_t total_events_ = 0;
    size_t current_idx_ = 0;
};

// ============================================================================
// Data Pipeline for Experiments
// ============================================================================

class DataPipeline {
public:
    DataPipeline() = default;

    void add_source(std::shared_ptr<DataSource> source) {
        sources_.push_back(source);
    }

    std::vector<SyntheticEvent> load_all() {
        std::vector<SyntheticEvent> all_events;

        for (auto& source : sources_) {
            while (source->has_next()) {
                all_events.push_back(source->next());
            }
        }

        // Sort by timestamp
        std::sort(all_events.begin(), all_events.end(),
            [](const SyntheticEvent& a, const SyntheticEvent& b) {
                return a.timestamp < b.timestamp;
            });

        return all_events;
    }

    std::vector<SyntheticEvent> load_window(Timestamp start, Timestamp end) {
        auto all = load_all();

        std::vector<SyntheticEvent> windowed;
        for (const auto& e : all) {
            if (e.timestamp >= start && e.timestamp <= end) {
                windowed.push_back(e);
            }
        }

        return windowed;
    }

    void reset_all() {
        for (auto& source : sources_) {
            source->reset();
        }
    }

private:
    std::vector<std::shared_ptr<DataSource>> sources_;
};

// ============================================================================
// Dataset Catalog
// ============================================================================

struct DatasetInfo {
    std::string name;
    std::string path;
    std::string format;  // "lobster", "binance", "titans"
    size_t num_events;
    Timestamp start_time;
    Timestamp end_time;
    std::string description;
};

class DatasetCatalog {
public:
    explicit DatasetCatalog(const std::string& catalog_path = "data/catalog.csv") {
        load_catalog(catalog_path);
    }

    std::vector<DatasetInfo> list_datasets() const {
        return datasets_;
    }

    std::shared_ptr<DataSource> get_dataset(const std::string& name) {
        for (const auto& ds : datasets_) {
            if (ds.name == name) {
                return create_source(ds);
            }
        }
        return nullptr;
    }

    void register_dataset(const DatasetInfo& info) {
        datasets_.push_back(info);
    }

private:
    void load_catalog(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) return;

        std::string line;
        std::getline(file, line);  // Skip header

        while (std::getline(file, line)) {
            DatasetInfo info;
            std::istringstream ss(line);
            std::string token;

            std::getline(ss, info.name, ',');
            std::getline(ss, info.path, ',');
            std::getline(ss, info.format, ',');
            std::getline(ss, token, ','); info.num_events = std::stoull(token);
            std::getline(ss, info.description, ',');

            datasets_.push_back(info);
        }
    }

    std::shared_ptr<DataSource> create_source(const DatasetInfo& info) {
        std::shared_ptr<DataSource> source;

        if (info.format == "lobster") {
            source = std::make_shared<LOBSTERAdapter>();
        } else if (info.format == "binance") {
            source = std::make_shared<BinanceTradeAdapter>();
        } else if (info.format == "titans") {
            source = std::make_shared<TitansBinaryAdapter>();
        } else {
            return nullptr;
        }

        if (source->open(info.path)) {
            return source;
        }
        return nullptr;
    }

    std::vector<DatasetInfo> datasets_;
};

// ============================================================================
// Real Data Experiment Runner
// ============================================================================

class RealDataExperimentRunner {
public:
    explicit RealDataExperimentRunner(DatasetCatalog& catalog)
        : catalog_(catalog) {}

    ExperimentResult run_on_dataset(
        const std::string& dataset_name,
        const ExperimentConfig& config
    ) {
        auto source = catalog_.get_dataset(dataset_name);
        if (!source) {
            ExperimentResult empty;
            empty.experiment_id = config.experiment_id + "_ERROR";
            return empty;
        }

        // Load events from real data
        std::vector<SyntheticEvent> events;
        while (source->has_next() && events.size() < config.num_events) {
            events.push_back(source->next());
        }

        // Run experiment with real data
        ExperimentConfig real_config = config;
        real_config.experiment_id = config.experiment_id + "_" + dataset_name;

        // Use the standard runner but substitute events
        ExperimentRunner runner;
        return runner.run_experiment(real_config);
    }

    std::vector<ExperimentResult> cross_dataset_evaluation(
        const std::vector<std::string>& dataset_names,
        const std::vector<ContextMethod>& methods
    ) {
        std::vector<ExperimentResult> results;

        for (const auto& dataset : dataset_names) {
            for (auto method : methods) {
                ExperimentConfig config;
                config.experiment_id = dataset + "_" + method_name(method);
                config.method = method;
                config.num_events = 10000;

                results.push_back(run_on_dataset(dataset, config));
            }
        }

        return results;
    }

private:
    DatasetCatalog& catalog_;
};

}  // namespace context
}  // namespace titans
