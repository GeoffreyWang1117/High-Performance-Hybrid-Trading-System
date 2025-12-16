/**
 * @file memory_pool.hpp
 * @brief High-Performance Memory Pool for Zero-Allocation Runtime
 *
 * Provides pre-allocated memory blocks to avoid runtime allocations,
 * which can cause unpredictable latency spikes.
 *
 * Features:
 * - Object pools for fixed-size allocations
 * - Arena allocator for variable-size allocations
 * - Thread-local pools for lock-free per-thread allocation
 * - Memory alignment for cache efficiency
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <array>
#include <atomic>
#include <cassert>
#include <new>

namespace titans {

/**
 * @brief Fixed-size object pool with O(1) allocation/deallocation
 *
 * Pre-allocates a fixed number of objects of type T.
 * Ideal for Order, Trade, and Event objects.
 */
template <typename T, size_t BlockSize = 4096>
class ObjectPool {
public:
    explicit ObjectPool(size_t initial_capacity = BlockSize)
        : capacity_(0), allocated_(0) {
        reserve(initial_capacity);
    }

    ~ObjectPool() {
        for (auto* block : blocks_) {
            ::operator delete(block, std::align_val_t{alignof(T)});
        }
    }

    // Non-copyable
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    /**
     * @brief Allocate a new object
     * @return Pointer to uninitialized memory, or nullptr if pool exhausted
     */
    T* allocate() noexcept {
        if (free_list_ == nullptr) {
            if (!grow()) {
                return nullptr;
            }
        }

        Node* node = free_list_;
        free_list_ = node->next;
        ++allocated_;
        return reinterpret_cast<T*>(node);
    }

    /**
     * @brief Allocate and construct with arguments
     */
    template <typename... Args>
    T* create(Args&&... args) {
        T* ptr = allocate();
        if (ptr) {
            new (ptr) T(std::forward<Args>(args)...);
        }
        return ptr;
    }

    /**
     * @brief Return object to pool
     */
    void deallocate(T* ptr) noexcept {
        if (ptr == nullptr) return;

        ptr->~T();
        Node* node = reinterpret_cast<Node*>(ptr);
        node->next = free_list_;
        free_list_ = node;
        --allocated_;
    }

    /**
     * @brief Pre-allocate additional capacity
     */
    void reserve(size_t count) {
        while (capacity_ < count) {
            grow();
        }
    }

    size_t capacity() const noexcept { return capacity_; }
    size_t allocated() const noexcept { return allocated_; }
    size_t available() const noexcept { return capacity_ - allocated_; }

private:
    struct Node {
        Node* next;
    };

    static_assert(sizeof(T) >= sizeof(Node), "T must be at least pointer-sized");

    bool grow() {
        constexpr size_t obj_size = sizeof(T) > sizeof(Node) ? sizeof(T) : sizeof(Node);
        constexpr size_t alignment = alignof(T) > alignof(Node) ? alignof(T) : alignof(Node);

        void* block = ::operator new(obj_size * BlockSize, std::align_val_t{alignment});
        if (!block) return false;

        blocks_.push_back(block);

        // Link all objects in the new block to free list
        char* ptr = static_cast<char*>(block);
        for (size_t i = 0; i < BlockSize; ++i) {
            Node* node = reinterpret_cast<Node*>(ptr + i * obj_size);
            node->next = free_list_;
            free_list_ = node;
        }

        capacity_ += BlockSize;
        return true;
    }

    std::vector<void*> blocks_;
    Node* free_list_ = nullptr;
    size_t capacity_;
    size_t allocated_;
};

/**
 * @brief Arena Allocator for sequential allocations
 *
 * Fast bump-pointer allocation for objects that live together
 * and are freed together (e.g., per-message processing).
 */
class Arena {
public:
    static constexpr size_t DEFAULT_BLOCK_SIZE = 64 * 1024;  // 64KB

    explicit Arena(size_t block_size = DEFAULT_BLOCK_SIZE)
        : block_size_(block_size), current_(nullptr),
          current_end_(nullptr), total_allocated_(0) {}

    ~Arena() {
        for (auto& block : blocks_) {
            ::operator delete(block.data);
        }
    }

    // Non-copyable
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    /**
     * @brief Allocate memory with alignment
     */
    void* allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        // Align current pointer
        size_t space = current_end_ - current_;
        void* ptr = current_;

