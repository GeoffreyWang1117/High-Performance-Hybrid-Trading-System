/**
 * @file distributed_experiment.hpp
 * @brief Distributed Experiment Framework
 *
 * Supports running experiments across multiple machines:
 * - Coordinator-worker architecture
 * - Task distribution and load balancing
 * - Result aggregation
 * - Fault tolerance
 */

#pragma once

#include "experiment_harness.hpp"
#include "experiment_persistence.hpp"
#include "llm_interface.hpp"
#include "../core/http_client.hpp"
#include "../core/json.hpp"
#include "../core/gpu_monitor.hpp"
#include <thread>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <set>
#include <map>
#include <tuple>

namespace titans {
namespace context {

// ============================================================================
// Distributed Task Structures
// ============================================================================

enum class TaskStatus {
    Pending,
    Running,
    Completed,
    Failed,
    Cancelled
};

struct ExperimentTask {
    std::string task_id;
    ExperimentConfig config;
    TaskStatus status = TaskStatus::Pending;
    std::string assigned_worker;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point completed_at;
    int retry_count = 0;
    std::string error_message;
};

struct WorkerInfo {
    std::string worker_id;
    std::string host;
    int port;
    bool available = true;
    int current_tasks = 0;
    int max_concurrent_tasks = 1;
    std::chrono::steady_clock::time_point last_heartbeat;

    // Hardware info
    int gpu_count = 0;
    double gpu_memory_gb = 0;
    std::string llm_backend;
    std::vector<std::string> available_models;

    bool is_alive(int timeout_seconds = 30) const {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat).count();
        return elapsed < timeout_seconds;
    }
};

struct TaskResult {
    std::string task_id;
    bool success = false;
    ExperimentResult result;
    std::string error_message;
    double execution_time_s = 0;
    monitoring::GPUMonitor::Statistics gpu_stats;
};

// ============================================================================
// Task Queue
// ============================================================================

class TaskQueue {
public:
    void push(const ExperimentTask& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push(task);
        cv_.notify_one();
    }

    bool pop(ExperimentTask& task, int timeout_ms = 1000) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                         [this] { return !tasks_.empty() || shutdown_; })) {
            if (shutdown_ && tasks_.empty()) return false;
            task = tasks_.front();
            tasks_.pop();
            return true;
        }
        return false;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
        cv_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<ExperimentTask> tasks_;
    bool shutdown_ = false;
};

// ============================================================================
// Experiment Worker
// ============================================================================

class ExperimentWorker {
public:
    struct Config {
        std::string worker_id;
        std::string coordinator_host = "localhost";
        int coordinator_port = 9000;
        int max_concurrent = 1;
        int heartbeat_interval_s = 10;
    };

    explicit ExperimentWorker(const Config& config)
        : config_(config), running_(false) {}

    void start() {
        running_ = true;

        // Start GPU monitor
        gpu_monitor_.start();

        // Start heartbeat thread
        heartbeat_thread_ = std::thread([this]() {
            while (running_) {
                send_heartbeat();
                std::this_thread::sleep_for(
                    std::chrono::seconds(config_.heartbeat_interval_s)
                );
            }
        });

        // Start worker threads
        for (int i = 0; i < config_.max_concurrent; ++i) {
            worker_threads_.emplace_back([this]() {
                work_loop();
            });
        }

        printf("Worker %s started with %d threads\n",
               config_.worker_id.c_str(), config_.max_concurrent);
    }

    void stop() {
        running_ = false;
        task_queue_.shutdown();

        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
        }

        for (auto& t : worker_threads_) {
            if (t.joinable()) t.join();
        }

        gpu_monitor_.stop();
    }

    void submit_local(const ExperimentTask& task) {
        task_queue_.push(task);
    }

