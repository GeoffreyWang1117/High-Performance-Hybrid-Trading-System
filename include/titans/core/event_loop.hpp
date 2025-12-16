/**
 * @file event_loop.hpp
 * @brief High-Performance Event Loop based on epoll/io_uring
 *
 * The main event loop that drives the trading system.
 * Handles I/O events, timers, and signal processing with
 * microsecond-level precision.
 */

#pragma once

#include "types.hpp"
#include "event_bus.hpp"

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>

#include <functional>
#include <unordered_map>
#include <queue>
#include <vector>
#include <memory>
#include <atomic>
#include <thread>

namespace titans {

/**
 * @brief File descriptor event handler
 */
struct FdHandler {
    int fd;
    uint32_t events;  // EPOLLIN, EPOLLOUT, etc.
    std::function<void(uint32_t events)> callback;
};

/**
 * @brief Timer entry for timer wheel
 */
struct TimerEntry {
    uint64_t id;
    Timestamp expiry;
    Duration interval;  // 0 for one-shot
    std::function<void()> callback;
    bool cancelled;

    bool operator>(const TimerEntry& other) const {
        return expiry > other.expiry;
    }
};

/**
 * @brief High-resolution timer using timerfd
 */
class Timer {
public:
    Timer() : fd_(-1), armed_(false) {
        fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (fd_ < 0) {
            throw std::runtime_error("Failed to create timerfd");
        }
    }

    ~Timer() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    int fd() const { return fd_; }

    void arm(Duration timeout_ns, Duration interval_ns = 0) {
        struct itimerspec its{};

        its.it_value.tv_sec = timeout_ns / 1000000000LL;
        its.it_value.tv_nsec = timeout_ns % 1000000000LL;

        if (interval_ns > 0) {
            its.it_interval.tv_sec = interval_ns / 1000000000LL;
            its.it_interval.tv_nsec = interval_ns % 1000000000LL;
        }

        if (timerfd_settime(fd_, 0, &its, nullptr) < 0) {
            throw std::runtime_error("Failed to arm timer");
        }
        armed_ = true;
    }

    void disarm() {
        struct itimerspec its{};
        timerfd_settime(fd_, 0, &its, nullptr);
        armed_ = false;
    }

    uint64_t read_expirations() {
        uint64_t expirations = 0;
        ssize_t n = ::read(fd_, &expirations, sizeof(expirations));
        if (n != sizeof(expirations)) {
            return 0;
        }
        return expirations;
    }

    bool is_armed() const { return armed_; }

private:
    int fd_;
    bool armed_;
};

/**
 * @brief Main Event Loop
 *
 * Multiplexes I/O, timers, and events using epoll.
 * Designed for single-threaded, run-to-completion model.
 */
class EventLoop {
public:
    static constexpr int MAX_EVENTS = 256;
    static constexpr Duration DEFAULT_TIMEOUT_MS = 100;

    EventLoop() : epoll_fd_(-1), event_fd_(-1), running_(false),
                  next_timer_id_(1) {
        // Create epoll instance
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            throw std::runtime_error("Failed to create epoll instance");
        }

        // Create eventfd for wakeup
        event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (event_fd_ < 0) {
            close(epoll_fd_);
            throw std::runtime_error("Failed to create eventfd");
        }

        // Add eventfd to epoll
        add_fd(event_fd_, EPOLLIN, [this](uint32_t) {
            uint64_t val;
            ::read(event_fd_, &val, sizeof(val));
        });
    }

    ~EventLoop() {
        stop();
        if (event_fd_ >= 0) close(event_fd_);
        if (epoll_fd_ >= 0) close(epoll_fd_);
    }

    // Non-copyable
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    /**
     * @brief Add a file descriptor to the event loop
     */
    void add_fd(int fd, uint32_t events, std::function<void(uint32_t)> callback) {
        struct epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            throw std::runtime_error("Failed to add fd to epoll");
        }

