/**
 * @file debug.hpp
 * @brief Debugging and Diagnostic Utilities
 *
 * Comprehensive debugging tools for development and troubleshooting:
 * - Logging with levels and categories
 * - Assertion macros with detailed output
 * - Performance profiling
 * - Memory tracking
 * - State inspection
 */

#pragma once

#include <string>
#include <sstream>
#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <mutex>
#include <map>
#include <vector>
#include <functional>
#include <iomanip>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <algorithm>
#include <limits>

#ifdef __GNUC__
#include <execinfo.h>
#endif

namespace titans {
namespace debug {

// ============================================================================
// Log Levels and Categories
// ============================================================================

enum class LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Fatal = 5,
    Off = 6
};

enum class LogCategory {
    General,
    Context,
    LLM,
    Experiment,
    GPU,
    Network,
    Memory,
    Performance
};

inline const char* level_str(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default: return "?????";
    }
}

inline const char* level_color(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "\033[90m";  // Gray
        case LogLevel::Debug: return "\033[36m";  // Cyan
        case LogLevel::Info:  return "\033[32m";  // Green
        case LogLevel::Warn:  return "\033[33m";  // Yellow
        case LogLevel::Error: return "\033[31m";  // Red
        case LogLevel::Fatal: return "\033[35m";  // Magenta
        default: return "\033[0m";
    }
}

inline const char* category_str(LogCategory cat) {
    switch (cat) {
        case LogCategory::General:     return "GEN";
        case LogCategory::Context:     return "CTX";
        case LogCategory::LLM:         return "LLM";
        case LogCategory::Experiment:  return "EXP";
        case LogCategory::GPU:         return "GPU";
        case LogCategory::Network:     return "NET";
        case LogCategory::Memory:      return "MEM";
        case LogCategory::Performance: return "PRF";
        default: return "???";
    }
}

// ============================================================================
// Logger
// ============================================================================

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void set_level(LogLevel level) { min_level_ = level; }
    void set_color(bool enabled) { color_enabled_ = enabled; }
    void set_file(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) file_.close();
        file_.open(path, std::ios::app);
    }

    void log(LogLevel level, LogCategory category,
             const std::string& file, int line,
             const std::string& func, const std::string& message) {
        if (level < min_level_) return;

        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count() % 1000;

        std::ostringstream ss;

        // Timestamp
        ss << std::put_time(std::localtime(&time_t), "%H:%M:%S");
        ss << "." << std::setfill('0') << std::setw(3) << ms;

        // Thread ID
        ss << " [" << std::hex << std::this_thread::get_id() << std::dec << "]";

        // Level and category
        ss << " " << level_str(level) << " " << category_str(category);

        // Location (shortened)
        std::string short_file = file;
        auto pos = short_file.rfind('/');
        if (pos != std::string::npos) short_file = short_file.substr(pos + 1);
        ss << " " << short_file << ":" << line;

        // Message
        ss << " | " << message;

        std::string log_line = ss.str();

        std::lock_guard<std::mutex> lock(mutex_);

        // Console output
        if (color_enabled_) {
            std::cerr << level_color(level) << log_line << "\033[0m" << std::endl;
        } else {
            std::cerr << log_line << std::endl;
        }

        // File output
        if (file_.is_open()) {
            file_ << log_line << std::endl;
        }
    }

private:
    Logger() : min_level_(LogLevel::Info), color_enabled_(true) {}

    LogLevel min_level_;
    bool color_enabled_;
    std::ofstream file_;
    std::mutex mutex_;
};

// Logging macros
#define TITANS_LOG(level, category, msg) \
    titans::debug::Logger::instance().log( \
        level, category, __FILE__, __LINE__, __func__, msg)

#define LOG_TRACE(cat, msg) TITANS_LOG(titans::debug::LogLevel::Trace, cat, msg)
#define LOG_DEBUG(cat, msg) TITANS_LOG(titans::debug::LogLevel::Debug, cat, msg)
#define LOG_INFO(cat, msg)  TITANS_LOG(titans::debug::LogLevel::Info, cat, msg)
#define LOG_WARN(cat, msg)  TITANS_LOG(titans::debug::LogLevel::Warn, cat, msg)
#define LOG_ERROR(cat, msg) TITANS_LOG(titans::debug::LogLevel::Error, cat, msg)
#define LOG_FATAL(cat, msg) TITANS_LOG(titans::debug::LogLevel::Fatal, cat, msg)

// ============================================================================
// Assertions
// ============================================================================

