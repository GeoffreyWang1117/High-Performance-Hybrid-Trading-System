/**
 * @file spsc_queue.hpp
 * @brief Single-Producer Single-Consumer Lock-Free Queue
 *
 * A high-performance, cache-optimized SPSC queue for inter-component
 * communication. Designed for microsecond-level latency.
 *
 * Features:
 * - Lock-free implementation using memory barriers
 * - Cache-line padding to prevent false sharing
 * - Bounded capacity for predictable memory usage
 * - Support for bulk operations
 */

#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <new>

namespace titans {

// Cache line size for padding
constexpr size_t CACHELINE_SIZE = 64;

template <typename T, size_t Capacity>
class SPSCQueue {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of 2");
    static_assert(std::is_trivially_copyable_v<T> || std::is_nothrow_move_constructible_v<T>,
                  "T must be trivially copyable or nothrow move constructible");

public:
    SPSCQueue() : head_(0), tail_(0) {
        // Initialize slots
        for (size_t i = 0; i < Capacity; ++i) {
            slots_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    ~SPSCQueue() = default;

    // Non-copyable, non-movable
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    SPSCQueue(SPSCQueue&&) = delete;
    SPSCQueue& operator=(SPSCQueue&&) = delete;

    /**
     * @brief Push an item to the queue (producer only)
     * @return true if successful, false if queue is full
     */
    template <typename... Args>
    bool try_push(Args&&... args) noexcept {
        const size_t pos = tail_.load(std::memory_order_relaxed);
        Slot& slot = slots_[pos & kMask];

        const size_t seq = slot.seq.load(std::memory_order_acquire);
        const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

        if (diff < 0) {
            // Queue is full
            return false;
        }

        // Construct in place
        new (&slot.data) T(std::forward<Args>(args)...);
        slot.seq.store(pos + 1, std::memory_order_release);
        tail_.store(pos + 1, std::memory_order_relaxed);
        return true;
    }

    /**
     * @brief Push with blocking retry (producer only)
     */
    template <typename... Args>
    void push(Args&&... args) noexcept {
        while (!try_push(std::forward<Args>(args)...)) {
            // Spin with pause instruction
            #if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
            #elif defined(__aarch64__)
            asm volatile("yield" ::: "memory");
            #endif
        }
    }

    /**
     * @brief Pop an item from the queue (consumer only)
     * @return optional containing the item if successful
     */
    std::optional<T> try_pop() noexcept {
        const size_t pos = head_.load(std::memory_order_relaxed);
        Slot& slot = slots_[pos & kMask];

        const size_t seq = slot.seq.load(std::memory_order_acquire);
        const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

        if (diff < 0) {
            // Queue is empty
            return std::nullopt;
        }

        T result = std::move(slot.data);
        slot.seq.store(pos + Capacity, std::memory_order_release);
        head_.store(pos + 1, std::memory_order_relaxed);
        return result;
    }

    /**
     * @brief Pop with output parameter (avoids optional overhead)
     * @return true if successful
     */
    bool try_pop(T& out) noexcept {
        const size_t pos = head_.load(std::memory_order_relaxed);
        Slot& slot = slots_[pos & kMask];

        const size_t seq = slot.seq.load(std::memory_order_acquire);
        const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

        if (diff < 0) {
            return false;
        }

        out = std::move(slot.data);
        slot.seq.store(pos + Capacity, std::memory_order_release);
        head_.store(pos + 1, std::memory_order_relaxed);
        return true;
    }

    /**
     * @brief Peek at the front item without removing it
     */
    const T* peek() const noexcept {
        const size_t pos = head_.load(std::memory_order_relaxed);
        const Slot& slot = slots_[pos & kMask];

        const size_t seq = slot.seq.load(std::memory_order_acquire);
        if (static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1) < 0) {
            return nullptr;
        }
        return &slot.data;
    }

    /**
     * @brief Bulk pop multiple items
     * @return Number of items popped
     */
    template <typename OutputIt>
    size_t pop_bulk(OutputIt out, size_t max_count) noexcept {
        size_t count = 0;
        T item;
        while (count < max_count && try_pop(item)) {
            *out++ = std::move(item);
            ++count;
        }
        return count;
    }

    /**
     * @brief Check if queue is empty (approximate)
     */
    bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) ==
               tail_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Get approximate size
     */
    size_t size() const noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_relaxed);
        return tail >= head ? tail - head : 0;
    }

    /**
     * @brief Get capacity
     */
    static constexpr size_t capacity() noexcept {
        return Capacity;
    }

private:
    static constexpr size_t kMask = Capacity - 1;

    struct alignas(CACHELINE_SIZE) Slot {
        std::atomic<size_t> seq;
        T data;
    };

    // Padding to prevent false sharing
    alignas(CACHELINE_SIZE) std::atomic<size_t> head_;
    char pad1_[CACHELINE_SIZE - sizeof(std::atomic<size_t>)];

    alignas(CACHELINE_SIZE) std::atomic<size_t> tail_;
    char pad2_[CACHELINE_SIZE - sizeof(std::atomic<size_t>)];

    alignas(CACHELINE_SIZE) std::array<Slot, Capacity> slots_;
};

/**
 * @brief MPSC (Multi-Producer Single-Consumer) Queue
 *
 * For cases where multiple threads need to push events.
 * Uses atomic CAS for producers.
 */
template <typename T, size_t Capacity>
class MPSCQueue {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of 2");

public:
    MPSCQueue() : head_(0), tail_(0) {
        for (size_t i = 0; i < Capacity; ++i) {
            slots_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Thread-safe push (multiple producers)
     */
    template <typename... Args>
    bool try_push(Args&&... args) noexcept {
        size_t pos;
        Slot* slot;

        while (true) {
            pos = tail_.load(std::memory_order_relaxed);
            slot = &slots_[pos & kMask];

            const size_t seq = slot->seq.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (diff < 0) {
                return false;  // Full
            }

            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1,
                        std::memory_order_relaxed)) {
                    break;
                }
            }
        }

        new (&slot->data) T(std::forward<Args>(args)...);
        slot->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Single consumer pop
     */
    bool try_pop(T& out) noexcept {
        const size_t pos = head_.load(std::memory_order_relaxed);
        Slot& slot = slots_[pos & kMask];

        const size_t seq = slot.seq.load(std::memory_order_acquire);
        const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

        if (diff < 0) {
            return false;
        }

        out = std::move(slot.data);
        slot.seq.store(pos + Capacity, std::memory_order_release);
        head_.store(pos + 1, std::memory_order_relaxed);
        return true;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) ==
               tail_.load(std::memory_order_relaxed);
    }

    size_t size() const noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_relaxed);
        return tail >= head ? tail - head : 0;
    }

private:
    static constexpr size_t kMask = Capacity - 1;

    struct alignas(CACHELINE_SIZE) Slot {
        std::atomic<size_t> seq;
        T data;
    };

    alignas(CACHELINE_SIZE) std::atomic<size_t> head_;
    char pad1_[CACHELINE_SIZE - sizeof(std::atomic<size_t>)];

    alignas(CACHELINE_SIZE) std::atomic<size_t> tail_;
    char pad2_[CACHELINE_SIZE - sizeof(std::atomic<size_t>)];

    alignas(CACHELINE_SIZE) std::array<Slot, Capacity> slots_;
};

}  // namespace titans
