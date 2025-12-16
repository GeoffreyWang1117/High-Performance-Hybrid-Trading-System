/**
 * @file replay_main.cpp
 * @brief Standalone Replay Tool
 *
 * Tool for replaying historical market data from binary log files.
 * Useful for debugging and analysis.
 */

#include "titans/market_data/binary_logger.hpp"
#include "titans/trading/order_book.hpp"

#include <iostream>
#include <string>
#include <chrono>

using namespace titans;

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " <log_file> [options]\n\n"
              << "Options:\n"
              << "  --info          Show file information only\n"
              << "  --stats         Print statistics\n"
              << "  --verbose       Print each record\n"
              << "  --speed <n>     Replay speed multiplier (default: 1.0)\n"
              << "  --start <ts>    Start timestamp (ns)\n"
              << "  --end <ts>      End timestamp (ns)\n"
              << std::endl;
}

class PrintVisitor : public IRecordVisitor {
public:
    PrintVisitor(bool verbose = false) : verbose_(verbose) {}

    void on_book_snapshot(const BookSnapshotRecord& record,
                         const PriceLevel* bids,
                         const PriceLevel* asks) override {
        ++snapshots_;
        if (verbose_) {
            std::cout << "SNAPSHOT " << record.symbol.view()
                      << " seq=" << record.seq_num
                      << " bids=" << (int)record.bid_levels
                      << " asks=" << (int)record.ask_levels << "\n";
        }
    }

    void on_book_update(const BookUpdateRecord& record) override {
        ++updates_;
        if (verbose_) {
            std::cout << "UPDATE " << record.symbol.view()
                      << " " << (record.side == Side::Buy ? "BID" : "ASK")
                      << " price=" << from_price(record.price)
                      << " qty=" << from_quantity(record.quantity) << "\n";
        }
    }

    void on_trade(const TradeRecord& record) override {
        ++trades_;
        if (verbose_) {
            std::cout << "TRADE " << record.symbol.view()
                      << " id=" << record.trade_id
                      << " price=" << from_price(record.price)
                      << " qty=" << from_quantity(record.quantity)
                      << " side=" << (record.aggressor_side == Side::Buy ? "BUY" : "SELL")
                      << "\n";
        }
    }

    void on_quote(const QuoteRecord& record) override {
        ++quotes_;
        if (verbose_) {
            std::cout << "QUOTE " << record.symbol.view()
                      << " bid=" << from_price(record.bid_price)
                      << "@" << from_quantity(record.bid_qty)
                      << " ask=" << from_price(record.ask_price)
                      << "@" << from_quantity(record.ask_qty) << "\n";
        }
    }

    void print_stats() const {
        std::cout << "\n=== Replay Statistics ===\n";
        std::cout << "Snapshots: " << snapshots_ << "\n";
        std::cout << "Updates:   " << updates_ << "\n";
        std::cout << "Trades:    " << trades_ << "\n";
        std::cout << "Quotes:    " << quotes_ << "\n";
        std::cout << "Total:     " << (snapshots_ + updates_ + trades_ + quotes_) << "\n";
    }

private:
    bool verbose_;
    uint64_t snapshots_ = 0;
    uint64_t updates_ = 0;
    uint64_t trades_ = 0;
    uint64_t quotes_ = 0;
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string file_path = argv[1];
    bool info_only = false;
    bool verbose = false;
    bool show_stats = true;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--info") {
            info_only = true;
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--stats") {
            show_stats = true;
        }
    }

    // Open file
    ReplayEngine replay;
    if (!replay.open(file_path)) {
        std::cerr << "Error: Failed to open file: " << file_path << "\n";
        return 1;
    }

    // Print header info
    const auto& header = replay.header();
    std::cout << "=== Log File Info ===\n";
    std::cout << "File:    " << file_path << "\n";
    std::cout << "Version: " << header.version << "\n";
    std::cout << "Symbol:  " << header.symbol << "\n";
    std::cout << "Records: " << header.num_records << "\n";
    std::cout << "Size:    " << header.file_size << " bytes\n";

    // Format timestamps
    auto format_time = [](Timestamp ts) -> std::string {
        time_t secs = ts / 1000000000LL;
        struct tm* tm = std::localtime(&secs);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
        return std::string(buf);
    };

    std::cout << "Start:   " << format_time(header.start_time) << "\n";
    std::cout << "End:     " << format_time(header.end_time) << "\n";
    std::cout << "====================\n\n";

    if (info_only) {
        return 0;
    }

    // Replay
    PrintVisitor visitor(verbose);

    auto start = std::chrono::high_resolution_clock::now();
    replay.replay(visitor);
    auto end = std::chrono::high_resolution_clock::now();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    if (show_stats) {
        visitor.print_stats();
        std::cout << "\nReplay time: " << ms << " ms\n";
        if (ms > 0) {
            std::cout << "Throughput:  " << (header.num_records * 1000 / ms) << " records/sec\n";
        }
    }

    return 0;
}
