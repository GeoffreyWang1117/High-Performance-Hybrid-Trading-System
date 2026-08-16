/**
 * @file gpu_monitor.hpp
 * @brief GPU Metrics Collection and Monitoring
 *
 * Collects NVIDIA GPU metrics via nvidia-smi for experiment tracking:
 * - GPU utilization
 * - Memory usage
 * - Temperature
 * - Power consumption
 */

#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <fstream>
#include <sstream>
#include <array>
#include <cstdio>
#include <functional>

namespace titans {
namespace monitoring {

// ============================================================================
// GPU Metrics Structures
// ============================================================================

struct GPUMetrics {
    int gpu_id = 0;
    std::string name;

    // Utilization (0-100%)
    double gpu_utilization = 0;
    double memory_utilization = 0;

    // Memory (bytes)
    size_t memory_used = 0;
    size_t memory_total = 0;
    double memory_percent = 0;

    // Thermal
    double temperature_c = 0;

    // Power
    double power_draw_w = 0;
    double power_limit_w = 0;

    // Performance state
    std::string pstate;  // P0-P12

    // Timestamp
    std::chrono::steady_clock::time_point timestamp;

    double memory_used_gb() const { return memory_used / (1024.0 * 1024 * 1024); }
    double memory_total_gb() const { return memory_total / (1024.0 * 1024 * 1024); }
};

struct GPUSnapshot {
    std::chrono::steady_clock::time_point timestamp;
    std::vector<GPUMetrics> gpus;

    // Aggregate metrics
    double total_gpu_utilization() const {
        if (gpus.empty()) return 0;
        double sum = 0;
        for (const auto& g : gpus) sum += g.gpu_utilization;
        return sum / gpus.size();
    }

    double total_memory_used_gb() const {
        double sum = 0;
        for (const auto& g : gpus) sum += g.memory_used_gb();
        return sum;
    }

    double max_temperature() const {
        double max_temp = 0;
        for (const auto& g : gpus) max_temp = std::max(max_temp, g.temperature_c);
        return max_temp;
    }

    double total_power_draw() const {
        double sum = 0;
        for (const auto& g : gpus) sum += g.power_draw_w;
        return sum;
    }
};

// ============================================================================
// GPU Query (nvidia-smi wrapper)
// ============================================================================

class GPUQuery {
public:
    static bool is_nvidia_available() {
        return execute_command("nvidia-smi --version").find("NVIDIA") != std::string::npos;
    }

    static int gpu_count() {
        auto output = execute_command("nvidia-smi --query-gpu=count --format=csv,noheader,nounits");
        try {
            return std::stoi(output);
        } catch (...) {
            return 0;
        }
    }

    static GPUSnapshot query_all() {
        GPUSnapshot snapshot;
        snapshot.timestamp = std::chrono::steady_clock::now();

        // Query all metrics in one call for efficiency
        std::string cmd = "nvidia-smi --query-gpu="
            "index,name,utilization.gpu,utilization.memory,"
            "memory.used,memory.total,temperature.gpu,"
            "power.draw,power.limit,pstate"
            " --format=csv,noheader,nounits";

        auto output = execute_command(cmd);
        std::istringstream stream(output);
        std::string line;

        while (std::getline(stream, line)) {
            if (line.empty()) continue;

            GPUMetrics gpu;
            gpu.timestamp = snapshot.timestamp;

            std::istringstream line_stream(line);
            std::string token;
            int field = 0;

            while (std::getline(line_stream, token, ',')) {
                // Trim whitespace
                size_t start = token.find_first_not_of(" \t");
                size_t end = token.find_last_not_of(" \t");
                if (start != std::string::npos) {
                    token = token.substr(start, end - start + 1);
                }

                try {
                    switch (field) {
                        case 0: gpu.gpu_id = std::stoi(token); break;
                        case 1: gpu.name = token; break;
                        case 2: gpu.gpu_utilization = std::stod(token); break;
                        case 3: gpu.memory_utilization = std::stod(token); break;
                        case 4: gpu.memory_used = std::stoull(token) * 1024 * 1024; break;  // MiB to bytes
                        case 5: gpu.memory_total = std::stoull(token) * 1024 * 1024; break;
                        case 6: gpu.temperature_c = std::stod(token); break;
                        case 7: gpu.power_draw_w = std::stod(token); break;
                        case 8: gpu.power_limit_w = std::stod(token); break;
                        case 9: gpu.pstate = token; break;
                    }
                } catch (...) {
                    // Skip parse errors
                }
                ++field;
            }

            if (gpu.memory_total > 0) {
                gpu.memory_percent = 100.0 * gpu.memory_used / gpu.memory_total;
            }

            snapshot.gpus.push_back(gpu);
        }

        return snapshot;
    }

