/**
 * @file cuda_utils.cuh
 * @brief CUDA Utilities and Common Functions
 *
 * Provides CUDA helper macros, error checking, and memory
 * management utilities for GPU-accelerated signal processing.
 */

#pragma once

#ifdef TITANS_CUDA_ENABLED

#include <cuda_runtime.h>
#include <cuda.h>
#include <cstdio>
#include <stdexcept>

namespace titans {
namespace cuda {

// ============================================================================
// Error Checking
// ============================================================================

#define CUDA_CHECK(call) do { \
    cudaError_t err = call; \
    if (err != cudaSuccess) { \
        fprintf(stderr, "CUDA error at %s:%d: %s\n", \
                __FILE__, __LINE__, cudaGetErrorString(err)); \
        throw std::runtime_error(cudaGetErrorString(err)); \
    } \
} while(0)

#define CUDA_CHECK_LAST() do { \
    cudaError_t err = cudaGetLastError(); \
    if (err != cudaSuccess) { \
        fprintf(stderr, "CUDA error at %s:%d: %s\n", \
                __FILE__, __LINE__, cudaGetErrorString(err)); \
        throw std::runtime_error(cudaGetErrorString(err)); \
    } \
} while(0)

// ============================================================================
// Device Management
// ============================================================================

/**
 * @brief Get device properties
 */
inline void print_device_info() {
    int device_count;
    CUDA_CHECK(cudaGetDeviceCount(&device_count));

    printf("CUDA Devices: %d\n", device_count);

    for (int i = 0; i < device_count; ++i) {
        cudaDeviceProp prop;
        CUDA_CHECK(cudaGetDeviceProperties(&prop, i));

        printf("Device %d: %s\n", i, prop.name);
        printf("  Compute capability: %d.%d\n", prop.major, prop.minor);
        printf("  Total memory: %.2f GB\n", prop.totalGlobalMem / 1e9);
        printf("  SM count: %d\n", prop.multiProcessorCount);
        printf("  Max threads/block: %d\n", prop.maxThreadsPerBlock);
        printf("  Max block dims: %d x %d x %d\n",
               prop.maxThreadsDim[0], prop.maxThreadsDim[1], prop.maxThreadsDim[2]);
        printf("  Max grid dims: %d x %d x %d\n",
               prop.maxGridSize[0], prop.maxGridSize[1], prop.maxGridSize[2]);
        printf("  Warp size: %d\n", prop.warpSize);
        printf("  Shared memory/block: %zu KB\n", prop.sharedMemPerBlock / 1024);
        printf("  L2 cache: %d KB\n", prop.l2CacheSize / 1024);
    }
}

/**
 * @brief RAII CUDA device selector
 */
class DeviceGuard {
public:
    explicit DeviceGuard(int device) {
        CUDA_CHECK(cudaGetDevice(&prev_device_));
        CUDA_CHECK(cudaSetDevice(device));
    }

    ~DeviceGuard() {
        cudaSetDevice(prev_device_);
    }

private:
    int prev_device_;
};

// ============================================================================
// Memory Management
// ============================================================================

/**
 * @brief Device memory wrapper with RAII
 */
template <typename T>
class DeviceArray {
public:
    DeviceArray() : data_(nullptr), size_(0) {}

    explicit DeviceArray(size_t size) : size_(size) {
        CUDA_CHECK(cudaMalloc(&data_, size * sizeof(T)));
    }

    ~DeviceArray() {
        if (data_) {
            cudaFree(data_);
        }
    }

    // Move only
    DeviceArray(DeviceArray&& other) noexcept
        : data_(other.data_), size_(other.size_) {
        other.data_ = nullptr;
        other.size_ = 0;
    }