private:
    void work_loop() {
        ExperimentRunner runner;

        while (running_) {
            ExperimentTask task;
            if (!task_queue_.pop(task, 1000)) continue;

            printf("Worker %s executing task %s\n",
                   config_.worker_id.c_str(), task.task_id.c_str());

            auto start = std::chrono::steady_clock::now();
            gpu_monitor_.clear_history();

            TaskResult result;
            result.task_id = task.task_id;

            try {
                result.result = runner.run_experiment(task.config);
                result.success = true;
            } catch (const std::exception& e) {
                result.success = false;
                result.error_message = e.what();
            }

            auto end = std::chrono::steady_clock::now();
            result.execution_time_s = std::chrono::duration<double>(end - start).count();
            result.gpu_stats = gpu_monitor_.compute_statistics();

            send_result(result);
        }
    }

    void send_heartbeat() {
        auto snapshot = gpu_monitor_.current();

        json::Value heartbeat = json::object({
            {"worker_id", config_.worker_id},
            {"status", "alive"},
            {"current_tasks", static_cast<int>(task_queue_.size())},
            {"gpu_count", static_cast<int>(snapshot.gpus.size())},
            {"gpu_utilization", snapshot.total_gpu_utilization()},
            {"gpu_memory_gb", snapshot.total_memory_used_gb()}
        });

        http::HttpClient client;
        client.post(
            "http://" + config_.coordinator_host + ":" +
            std::to_string(config_.coordinator_port) + "/api/heartbeat",
            json::stringify(heartbeat),
            "application/json",
            5000
        );
    }

    void send_result(const TaskResult& result) {
        json::Value result_json = json::object({
            {"task_id", result.task_id},
            {"success", result.success},
            {"execution_time_s", result.execution_time_s},
            {"error_message", result.error_message},
            {"accuracy", result.result.impact_metrics.accuracy},
            {"gpu_avg_utilization", result.gpu_stats.avg_gpu_utilization},
            {"gpu_max_memory_gb", result.gpu_stats.max_memory_gb}
        });

        http::HttpClient client;
        client.post(
            "http://" + config_.coordinator_host + ":" +
            std::to_string(config_.coordinator_port) + "/api/result",
            json::stringify(result_json),
            "application/json",
            10000
        );
    }

    Config config_;
    std::atomic<bool> running_;
    TaskQueue task_queue_;
    std::thread heartbeat_thread_;
    std::vector<std::thread> worker_threads_;
    monitoring::GPUMonitor gpu_monitor_;
};

// ============================================================================
// Experiment Coordinator
// ============================================================================

class ExperimentCoordinator {
public:
    struct Config {
        int port;
        int task_timeout_s;
        int max_retries;

        Config() : port(9000), task_timeout_s(3600), max_retries(3) {}
    };

    explicit ExperimentCoordinator(Config config = Config())
        : config_(std::move(config)), running_(false) {}

    // Submit a batch of experiments
    std::string submit_experiment_batch(
        const std::string& batch_id,
        const std::vector<ExperimentConfig>& configs
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        for (size_t i = 0; i < configs.size(); ++i) {
            ExperimentTask task;
            task.task_id = batch_id + "_" + std::to_string(i);
            task.config = configs[i];
            task.config.experiment_id = task.task_id;
            task.status = TaskStatus::Pending;
            task.created_at = std::chrono::steady_clock::now();

            pending_tasks_.push(task);
            all_tasks_[task.task_id] = task;
        }

        printf("Submitted batch %s with %zu tasks\n", batch_id.c_str(), configs.size());
        return batch_id;
    }

    // Register a worker
    void register_worker(const WorkerInfo& info) {
        std::lock_guard<std::mutex> lock(mutex_);
        workers_[info.worker_id] = info;
        printf("Registered worker %s (%s:%d)\n",
               info.worker_id.c_str(), info.host.c_str(), info.port);
    }