inline void assertion_failed(const char* expr, const char* file, int line,
                             const char* func, const std::string& msg = "") {
    std::ostringstream ss;
    ss << "Assertion failed: " << expr << "\n";
    ss << "  Location: " << file << ":" << line << " in " << func << "\n";
    if (!msg.empty()) {
        ss << "  Message: " << msg << "\n";
    }

    LOG_FATAL(LogCategory::General, ss.str());

    // Print stack trace if available
    #ifdef __GNUC__
    void* array[20];
    int size = backtrace(array, 20);
    char** strings = backtrace_symbols(array, size);
    if (strings) {
        std::cerr << "Stack trace:\n";
        for (int i = 0; i < size; ++i) {
            std::cerr << "  " << strings[i] << "\n";
        }
        free(strings);
    }
    #endif

    std::abort();
}

#define TITANS_ASSERT(expr) \
    do { \
        if (!(expr)) { \
            titans::debug::assertion_failed(#expr, __FILE__, __LINE__, __func__); \
        } \
    } while (0)

#define TITANS_ASSERT_MSG(expr, msg) \
    do { \
        if (!(expr)) { \
            titans::debug::assertion_failed(#expr, __FILE__, __LINE__, __func__, msg); \
        } \
    } while (0)

#define TITANS_ASSERT_EQ(a, b) \
    do { \
        if (!((a) == (b))) { \
            std::ostringstream _ss; \
            _ss << "Expected " << #a << " == " << #b << ", got " << (a) << " vs " << (b); \
            titans::debug::assertion_failed(#a " == " #b, __FILE__, __LINE__, __func__, _ss.str()); \
        } \
    } while (0)

#define TITANS_ASSERT_NE(a, b) TITANS_ASSERT_MSG((a) != (b), #a " should != " #b)
#define TITANS_ASSERT_LT(a, b) TITANS_ASSERT_MSG((a) < (b), #a " should < " #b)
#define TITANS_ASSERT_LE(a, b) TITANS_ASSERT_MSG((a) <= (b), #a " should <= " #b)
#define TITANS_ASSERT_GT(a, b) TITANS_ASSERT_MSG((a) > (b), #a " should > " #b)
#define TITANS_ASSERT_GE(a, b) TITANS_ASSERT_MSG((a) >= (b), #a " should >= " #b)

// Debug-only assertions
#ifdef NDEBUG
#define TITANS_DEBUG_ASSERT(expr) ((void)0)
#else
#define TITANS_DEBUG_ASSERT(expr) TITANS_ASSERT(expr)
#endif

// ============================================================================
// Performance Profiling
// ============================================================================

class ScopedTimer {
public:
    ScopedTimer(const std::string& name, LogCategory category = LogCategory::Performance)
        : name_(name), category_(category),
          start_(std::chrono::high_resolution_clock::now()) {}

    ~ScopedTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start_).count();

        std::ostringstream ss;
        ss << name_ << ": " << std::fixed << std::setprecision(3) << duration << " ms";
        LOG_DEBUG(category_, ss.str());
    }

private:
    std::string name_;
    LogCategory category_;
    std::chrono::high_resolution_clock::time_point start_;
};

#define TITANS_PROFILE_SCOPE(name) \
    titans::debug::ScopedTimer _timer_##__LINE__(name)

#define TITANS_PROFILE_FUNCTION() \
    titans::debug::ScopedTimer _timer_##__LINE__(__func__)

class Profiler {
public:
    struct Stats {
        std::string name;
        size_t call_count = 0;
        double total_time_ms = 0;
        double min_time_ms = std::numeric_limits<double>::max();
        double max_time_ms = 0;

        double avg_time_ms() const {
            return call_count > 0 ? total_time_ms / call_count : 0;
        }
    };

    static Profiler& instance() {
        static Profiler profiler;
        return profiler;
    }

    void record(const std::string& name, double duration_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& stats = stats_[name];
        stats.name = name;
        stats.call_count++;
        stats.total_time_ms += duration_ms;
        stats.min_time_ms = std::min(stats.min_time_ms, duration_ms);
        stats.max_time_ms = std::max(stats.max_time_ms, duration_ms);
    }

    std::vector<Stats> get_stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Stats> result;
        for (const auto& [_, stats] : stats_) {
            result.push_back(stats);
        }
        return result;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.clear();
    }

    void print_report() const {
        auto stats = get_stats();

        std::sort(stats.begin(), stats.end(),
            [](const Stats& a, const Stats& b) {
                return a.total_time_ms > b.total_time_ms;
            });

        printf("\n╔═══════════════════════════════════════════════════════════════════════╗\n");
        printf("║                      PROFILING REPORT                                 ║\n");
        printf("╠═══════════════════════════════════════════════════════════════════════╣\n");
        printf("║ Name                      │ Calls  │ Total(ms) │ Avg(ms) │ Min/Max    ║\n");
        printf("╠═══════════════════════════╪════════╪═══════════╪═════════╪════════════╣\n");

        for (const auto& s : stats) {
            printf("║ %-25s │ %6zu │ %9.2f │ %7.3f │ %.2f/%.2f  ║\n",
                   s.name.substr(0, 25).c_str(),
                   s.call_count, s.total_time_ms, s.avg_time_ms(),
                   s.min_time_ms, s.max_time_ms);
        }

        printf("╚═══════════════════════════════════════════════════════════════════════╝\n");
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, Stats> stats_;
};

class ProfiledScope {
public:
    ProfiledScope(const std::string& name)
        : name_(name), start_(std::chrono::high_resolution_clock::now()) {}

    ~ProfiledScope() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start_).count();
        Profiler::instance().record(name_, duration);
    }

