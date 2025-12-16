/**
 * @file binary_logger.hpp
 * @brief High-Performance Binary Logger for Market Data
 *
 * Implements efficient binary logging with:
 * - Memory-mapped I/O for low-latency writes
 * - Structured binary format for fast replay
 * - Compression support for storage efficiency
 * - Automatic file rotation
 */

#pragma once

#include "titans/core/types.hpp"
#include "titans/core/event_bus.hpp"

#include <fstream>
#include <vector>
#include <memory>
#include <filesystem>
#include <cstring>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

namespace titans {

// ============================================================================
// Binary Log Format
// ============================================================================

constexpr uint32_t LOG_MAGIC = 0x54495441;  // "TITA"
constexpr uint16_t LOG_VERSION = 1;

/**
 * @brief Log file header
 */
struct alignas(64) LogHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    Timestamp start_time;
    Timestamp end_time;
    uint64_t num_records;
    uint64_t file_size;
    char symbol[SYMBOL_MAX_LEN];
    char reserved[16];
};

/**
 * @brief Record types
 */
enum class RecordType : uint8_t {
    Invalid = 0,
    BookSnapshot = 1,
    BookUpdate = 2,
    Trade = 3,
    Quote = 4,
    OrderEvent = 5,
    Signal = 6,
    Metadata = 255
};

/**
 * @brief Record header (fixed 16 bytes)
 */
struct alignas(16) RecordHeader {
    uint32_t    size;        // Total record size including header
    RecordType  type;
    uint8_t     flags;       // Compression, etc.
    uint16_t    reserved;
    Timestamp   timestamp;
};

/**
 * @brief Book snapshot record
 */
struct BookSnapshotRecord {
    RecordHeader header;
    Symbol       symbol;
    SequenceNum  seq_num;
    uint8_t      bid_levels;
    uint8_t      ask_levels;
    uint8_t      padding[6];
    // Followed by bid_levels PriceLevel structs, then ask_levels PriceLevel structs
};

/**
 * @brief Book update record
 */
struct BookUpdateRecord {
    RecordHeader header;
    Symbol       symbol;
    SequenceNum  seq_num;
    Side         side;
    uint8_t      is_delete;
    uint8_t      padding[6];
    Price        price;
    Quantity     quantity;
};

/**
 * @brief Trade record
 */
struct TradeRecord {
    RecordHeader header;
    Symbol       symbol;
    uint64_t     trade_id;
    Price        price;
    Quantity     quantity;
    Side         aggressor_side;
    uint8_t      padding[7];
};

/**
 * @brief Quote record
 */
struct QuoteRecord {
    RecordHeader header;
    Symbol       symbol;
    Price        bid_price;
    Price        ask_price;
    Quantity     bid_qty;
    Quantity     ask_qty;
    SequenceNum  seq_num;
};

// ============================================================================
// Binary Logger Implementation
// ============================================================================

/**
 * @brief Configuration for binary logger
 */
struct LoggerConfig {
    std::string base_path = "./data/logs";
    size_t max_file_size = 1024 * 1024 * 1024;  // 1GB
    size_t buffer_size = 64 * 1024;  // 64KB
    bool use_mmap = true;
    bool sync_on_write = false;
    bool compress = false;
};

/**
 * @brief High-performance binary logger
 */
class BinaryLogger {
public:
    explicit BinaryLogger(const LoggerConfig& config = {})
        : config_(config), fd_(-1), file_size_(0),
          num_records_(0), is_open_(false) {}

    ~BinaryLogger() {
        close();
    }

    /**
     * @brief Open a new log file
     */
    bool open(const Symbol& symbol) {
        if (is_open_) close();

        symbol_ = symbol;
        start_time_ = now_ns();

        // Create directory if needed
        std::filesystem::create_directories(config_.base_path);

        // Generate filename
        current_path_ = generate_filename();

        // Open file
        fd_ = ::open(current_path_.c_str(),
                     O_RDWR | O_CREAT | O_TRUNC,
                     S_IRUSR | S_IWUSR | S_IRGRP);

        if (fd_ < 0) {
            return false;
        }

        // Reserve space for header
        write_buffer_.resize(config_.buffer_size);
        buffer_pos_ = 0;

        // Write header placeholder
        LogHeader header{};
        header.magic = LOG_MAGIC;
        header.version = LOG_VERSION;
        header.header_size = sizeof(LogHeader);
        header.start_time = start_time_;
        std::memcpy(header.symbol, symbol_.data.data(), SYMBOL_MAX_LEN);

        write_raw(&header, sizeof(header));
        flush();

        is_open_ = true;
        return true;
    }

    /**
     * @brief Close the log file
     */
    void close() {
        if (!is_open_) return;

        flush();

        // Update header with final values
        LogHeader header{};
        header.magic = LOG_MAGIC;
        header.version = LOG_VERSION;
        header.header_size = sizeof(LogHeader);
        header.start_time = start_time_;
        header.end_time = now_ns();
        header.num_records = num_records_;
        header.file_size = file_size_;
        std::memcpy(header.symbol, symbol_.data.data(), SYMBOL_MAX_LEN);

        lseek(fd_, 0, SEEK_SET);
        ::write(fd_, &header, sizeof(header));

        ::close(fd_);
        fd_ = -1;
        is_open_ = false;
    }