    DeviceArray& operator=(DeviceArray&& other) noexcept {
        if (this != &other) {
            if (data_) cudaFree(data_);
            data_ = other.data_;
            size_ = other.size_;
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    // No copy
    DeviceArray(const DeviceArray&) = delete;
    DeviceArray& operator=(const DeviceArray&) = delete;

    /**
     * @brief Copy from host to device
     */
    void copy_from_host(const T* host_data, size_t count) {
        CUDA_CHECK(cudaMemcpy(data_, host_data,
                              count * sizeof(T), cudaMemcpyHostToDevice));
    }

    /**
     * @brief Copy from device to host
     */
    void copy_to_host(T* host_data, size_t count) const {
        CUDA_CHECK(cudaMemcpy(host_data, data_,
                              count * sizeof(T), cudaMemcpyDeviceToHost));
    }

    /**
     * @brief Async copy from host
     */
    void copy_from_host_async(const T* host_data, size_t count, cudaStream_t stream) {
        CUDA_CHECK(cudaMemcpyAsync(data_, host_data,
                                   count * sizeof(T), cudaMemcpyHostToDevice, stream));
    }

    /**
     * @brief Async copy to host
     */
    void copy_to_host_async(T* host_data, size_t count, cudaStream_t stream) const {
        CUDA_CHECK(cudaMemcpyAsync(host_data, data_,
                                   count * sizeof(T), cudaMemcpyDeviceToHost, stream));
    }

    /**
     * @brief Set to zero
     */
    void clear() {
        CUDA_CHECK(cudaMemset(data_, 0, size_ * sizeof(T)));
    }

    /**
     * @brief Resize (reallocates)
     */
    void resize(size_t new_size) {
        if (new_size == size_) return;
        if (data_) cudaFree(data_);
        size_ = new_size;
        CUDA_CHECK(cudaMalloc(&data_, size_ * sizeof(T)));
    }

    T* data() { return data_; }
    const T* data() const { return data_; }
    size_t size() const { return size_; }

private:
    T* data_;
    size_t size_;
};

/**
 * @brief Pinned (page-locked) host memory for faster transfers
 */
template <typename T>
class PinnedArray {
public:
    PinnedArray() : data_(nullptr), size_(0) {}

    explicit PinnedArray(size_t size) : size_(size) {
        CUDA_CHECK(cudaMallocHost(&data_, size * sizeof(T)));
    }

    ~PinnedArray() {
        if (data_) {
            cudaFreeHost(data_);
        }
    }

    PinnedArray(PinnedArray&& other) noexcept
        : data_(other.data_), size_(other.size_) {
        other.data_ = nullptr;
        other.size_ = 0;
    }

    PinnedArray& operator=(PinnedArray&& other) noexcept {
        if (this != &other) {
            if (data_) cudaFreeHost(data_);
            data_ = other.data_;
            size_ = other.size_;
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    T* data() { return data_; }
    const T* data() const { return data_; }
    size_t size() const { return size_; }

    T& operator[](size_t i) { return data_[i]; }
    const T& operator[](size_t i) const { return data_[i]; }

private:
    T* data_;
    size_t size_;
};

// ============================================================================
// Stream Management
// ============================================================================

/**
 * @brief CUDA stream wrapper
 */
class Stream {
public:
    Stream() {
        CUDA_CHECK(cudaStreamCreate(&stream_));
    }

    explicit Stream(unsigned int flags) {
        CUDA_CHECK(cudaStreamCreateWithFlags(&stream_, flags));
    }

    ~Stream() {
        if (stream_) {
            cudaStreamDestroy(stream_);
        }
    }

    Stream(Stream&& other) noexcept : stream_(other.stream_) {
        other.stream_ = nullptr;
    }

    void synchronize() {
        CUDA_CHECK(cudaStreamSynchronize(stream_));
    }

    bool is_complete() {
        cudaError_t result = cudaStreamQuery(stream_);
        if (result == cudaSuccess) return true;
        if (result == cudaErrorNotReady) return false;
        CUDA_CHECK(result);
        return false;
    }

    cudaStream_t get() { return stream_; }
    operator cudaStream_t() { return stream_; }

private:
    cudaStream_t stream_ = nullptr;
};

/**
 * @brief CUDA event for timing and synchronization
 */
class Event {
public:
    Event() {
        CUDA_CHECK(cudaEventCreate(&event_));
    }

    explicit Event(unsigned int flags) {
        CUDA_CHECK(cudaEventCreateWithFlags(&event_, flags));
    }

    ~Event() {
        if (event_) {
            cudaEventDestroy(event_);
        }
    }

    void record(cudaStream_t stream = 0) {
        CUDA_CHECK(cudaEventRecord(event_, stream));
    }

    void synchronize() {
        CUDA_CHECK(cudaEventSynchronize(event_));
    }

    static float elapsed_ms(const Event& start, const Event& end) {
        float ms;
        CUDA_CHECK(cudaEventElapsedTime(&ms, start.event_, end.event_));
        return ms;
    }

    cudaEvent_t get() { return event_; }

private:
    cudaEvent_t event_ = nullptr;
};

// ============================================================================
// Kernel Launch Helpers
// ============================================================================

/**
 * @brief Calculate optimal block size
 */
inline int optimal_block_size(void* kernel, int shared_mem = 0) {
    int min_grid_size, block_size;
    CUDA_CHECK(cudaOccupancyMaxPotentialBlockSize(
        &min_grid_size, &block_size, kernel, shared_mem));
    return block_size;
}

/**
 * @brief Calculate grid size for 1D launch
 */
inline int grid_size_1d(int n, int block_size) {
    return (n + block_size - 1) / block_size;
}

/**
 * @brief Calculate grid size for 2D launch
 */
inline dim3 grid_size_2d(int width, int height, dim3 block_size) {
    return dim3(
        (width + block_size.x - 1) / block_size.x,
        (height + block_size.y - 1) / block_size.y
    );
}

}  // namespace cuda
}  // namespace titans

#endif  // TITANS_CUDA_ENABLED