private:
    std::string name_;
    std::chrono::high_resolution_clock::time_point start_;
};

#define TITANS_PROFILE(name) \
    titans::debug::ProfiledScope _profiled_##__LINE__(name)

// ============================================================================
// Memory Tracking
// ============================================================================

class MemoryTracker {
public:
    static MemoryTracker& instance() {
        static MemoryTracker tracker;
        return tracker;
    }

    void track_allocation(const std::string& category, size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        allocations_[category] += bytes;
        total_allocated_ += bytes;
        peak_allocated_ = std::max(peak_allocated_.load(), total_allocated_.load());
    }

    void track_deallocation(const std::string& category, size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        allocations_[category] -= bytes;
        total_allocated_ -= bytes;
    }

    size_t total_allocated() const { return total_allocated_; }
    size_t peak_allocated() const { return peak_allocated_; }

    void print_report() const {
        std::lock_guard<std::mutex> lock(mutex_);

        printf("\n╔═══════════════════════════════════════════════════════════════╗\n");
        printf("║                    MEMORY REPORT                              ║\n");
        printf("╠═══════════════════════════════════════════════════════════════╣\n");
        printf("║ Total Allocated: %10zu bytes (%6.2f MB)                  ║\n",
               total_allocated_.load(), total_allocated_.load() / (1024.0 * 1024));
        printf("║ Peak Allocated:  %10zu bytes (%6.2f MB)                  ║\n",
               peak_allocated_.load(), peak_allocated_.load() / (1024.0 * 1024));
        printf("╠───────────────────────────────────────────────────────────────╣\n");
        printf("║ Category               │ Current Bytes                       ║\n");

        for (const auto& [cat, bytes] : allocations_) {
            printf("║ %-22s │ %12zu (%6.2f MB)            ║\n",
                   cat.c_str(), bytes, bytes / (1024.0 * 1024));
        }

        printf("╚═══════════════════════════════════════════════════════════════╝\n");
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, size_t> allocations_;
    std::atomic<size_t> total_allocated_{0};
    std::atomic<size_t> peak_allocated_{0};
};

#define TITANS_TRACK_ALLOC(category, bytes) \
    titans::debug::MemoryTracker::instance().track_allocation(category, bytes)

#define TITANS_TRACK_DEALLOC(category, bytes) \
    titans::debug::MemoryTracker::instance().track_deallocation(category, bytes)

// ============================================================================
// Debug Dumpers
// ============================================================================

class Dumper {
public:
    template <typename T>
    static std::string hex_dump(const T* data, size_t count, size_t bytes_per_line = 16) {
        std::ostringstream ss;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
        size_t total_bytes = count * sizeof(T);

        for (size_t i = 0; i < total_bytes; i += bytes_per_line) {
            ss << std::hex << std::setfill('0') << std::setw(8) << i << ": ";

            // Hex bytes
            for (size_t j = 0; j < bytes_per_line && i + j < total_bytes; ++j) {
                ss << std::setw(2) << static_cast<int>(bytes[i + j]) << " ";
            }

            // Padding
            for (size_t j = total_bytes - i; j < bytes_per_line; ++j) {
                ss << "   ";
            }

            ss << " | ";

            // ASCII
            for (size_t j = 0; j < bytes_per_line && i + j < total_bytes; ++j) {
                char c = bytes[i + j];
                ss << (std::isprint(c) ? c : '.');
            }

            ss << "\n";
        }

        return ss.str();
    }