    // Process heartbeat from worker
    void process_heartbeat(const std::string& worker_id, const json::Value& data) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (workers_.find(worker_id) != workers_.end()) {
            workers_[worker_id].last_heartbeat = std::chrono::steady_clock::now();
            workers_[worker_id].current_tasks = data["current_tasks"].as_int();
            workers_[worker_id].available = workers_[worker_id].current_tasks <
                                           workers_[worker_id].max_concurrent_tasks;
        }
    }

    // Process result from worker
    void process_result(const TaskResult& result) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (all_tasks_.find(result.task_id) != all_tasks_.end()) {
            auto& task = all_tasks_[result.task_id];

            if (result.success) {
                task.status = TaskStatus::Completed;
                task.completed_at = std::chrono::steady_clock::now();
                completed_results_[result.task_id] = result;
                printf("Task %s completed (accuracy: %.2f%%)\n",
                       result.task_id.c_str(), result.result.impact_metrics.accuracy * 100);
            } else {
                task.retry_count++;
                if (task.retry_count >= config_.max_retries) {
                    task.status = TaskStatus::Failed;
                    task.error_message = result.error_message;
                    printf("Task %s failed after %d retries: %s\n",
                           result.task_id.c_str(), task.retry_count, result.error_message.c_str());
                } else {
                    task.status = TaskStatus::Pending;
                    pending_tasks_.push(task);
                    printf("Task %s retry %d/%d\n",
                           result.task_id.c_str(), task.retry_count, config_.max_retries);
                }
            }

            // Free up worker
            if (!task.assigned_worker.empty() &&
                workers_.find(task.assigned_worker) != workers_.end()) {
                workers_[task.assigned_worker].current_tasks--;
                workers_[task.assigned_worker].available = true;
            }
        }
    }

    // Assign tasks to available workers
    void dispatch_tasks() {
        std::lock_guard<std::mutex> lock(mutex_);

        for (auto& [worker_id, worker] : workers_) {
            if (!worker.is_alive() || !worker.available) continue;
            if (pending_tasks_.empty()) break;

            auto task = pending_tasks_.front();
            pending_tasks_.pop();

            task.status = TaskStatus::Running;
            task.assigned_worker = worker_id;
            task.started_at = std::chrono::steady_clock::now();
            all_tasks_[task.task_id] = task;

            worker.current_tasks++;
            worker.available = worker.current_tasks < worker.max_concurrent_tasks;

            // Send task to worker
            send_task_to_worker(worker, task);
        }
    }

    // Get batch status
    struct BatchStatus {
        std::string batch_id;
        size_t total_tasks = 0;
        size_t pending = 0;
        size_t running = 0;
        size_t completed = 0;
        size_t failed = 0;
        double progress_percent = 0;
    };

    BatchStatus get_batch_status(const std::string& batch_id) const {
        std::lock_guard<std::mutex> lock(mutex_);

        BatchStatus status;
        status.batch_id = batch_id;

        for (const auto& [task_id, task] : all_tasks_) {
            if (task_id.find(batch_id) != 0) continue;

            status.total_tasks++;
            switch (task.status) {
                case TaskStatus::Pending: status.pending++; break;
                case TaskStatus::Running: status.running++; break;
                case TaskStatus::Completed: status.completed++; break;
                case TaskStatus::Failed: status.failed++; break;
                default: break;
            }
        }

        if (status.total_tasks > 0) {
            status.progress_percent = 100.0 * (status.completed + status.failed) / status.total_tasks;
        }

        return status;
    }

    // Get all results for a batch
    std::vector<ExperimentResult> get_batch_results(const std::string& batch_id) const {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<ExperimentResult> results;
        for (const auto& [task_id, result] : completed_results_) {
            if (task_id.find(batch_id) == 0) {
                results.push_back(result.result);
            }
        }
        return results;
    }

    // Print status
    void print_status() const {
        std::lock_guard<std::mutex> lock(mutex_);

        printf("\n╔═══════════════════════════════════════════════════════════════╗\n");
        printf("║                 COORDINATOR STATUS                            ║\n");
        printf("╠═══════════════════════════════════════════════════════════════╣\n");
        printf("║ Workers: %zu active                                           ║\n", workers_.size());

        for (const auto& [id, w] : workers_) {
            printf("║   %-10s %s tasks=%d/%d gpu=%d                        ║\n",
                   id.c_str(),
                   w.is_alive() ? "✓" : "✗",
                   w.current_tasks, w.max_concurrent_tasks, w.gpu_count);
        }

        printf("╠───────────────────────────────────────────────────────────────╣\n");
        printf("║ Tasks: %zu pending, %zu total                                 ║\n",
               pending_tasks_.size(), all_tasks_.size());

        size_t completed = 0, failed = 0, running = 0;
        for (const auto& [_, t] : all_tasks_) {
            if (t.status == TaskStatus::Completed) completed++;
            else if (t.status == TaskStatus::Failed) failed++;
            else if (t.status == TaskStatus::Running) running++;
        }
        printf("║ Status: %zu running, %zu completed, %zu failed               ║\n",
               running, completed, failed);
        printf("╚═══════════════════════════════════════════════════════════════╝\n");
    }