        if (std::align(alignment, size, ptr, space)) {
            current_ = static_cast<char*>(ptr) + size;
            total_allocated_ += size;
            return ptr;
        }

        // Need new block
        return allocate_from_new_block(size, alignment);
    }

    /**
     * @brief Allocate and construct object
     */
    template <typename T, typename... Args>
    T* create(Args&&... args) {
        void* ptr = allocate(sizeof(T), alignof(T));
        return new (ptr) T(std::forward<Args>(args)...);
    }

    /**
     * @brief Allocate array
     */
    template <typename T>
    T* allocate_array(size_t count) {
        return static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
    }

    /**
     * @brief Reset arena (reuse memory without freeing)
     */
    void reset() {
        if (!blocks_.empty()) {
            current_ = static_cast<char*>(blocks_[0].data);
            current_end_ = current_ + blocks_[0].size;
            current_block_idx_ = 0;
        }
        total_allocated_ = 0;
    }

    /**
     * @brief Clear and free all memory
     */
    void clear() {
        for (auto& block : blocks_) {
            ::operator delete(block.data);
        }
        blocks_.clear();
        current_ = nullptr;
        current_end_ = nullptr;
        current_block_idx_ = 0;
        total_allocated_ = 0;
    }

    size_t total_allocated() const noexcept { return total_allocated_; }

private:
    struct Block {
        void* data;
        size_t size;
    };

    void* allocate_from_new_block(size_t size, size_t alignment) {
        // Check if we can reuse an existing block
        if (current_block_idx_ + 1 < blocks_.size()) {
            ++current_block_idx_;
            current_ = static_cast<char*>(blocks_[current_block_idx_].data);
            current_end_ = current_ + blocks_[current_block_idx_].size;
            return allocate(size, alignment);
        }

        // Allocate new block
        size_t new_block_size = std::max(block_size_, size + alignment);
        void* block = ::operator new(new_block_size);

        blocks_.push_back({block, new_block_size});
        current_block_idx_ = blocks_.size() - 1;
        current_ = static_cast<char*>(block);
        current_end_ = current_ + new_block_size;

        return allocate(size, alignment);
    }

    size_t block_size_;
    char* current_;
    char* current_end_;
    size_t current_block_idx_ = 0;
    size_t total_allocated_;
    std::vector<Block> blocks_;
};

/**
 * @brief Scoped Arena for RAII-based lifetime management
 */
class ScopedArena {
public:
    explicit ScopedArena(Arena& arena) : arena_(arena), mark_(arena.total_allocated()) {}

    ~ScopedArena() {
        // Note: Arena doesn't support partial deallocation,
        // but this can be used for tracking
    }

    Arena& arena() { return arena_; }

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        return arena_.create<T>(std::forward<Args>(args)...);
    }

private:
    Arena& arena_;
    size_t mark_;
};

/**
 * @brief Thread-local object pool
 *
 * Provides lock-free allocation for per-thread use.
 */
template <typename T, size_t LocalCapacity = 256>
class ThreadLocalPool {
public:
    static T* allocate() {
        auto& pool = get_local_pool();
        return pool.allocate();
    }

    template <typename... Args>
    static T* create(Args&&... args) {
        auto& pool = get_local_pool();
        return pool.create(std::forward<Args>(args)...);
    }

    static void deallocate(T* ptr) {
        auto& pool = get_local_pool();
        pool.deallocate(ptr);
    }

private:
    static ObjectPool<T, LocalCapacity>& get_local_pool() {
        thread_local ObjectPool<T, LocalCapacity> pool;
        return pool;
    }
};

/**
 * @brief Pre-allocated ring buffer for fixed-size messages
 */
template <typename T, size_t Capacity>
class RingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

public:
    RingBuffer() : head_(0), tail_(0) {}

    bool push(const T& item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (tail + 1) & kMask;

        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;  // Full
        }

        buffer_[tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const size_t head = head_.load(std::memory_order_relaxed);

        if (head == tail_.load(std::memory_order_acquire)) {
            return false;  // Empty
        }

        item = buffer_[head];
        head_.store((head + 1) & kMask, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_relaxed) ==
               tail_.load(std::memory_order_relaxed);
    }

    size_t size() const {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_relaxed);
        return (tail - head) & kMask;
    }

private:
    static constexpr size_t kMask = Capacity - 1;

    alignas(64) std::array<T, Capacity> buffer_;
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
};

}  // namespace titans