    static std::string json_pretty(const std::string& json, int indent = 2) {
        std::ostringstream ss;
        int depth = 0;
        bool in_string = false;

        for (size_t i = 0; i < json.size(); ++i) {
            char c = json[i];

            if (c == '"' && (i == 0 || json[i-1] != '\\')) {
                in_string = !in_string;
            }

            if (in_string) {
                ss << c;
                continue;
            }

            switch (c) {
                case '{':
                case '[':
                    ss << c << '\n';
                    depth++;
                    ss << std::string(depth * indent, ' ');
                    break;
                case '}':
                case ']':
                    ss << '\n';
                    depth--;
                    ss << std::string(depth * indent, ' ') << c;
                    break;
                case ',':
                    ss << c << '\n' << std::string(depth * indent, ' ');
                    break;
                case ':':
                    ss << ": ";
                    break;
                default:
                    if (!std::isspace(c)) ss << c;
            }
        }

        return ss.str();
    }
};

// ============================================================================
// Debug Console
// ============================================================================

class DebugConsole {
public:
    using CommandHandler = std::function<std::string(const std::vector<std::string>&)>;

    static DebugConsole& instance() {
        static DebugConsole console;
        return console;
    }

    void register_command(const std::string& name, const std::string& help,
                          CommandHandler handler) {
        commands_[name] = {help, handler};
    }

    std::string execute(const std::string& input) {
        auto parts = split(input);
        if (parts.empty()) return "";

        std::string cmd = parts[0];
        parts.erase(parts.begin());

        if (cmd == "help") {
            return help_text();
        }

        auto it = commands_.find(cmd);
        if (it == commands_.end()) {
            return "Unknown command: " + cmd + ". Type 'help' for available commands.";
        }

        try {
            return it->second.handler(parts);
        } catch (const std::exception& e) {
            return "Error: " + std::string(e.what());
        }
    }

    void run_interactive() {
        std::cout << "Titans Debug Console. Type 'help' for commands.\n";
        std::string line;

        while (std::cout << "> " && std::getline(std::cin, line)) {
            if (line == "quit" || line == "exit") break;
            std::cout << execute(line) << std::endl;
        }
    }

private:
    DebugConsole() {
        // Built-in commands
        register_command("profile", "Show profiling report", [](auto&) {
            std::ostringstream ss;
            auto stats = Profiler::instance().get_stats();
            for (const auto& s : stats) {
                ss << s.name << ": " << s.call_count << " calls, "
                   << s.avg_time_ms() << " ms avg\n";
            }
            return ss.str();
        });

        register_command("memory", "Show memory report", [](auto&) {
            std::ostringstream ss;
            ss << "Total: " << MemoryTracker::instance().total_allocated() << " bytes\n";
            ss << "Peak: " << MemoryTracker::instance().peak_allocated() << " bytes\n";
            return ss.str();
        });

        register_command("loglevel", "Set log level (trace/debug/info/warn/error)", [](auto& args) {
            if (args.empty()) return std::string("Usage: loglevel <level>");
            std::string level = args[0];
            if (level == "trace") Logger::instance().set_level(LogLevel::Trace);
            else if (level == "debug") Logger::instance().set_level(LogLevel::Debug);
            else if (level == "info") Logger::instance().set_level(LogLevel::Info);
            else if (level == "warn") Logger::instance().set_level(LogLevel::Warn);
            else if (level == "error") Logger::instance().set_level(LogLevel::Error);
            else return std::string("Unknown level: " + level);
            return std::string("Log level set to " + level);
        });
    }

    std::string help_text() const {
        std::ostringstream ss;
        ss << "Available commands:\n";
        for (const auto& [name, cmd] : commands_) {
            ss << "  " << name << " - " << cmd.help << "\n";
        }
        ss << "  help - Show this help\n";
        ss << "  quit - Exit console\n";
        return ss.str();
    }

    std::vector<std::string> split(const std::string& s) {
        std::vector<std::string> parts;
        std::istringstream ss(s);
        std::string part;
        while (ss >> part) parts.push_back(part);
        return parts;
    }

    struct Command {
        std::string help;
        CommandHandler handler;
    };

    std::map<std::string, Command> commands_;
};

}  // namespace debug
}  // namespace titans