private:
    void send_task_to_worker(const WorkerInfo& worker, const ExperimentTask& task) {
        json::Value task_json = json::object({
            {"task_id", task.task_id},
            {"config", json::object({
                {"experiment_id", task.config.experiment_id},
                {"num_events", static_cast<int>(task.config.num_events)},
                {"num_entities", static_cast<int>(task.config.num_entities)},
                {"contamination_rate", task.config.contamination_rate},
                {"method", static_cast<int>(task.config.method)},
                {"seed", static_cast<double>(task.config.seed)}
            })}
        });

        http::HttpClient client;
        client.post(
            "http://" + worker.host + ":" + std::to_string(worker.port) + "/api/task",
            json::stringify(task_json),
            "application/json",
            10000
        );
    }

    Config config_;
    std::atomic<bool> running_;
    mutable std::mutex mutex_;

    std::unordered_map<std::string, WorkerInfo> workers_;
    std::queue<ExperimentTask> pending_tasks_;
    std::unordered_map<std::string, ExperimentTask> all_tasks_;
    std::unordered_map<std::string, TaskResult> completed_results_;
};

// ============================================================================
// Local Distributed Simulation (for testing)
// ============================================================================

class LocalDistributedRunner {
public:
    LocalDistributedRunner(int num_workers = 4)
        : num_workers_(num_workers) {}

    std::vector<ExperimentResult> run_distributed(
        const std::vector<ExperimentConfig>& configs
    ) {
        printf("\n╔═══════════════════════════════════════════════════════════════╗\n");
        printf("║           LOCAL DISTRIBUTED EXPERIMENT                        ║\n");
        printf("║           %d workers, %zu tasks                               ║\n",
               num_workers_, configs.size());
        printf("╚═══════════════════════════════════════════════════════════════╝\n\n");

        std::vector<ExperimentResult> results(configs.size());
        std::atomic<size_t> task_index{0};
        std::atomic<size_t> completed{0};
        std::mutex print_mutex;

        // Start GPU monitor
        monitoring::GPUMonitor gpu_monitor(1000);
        if (monitoring::GPUQuery::is_nvidia_available()) {
            gpu_monitor.start();
        }

        auto start_time = std::chrono::steady_clock::now();

        // Worker threads
        std::vector<std::thread> workers;
        for (int w = 0; w < num_workers_; ++w) {
            workers.emplace_back([&, w]() {
                ExperimentRunner runner;

                while (true) {
                    size_t idx = task_index.fetch_add(1);
                    if (idx >= configs.size()) break;

                    auto result = runner.run_experiment(configs[idx]);
                    results[idx] = result;

                    size_t done = ++completed;
                    {
                        std::lock_guard<std::mutex> lock(print_mutex);
                        printf("\r  Progress: %zu/%zu (%.1f%%) - Worker %d completed %s",
                               done, configs.size(),
                               100.0 * done / configs.size(),
                               w, configs[idx].experiment_id.c_str());
                        fflush(stdout);
                    }
                }
            });
        }

        // Wait for completion
        for (auto& t : workers) {
            t.join();
        }

        auto end_time = std::chrono::steady_clock::now();
        double elapsed_s = std::chrono::duration<double>(end_time - start_time).count();

        printf("\n\n");

        // Print GPU stats if available
        if (monitoring::GPUQuery::is_nvidia_available()) {
            gpu_monitor.stop();
            auto stats = gpu_monitor.compute_statistics();
            monitoring::GPUMetricsReporter::print_statistics(stats);
        }

        printf("\nCompleted %zu experiments in %.1f seconds (%.2f exp/s)\n",
               configs.size(), elapsed_s, configs.size() / elapsed_s);

        return results;
    }

private:
    int num_workers_;
};

// ============================================================================
// Experiment Grid Generator
// ============================================================================

class ExperimentGridGenerator {
public:
    struct GridConfig {
        std::vector<ContextMethod> methods;
        std::vector<double> contamination_rates;
        std::vector<size_t> event_counts;
        int seeds_per_config = 5;  // For statistical significance
        size_t num_entities = 50;
    };

    static std::vector<ExperimentConfig> generate_grid(const GridConfig& grid) {
        std::vector<ExperimentConfig> configs;

        int config_id = 0;
        for (auto method : grid.methods) {
            for (double rate : grid.contamination_rates) {
                for (size_t events : grid.event_counts) {
                    for (int seed = 0; seed < grid.seeds_per_config; ++seed) {
                        ExperimentConfig config;
                        config.experiment_id = "grid_" + std::to_string(config_id++);
                        config.method = method;
                        config.contamination_rate = rate;
                        config.num_events = events;
                        config.num_entities = grid.num_entities;
                        config.seed = 42 + seed * 1000;

                        configs.push_back(config);
                    }
                }
            }
        }

        return configs;
    }