    static GPUMetrics query_gpu(int gpu_id) {
        auto snapshot = query_all();
        for (const auto& gpu : snapshot.gpus) {
            if (gpu.gpu_id == gpu_id) return gpu;
        }
        return {};
    }

private:
    static std::string execute_command(const std::string& cmd) {
        std::array<char, 4096> buffer;
        std::string result;

        // Silence "command not found" noise on GPU-less machines
        FILE* pipe = popen((cmd + " 2>/dev/null").c_str(), "r");
        if (!pipe) return "";

        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            result += buffer.data();
        }

        pclose(pipe);
        return result;
    }
};

// ============================================================================
// GPU Monitor (Background Collection)
// ============================================================================

class GPUMonitor {
public:
    using Callback = std::function<void(const GPUSnapshot&)>;

    explicit GPUMonitor(int interval_ms = 1000)
        : interval_ms_(interval_ms), running_(false) {}

    ~GPUMonitor() {
        stop();
    }

    void start() {
        if (running_.exchange(true)) return;

        monitor_thread_ = std::thread([this]() {
            while (running_) {
                auto snapshot = GPUQuery::query_all();

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    history_.push_back(snapshot);

                    // Keep last N snapshots
                    while (history_.size() > max_history_) {
                        history_.erase(history_.begin());
                    }
                }

                // Call callbacks
                for (const auto& cb : callbacks_) {
                    cb(snapshot);
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
            }
        });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
        }
    }

    void add_callback(Callback cb) {
        callbacks_.push_back(std::move(cb));
    }

    GPUSnapshot current() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return history_.empty() ? GPUSnapshot{} : history_.back();
    }

    std::vector<GPUSnapshot> history() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return history_;
    }

    // Statistics over collection period
    struct Statistics {
        double avg_gpu_utilization = 0;
        double max_gpu_utilization = 0;
        double avg_memory_gb = 0;
        double max_memory_gb = 0;
        double avg_temperature = 0;
        double max_temperature = 0;
        double avg_power_w = 0;
        double max_power_w = 0;
        size_t sample_count = 0;
    };

    Statistics compute_statistics() const {
        std::lock_guard<std::mutex> lock(mutex_);

        Statistics stats;
        if (history_.empty()) return stats;

        stats.sample_count = history_.size();

        for (const auto& snapshot : history_) {
            double util = snapshot.total_gpu_utilization();
            double mem = snapshot.total_memory_used_gb();
            double temp = snapshot.max_temperature();
            double power = snapshot.total_power_draw();

            stats.avg_gpu_utilization += util;
            stats.max_gpu_utilization = std::max(stats.max_gpu_utilization, util);

            stats.avg_memory_gb += mem;
            stats.max_memory_gb = std::max(stats.max_memory_gb, mem);

            stats.avg_temperature += temp;
            stats.max_temperature = std::max(stats.max_temperature, temp);

            stats.avg_power_w += power;
            stats.max_power_w = std::max(stats.max_power_w, power);
        }

        stats.avg_gpu_utilization /= stats.sample_count;
        stats.avg_memory_gb /= stats.sample_count;
        stats.avg_temperature /= stats.sample_count;
        stats.avg_power_w /= stats.sample_count;

        return stats;
    }

    void clear_history() {
        std::lock_guard<std::mutex> lock(mutex_);
        history_.clear();
    }

    void set_max_history(size_t max) {
        max_history_ = max;
    }