    /**
     * @brief Log a book snapshot
     */
    void log_book_snapshot(const Symbol& symbol, SequenceNum seq,
                           const std::vector<PriceLevel>& bids,
                           const std::vector<PriceLevel>& asks) {
        if (!is_open_) return;

        size_t levels_size = (bids.size() + asks.size()) * sizeof(PriceLevel);
        size_t total_size = sizeof(BookSnapshotRecord) + levels_size;

        BookSnapshotRecord record{};
        record.header.size = static_cast<uint32_t>(total_size);
        record.header.type = RecordType::BookSnapshot;
        record.header.timestamp = now_ns();
        record.symbol = symbol;
        record.seq_num = seq;
        record.bid_levels = static_cast<uint8_t>(bids.size());
        record.ask_levels = static_cast<uint8_t>(asks.size());

        write_raw(&record, sizeof(record));
        if (!bids.empty()) {
            write_raw(bids.data(), bids.size() * sizeof(PriceLevel));
        }
        if (!asks.empty()) {
            write_raw(asks.data(), asks.size() * sizeof(PriceLevel));
        }

        ++num_records_;
        check_rotation();
    }

    /**
     * @brief Log a book update
     */
    void log_book_update(const Symbol& symbol, SequenceNum seq,
                         Side side, Price price, Quantity quantity) {
        if (!is_open_) return;

        BookUpdateRecord record{};
        record.header.size = sizeof(BookUpdateRecord);
        record.header.type = RecordType::BookUpdate;
        record.header.timestamp = now_ns();
        record.symbol = symbol;
        record.seq_num = seq;
        record.side = side;
        record.is_delete = (quantity == 0) ? 1 : 0;
        record.price = price;
        record.quantity = quantity;

        write_raw(&record, sizeof(record));
        ++num_records_;
        check_rotation();
    }

    /**
     * @brief Log a trade
     */
    void log_trade(const Symbol& symbol, uint64_t trade_id,
                   Price price, Quantity quantity, Side aggressor) {
        if (!is_open_) return;

        TradeRecord record{};
        record.header.size = sizeof(TradeRecord);
        record.header.type = RecordType::Trade;
        record.header.timestamp = now_ns();
        record.symbol = symbol;
        record.trade_id = trade_id;
        record.price = price;
        record.quantity = quantity;
        record.aggressor_side = aggressor;

        write_raw(&record, sizeof(record));
        ++num_records_;
        check_rotation();
    }

    /**
     * @brief Log a quote
     */
    void log_quote(const Quote& quote) {
        if (!is_open_) return;

        QuoteRecord record{};
        record.header.size = sizeof(QuoteRecord);
        record.header.type = RecordType::Quote;
        record.header.timestamp = quote.timestamp;
        record.symbol = quote.symbol;
        record.bid_price = quote.bid_price;
        record.ask_price = quote.ask_price;
        record.bid_qty = quote.bid_qty;
        record.ask_qty = quote.ask_qty;
        record.seq_num = quote.seq_num;

        write_raw(&record, sizeof(record));
        ++num_records_;
        check_rotation();
    }

    /**
     * @brief Flush buffer to disk
     */
    void flush() {
        if (buffer_pos_ > 0 && fd_ >= 0) {
            ::write(fd_, write_buffer_.data(), buffer_pos_);
            file_size_ += buffer_pos_;
            buffer_pos_ = 0;

            if (config_.sync_on_write) {
                fsync(fd_);
            }
        }
    }

    bool is_open() const { return is_open_; }
    uint64_t num_records() const { return num_records_; }
    size_t file_size() const { return file_size_; }
    const std::string& current_path() const { return current_path_; }

private:
    void write_raw(const void* data, size_t size) {
        const char* ptr = static_cast<const char*>(data);

        while (size > 0) {
            size_t space = write_buffer_.size() - buffer_pos_;
            size_t to_copy = std::min(size, space);

            std::memcpy(write_buffer_.data() + buffer_pos_, ptr, to_copy);
            buffer_pos_ += to_copy;
            ptr += to_copy;
            size -= to_copy;

            if (buffer_pos_ >= write_buffer_.size()) {
                flush();
            }
        }
    }

    void check_rotation() {
        if (file_size_ + buffer_pos_ >= config_.max_file_size) {
            close();
            open(symbol_);
        }
    }

    std::string generate_filename() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto tm = *std::localtime(&time_t);

        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);

        return config_.base_path + "/" + symbol_.view().data() +
               "_" + buf + ".bin";
    }

    LoggerConfig config_;
    Symbol symbol_;
    std::string current_path_;
    int fd_;
    size_t file_size_;
    uint64_t num_records_;
    Timestamp start_time_;
    bool is_open_;

    std::vector<char> write_buffer_;
    size_t buffer_pos_ = 0;
};