    static GridConfig standard_grid() {
        return GridConfig{
            .methods = {
                ContextMethod::NoHistory,
                ContextMethod::FullHistory,
                ContextMethod::FixedWindow,
                ContextMethod::TimeFilter,
                ContextMethod::VersionedContext
            },
            .contamination_rates = {0.05, 0.10, 0.15, 0.20, 0.30},
            .event_counts = {1000, 5000, 10000},
            .seeds_per_config = 5,
            .num_entities = 50
        };
    }

    static GridConfig quick_grid() {
        return GridConfig{
            .methods = {
                ContextMethod::NoHistory,
                ContextMethod::VersionedContext
            },
            .contamination_rates = {0.10, 0.20},
            .event_counts = {1000},
            .seeds_per_config = 3,
            .num_entities = 20
        };
    }
};

// ============================================================================
// Result Aggregator
// ============================================================================

class ResultAggregator {
public:
    struct AggregatedResult {
        std::string method;
        double contamination_rate;
        size_t event_count;

        // Aggregated metrics (mean ± std)
        double accuracy_mean;
        double accuracy_std;
        double accuracy_clean_mean;
        double accuracy_contaminated_mean;
        double accuracy_delta_mean;

        int sample_count;
    };

    static std::vector<AggregatedResult> aggregate(
        const std::vector<ExperimentResult>& results
    ) {
        // Group by (method, contamination_rate, event_count)
        std::map<std::tuple<std::string, double, size_t>, std::vector<const ExperimentResult*>> groups;

        for (const auto& r : results) {
            auto key = std::make_tuple(r.method_name, r.contamination_rate, r.num_events);
            groups[key].push_back(&r);
        }

        std::vector<AggregatedResult> aggregated;

        for (const auto& [key, group] : groups) {
            AggregatedResult agg;
            agg.method = std::get<0>(key);
            agg.contamination_rate = std::get<1>(key);
            agg.event_count = std::get<2>(key);
            agg.sample_count = group.size();

            // Calculate means
            double sum_acc = 0, sum_clean = 0, sum_contam = 0, sum_delta = 0;
            for (const auto* r : group) {
                sum_acc += r->impact_metrics.accuracy;
                sum_clean += r->impact_metrics.accuracy_clean;
                sum_contam += r->impact_metrics.accuracy_contaminated;
                sum_delta += r->impact_metrics.accuracy_delta;
            }

            agg.accuracy_mean = sum_acc / group.size();
            agg.accuracy_clean_mean = sum_clean / group.size();
            agg.accuracy_contaminated_mean = sum_contam / group.size();
            agg.accuracy_delta_mean = sum_delta / group.size();

            // Calculate std
            double sum_sq = 0;
            for (const auto* r : group) {
                double diff = r->impact_metrics.accuracy - agg.accuracy_mean;
                sum_sq += diff * diff;
            }
            agg.accuracy_std = std::sqrt(sum_sq / group.size());

            aggregated.push_back(agg);
        }

        return aggregated;
    }

    static void print_aggregated(const std::vector<AggregatedResult>& results) {
        printf("\n╔═══════════════════════════════════════════════════════════════════════╗\n");
        printf("║                    AGGREGATED RESULTS                                 ║\n");
        printf("╠═══════════════════════════════════════════════════════════════════════╣\n");
        printf("║ Method            │ Rate │ Events │ Accuracy (mean±std) │ n │ Δ Clean ║\n");
        printf("╠═══════════════════╪══════╪════════╪═════════════════════╪═══╪═════════╣\n");

        for (const auto& r : results) {
            printf("║ %-17s │ %4.0f%% │ %6zu │ %5.1f%% ± %4.1f%%      │ %d │ %+5.1f%% ║\n",
                   r.method.c_str(),
                   r.contamination_rate * 100,
                   r.event_count,
                   r.accuracy_mean * 100,
                   r.accuracy_std * 100,
                   r.sample_count,
                   r.accuracy_delta_mean * 100);
        }

        printf("╚═══════════════════════════════════════════════════════════════════════╝\n");
    }
};

}  // namespace context
}  // namespace titans