        fd_handlers_[fd] = {fd, events, std::move(callback)};
    }

    /**
     * @brief Modify events for a file descriptor
     */
    void modify_fd(int fd, uint32_t events) {
        auto it = fd_handlers_.find(fd);
        if (it == fd_handlers_.end()) return;

        struct epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;

        epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
        it->second.events = events;
    }

    /**
     * @brief Remove a file descriptor from the event loop
     */
    void remove_fd(int fd) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        fd_handlers_.erase(fd);
    }

    /**
     * @brief Schedule a one-shot timer
     * @return Timer ID
     */
    uint64_t schedule_timer(Duration delay_ns, std::function<void()> callback) {
        uint64_t id = next_timer_id_++;

        TimerEntry entry{
            .id = id,
            .expiry = now_ns() + delay_ns,
            .interval = 0,
            .callback = std::move(callback),
            .cancelled = false
        };

        timer_queue_.push(entry);
        timer_map_[id] = entry.expiry;

        update_timer_fd();
        return id;
    }

    /**
     * @brief Schedule a repeating timer
     * @return Timer ID
     */
    uint64_t schedule_interval(Duration interval_ns, std::function<void()> callback) {
        uint64_t id = next_timer_id_++;

        TimerEntry entry{
            .id = id,
            .expiry = now_ns() + interval_ns,
            .interval = interval_ns,
            .callback = std::move(callback),
            .cancelled = false
        };

        timer_queue_.push(entry);
        timer_map_[id] = entry.expiry;

        update_timer_fd();
        return id;
    }

    /**
     * @brief Cancel a timer
     */
    void cancel_timer(uint64_t timer_id) {
        timer_map_.erase(timer_id);
        // Timer will be skipped when popped from queue
    }

    /**
     * @brief Wake up the event loop from another thread
     */
    void wakeup() {
        uint64_t val = 1;
        ::write(event_fd_, &val, sizeof(val));
    }

    /**
     * @brief Post a callback to be executed on the event loop thread
     */
    void post(std::function<void()> callback) {
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_callbacks_.push_back(std::move(callback));
        }
        wakeup();
    }

    /**
     * @brief Run the event loop (blocking)
     */
    void run() {
        running_ = true;

        while (running_) {
            run_once();
        }
    }

    /**
     * @brief Run one iteration of the event loop
     * @return Number of events processed
     */
    int run_once(int timeout_ms = DEFAULT_TIMEOUT_MS) {
        // Process pending callbacks
        process_pending_callbacks();

        // Calculate timeout based on next timer
        int actual_timeout = timeout_ms;
        if (!timer_queue_.empty()) {
            Duration until_next = timer_queue_.top().expiry - now_ns();
            if (until_next < 0) {
                actual_timeout = 0;
            } else {
                int timer_ms = static_cast<int>(until_next / 1000000);
                actual_timeout = std::min(timeout_ms, timer_ms);
            }
        }

        // Wait for events
        std::array<struct epoll_event, MAX_EVENTS> events;
        int nfds = epoll_wait(epoll_fd_, events.data(), MAX_EVENTS, actual_timeout);

        if (nfds < 0) {
            if (errno == EINTR) return 0;
            return -1;
        }

        // Process I/O events
        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            auto it = fd_handlers_.find(fd);
            if (it != fd_handlers_.end()) {
                it->second.callback(events[i].events);
            }
        }

        // Process timers
        process_timers();

        return nfds;
    }

    /**
     * @brief Stop the event loop
     */
    void stop() {
        running_ = false;
        wakeup();
    }

    /**
     * @brief Check if running
     */
    bool is_running() const { return running_; }

    /**
     * @brief Get the epoll fd (for external monitoring)
     */
    int epoll_fd() const { return epoll_fd_; }

private:
    void process_timers() {
        Timestamp current = now_ns();

        while (!timer_queue_.empty()) {
            const TimerEntry& top = timer_queue_.top();

            if (top.expiry > current) {
                break;
            }

            TimerEntry entry = timer_queue_.top();
            timer_queue_.pop();

            // Check if cancelled
            auto it = timer_map_.find(entry.id);
            if (it == timer_map_.end()) {
                continue;  // Cancelled
            }

            timer_map_.erase(entry.id);

            // Execute callback
            if (entry.callback) {
                entry.callback();
            }

            // Reschedule if interval timer
            if (entry.interval > 0) {
                entry.expiry = current + entry.interval;
                timer_queue_.push(entry);
                timer_map_[entry.id] = entry.expiry;
            }
        }
    }

    void process_pending_callbacks() {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            callbacks.swap(pending_callbacks_);
        }

        for (auto& cb : callbacks) {
            cb();
        }
    }

    void update_timer_fd() {
        // Optional: Update timerfd to next expiry for precise wakeup
    }

    int epoll_fd_;
    int event_fd_;
    std::atomic<bool> running_;

    std::unordered_map<int, FdHandler> fd_handlers_;

    std::priority_queue<TimerEntry, std::vector<TimerEntry>,
                        std::greater<TimerEntry>> timer_queue_;
    std::unordered_map<uint64_t, Timestamp> timer_map_;
    std::atomic<uint64_t> next_timer_id_;

    std::mutex pending_mutex_;
    std::vector<std::function<void()>> pending_callbacks_;
};

/**
 * @brief Event loop with integrated EventBus
 */
class TradingEventLoop : public EventLoop {
public:
    explicit TradingEventLoop(EventBus& bus) : bus_(bus) {}

    /**
     * @brief Process events from both I/O and EventBus
     */
    int run_once_with_bus(int timeout_ms = DEFAULT_TIMEOUT_MS) {
        int io_events = run_once(timeout_ms);
        size_t bus_events = bus_.process_queued();
        return io_events + static_cast<int>(bus_events);
    }

    EventBus& bus() { return bus_; }

private:
    EventBus& bus_;
};

}  // namespace titans