private:
    int interval_ms_;
    std::atomic<bool> running_;
    std::thread monitor_thread_;
    mutable std::mutex mutex_;
    std::vector<GPUSnapshot> history_;
    std::vector<Callback> callbacks_;
    size_t max_history_ = 3600;  // 1 hour at 1s interval
};

// ============================================================================
// GPU Metrics Logger
// ============================================================================

class GPUMetricsLogger {
public:
    explicit GPUMetricsLogger(const std::string& filepath)
        : filepath_(filepath) {
        // Write header
        std::ofstream file(filepath_);
        file << "timestamp_ms,gpu_id,utilization,memory_used_gb,memory_total_gb,"
             << "temperature_c,power_w,pstate\n";
    }

    void log(const GPUSnapshot& snapshot) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ofstream file(filepath_, std::ios::app);

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            snapshot.timestamp.time_since_epoch()
        ).count();

        for (const auto& gpu : snapshot.gpus) {
            file << ms << ","
                 << gpu.gpu_id << ","
                 << gpu.gpu_utilization << ","
                 << gpu.memory_used_gb() << ","
                 << gpu.memory_total_gb() << ","
                 << gpu.temperature_c << ","
                 << gpu.power_draw_w << ","
                 << gpu.pstate << "\n";
        }
    }

private:
    std::string filepath_;
    std::mutex mutex_;
};

// ============================================================================
// GPU Metrics Reporter
// ============================================================================

class GPUMetricsReporter {
public:
    static void print_snapshot(const GPUSnapshot& snapshot) {
        printf("\n╔═══════════════════════════════════════════════════════════════╗\n");
        printf("║                      GPU STATUS                               ║\n");
        printf("╠═══════════════════════════════════════════════════════════════╣\n");

        for (const auto& gpu : snapshot.gpus) {
            printf("║ GPU %d: %-50s ║\n", gpu.gpu_id, gpu.name.c_str());
            printf("║   Utilization: %5.1f%% GPU, %5.1f%% Memory                    ║\n",
                   gpu.gpu_utilization, gpu.memory_utilization);
            printf("║   Memory: %6.2f / %6.2f GB (%5.1f%%)                        ║\n",
                   gpu.memory_used_gb(), gpu.memory_total_gb(), gpu.memory_percent);
            printf("║   Temp: %5.1f°C  Power: %6.1f / %6.1f W  State: %-3s       ║\n",
                   gpu.temperature_c, gpu.power_draw_w, gpu.power_limit_w, gpu.pstate.c_str());
            printf("╠───────────────────────────────────────────────────────────────╣\n");
        }

        printf("║ Total: %5.1f%% util, %6.2f GB mem, %6.1f W power             ║\n",
               snapshot.total_gpu_utilization(),
               snapshot.total_memory_used_gb(),
               snapshot.total_power_draw());
        printf("╚═══════════════════════════════════════════════════════════════╝\n");
    }

    static void print_statistics(const GPUMonitor::Statistics& stats) {
        printf("\n╔═══════════════════════════════════════════════════════════════╗\n");
        printf("║                   GPU STATISTICS                              ║\n");
        printf("╠═══════════════════════════════════════════════════════════════╣\n");
        printf("║ Samples: %zu                                                  ║\n", stats.sample_count);
        printf("╠───────────────────────────────────────────────────────────────╣\n");
        printf("║ GPU Utilization: avg=%5.1f%%, max=%5.1f%%                      ║\n",
               stats.avg_gpu_utilization, stats.max_gpu_utilization);
        printf("║ Memory Usage:    avg=%5.2f GB, max=%5.2f GB                   ║\n",
               stats.avg_memory_gb, stats.max_memory_gb);
        printf("║ Temperature:     avg=%5.1f°C, max=%5.1f°C                     ║\n",
               stats.avg_temperature, stats.max_temperature);
        printf("║ Power Draw:      avg=%5.1f W, max=%5.1f W                     ║\n",
               stats.avg_power_w, stats.max_power_w);
        printf("╚═══════════════════════════════════════════════════════════════╝\n");
    }
};

}  // namespace monitoring
}  // namespace titans