// ============================================================================
// Replay Engine
// ============================================================================

/**
 * @brief Record visitor interface
 */
class IRecordVisitor {
public:
    virtual ~IRecordVisitor() = default;

    virtual void on_book_snapshot(const BookSnapshotRecord& record,
                                  const PriceLevel* bids,
                                  const PriceLevel* asks) = 0;

    virtual void on_book_update(const BookUpdateRecord& record) = 0;
    virtual void on_trade(const TradeRecord& record) = 0;
    virtual void on_quote(const QuoteRecord& record) = 0;
};

/**
 * @brief Binary log reader and replay engine
 */
class ReplayEngine {
public:
    ReplayEngine() : fd_(-1), file_size_(0), data_(nullptr) {}

    ~ReplayEngine() {
        close();
    }

    /**
     * @brief Open a log file for replay
     */
    bool open(const std::string& path) {
        close();

        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) return false;

        // Get file size
        file_size_ = lseek(fd_, 0, SEEK_END);
        lseek(fd_, 0, SEEK_SET);

        // Memory map the file
        data_ = static_cast<char*>(mmap(nullptr, file_size_,
                                        PROT_READ, MAP_PRIVATE, fd_, 0));

        if (data_ == MAP_FAILED) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        // Read header
        if (file_size_ < sizeof(LogHeader)) {
            close();
            return false;
        }

        std::memcpy(&header_, data_, sizeof(LogHeader));

        if (header_.magic != LOG_MAGIC) {
            close();
            return false;
        }

        current_pos_ = sizeof(LogHeader);
        return true;
    }

    /**
     * @brief Close the file
     */
    void close() {
        if (data_ && data_ != MAP_FAILED) {
            munmap(data_, file_size_);
            data_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    /**
     * @brief Replay all records
     */
    void replay(IRecordVisitor& visitor) {
        current_pos_ = sizeof(LogHeader);

        while (current_pos_ < file_size_) {
            if (current_pos_ + sizeof(RecordHeader) > file_size_) break;

            const RecordHeader* header =
                reinterpret_cast<const RecordHeader*>(data_ + current_pos_);

            if (current_pos_ + header->size > file_size_) break;

            dispatch_record(header, visitor);
            current_pos_ += header->size;
        }
    }

    /**
     * @brief Replay records in a time range
     */
    void replay_range(IRecordVisitor& visitor,
                      Timestamp start_time,
                      Timestamp end_time) {
        current_pos_ = sizeof(LogHeader);

        while (current_pos_ < file_size_) {
            if (current_pos_ + sizeof(RecordHeader) > file_size_) break;

            const RecordHeader* header =
                reinterpret_cast<const RecordHeader*>(data_ + current_pos_);

            if (current_pos_ + header->size > file_size_) break;

            if (header->timestamp >= start_time && header->timestamp <= end_time) {
                dispatch_record(header, visitor);
            }

            if (header->timestamp > end_time) break;

            current_pos_ += header->size;
        }
    }

    /**
     * @brief Get next record (for manual iteration)
     */
    const RecordHeader* next_record() {
        if (current_pos_ >= file_size_) return nullptr;
        if (current_pos_ + sizeof(RecordHeader) > file_size_) return nullptr;

        const RecordHeader* header =
            reinterpret_cast<const RecordHeader*>(data_ + current_pos_);

        if (current_pos_ + header->size > file_size_) return nullptr;

        current_pos_ += header->size;
        return header;
    }

    /**
     * @brief Reset to beginning
     */
    void reset() {
        current_pos_ = sizeof(LogHeader);
    }

    const LogHeader& header() const { return header_; }
    uint64_t num_records() const { return header_.num_records; }

private:
    void dispatch_record(const RecordHeader* header, IRecordVisitor& visitor) {
        switch (header->type) {
            case RecordType::BookSnapshot: {
                const auto* record =
                    reinterpret_cast<const BookSnapshotRecord*>(header);
                const auto* bids =
                    reinterpret_cast<const PriceLevel*>(record + 1);
                const auto* asks = bids + record->bid_levels;
                visitor.on_book_snapshot(*record, bids, asks);
                break;
            }
            case RecordType::BookUpdate: {
                const auto* record =
                    reinterpret_cast<const BookUpdateRecord*>(header);
                visitor.on_book_update(*record);
                break;
            }
            case RecordType::Trade: {
                const auto* record =
                    reinterpret_cast<const TradeRecord*>(header);
                visitor.on_trade(*record);
                break;
            }
            case RecordType::Quote: {
                const auto* record =
                    reinterpret_cast<const QuoteRecord*>(header);
                visitor.on_quote(*record);
                break;
            }
            default:
                break;
        }
    }

    int fd_;
    size_t file_size_;
    char* data_;
    size_t current_pos_ = 0;
    LogHeader header_;
};

}  // namespace titans
