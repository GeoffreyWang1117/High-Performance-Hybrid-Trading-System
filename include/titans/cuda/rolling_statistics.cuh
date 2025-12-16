/**
 * @file rolling_statistics.cuh
 * @brief GPU-Accelerated Rolling Statistics for Alpha Factors
 *
 * Implements high-performance rolling window calculations using CUDA:
 * - Rolling mean, variance, standard deviation
 * - Rolling correlation and covariance
 * - Exponential moving averages (EMA)
 * - Rolling min/max and percentiles
 *
 * These are fundamental building blocks for alpha factor computation.
 */

#pragma once

#ifdef TITANS_CUDA_ENABLED

#include "cuda_utils.cuh"
#include <cmath>

namespace titans {
namespace cuda {

// ============================================================================
// Rolling Statistics Kernels
// ============================================================================

/**
 * @brief Rolling mean kernel
 */
__global__ void rolling_mean_kernel(
    const float* __restrict__ input,
    float* __restrict__ output,
    int n,
    int window_size
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= n) return;

    if (idx < window_size - 1) {
        output[idx] = NAN;
        return;
    }

    float sum = 0.0f;
    for (int i = idx - window_size + 1; i <= idx; ++i) {
        sum += input[i];
    }
    output[idx] = sum / window_size;
}

/**
 * @brief Rolling variance kernel (Welford's online algorithm)
 */
__global__ void rolling_variance_kernel(
    const float* __restrict__ input,
    float* __restrict__ output,
    int n,
    int window_size
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= n) return;

    if (idx < window_size - 1) {
        output[idx] = NAN;
        return;
    }

    // Two-pass for numerical stability
    float mean = 0.0f;
    for (int i = idx - window_size + 1; i <= idx; ++i) {
        mean += input[i];
    }
    mean /= window_size;

    float var = 0.0f;
    for (int i = idx - window_size + 1; i <= idx; ++i) {
        float diff = input[i] - mean;
        var += diff * diff;
    }
    output[idx] = var / (window_size - 1);  // Sample variance
}

/**
 * @brief Rolling standard deviation kernel
 */
__global__ void rolling_std_kernel(
    const float* __restrict__ input,
    float* __restrict__ output,
    int n,
    int window_size
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= n) return;

    if (idx < window_size - 1) {
        output[idx] = NAN;
        return;
    }

    float mean = 0.0f;
    for (int i = idx - window_size + 1; i <= idx; ++i) {
        mean += input[i];
    }
    mean /= window_size;

    float var = 0.0f;
    for (int i = idx - window_size + 1; i <= idx; ++i) {
        float diff = input[i] - mean;
        var += diff * diff;
    }

    output[idx] = sqrtf(var / (window_size - 1));
}

/**
 * @brief Exponential moving average kernel
 */
__global__ void ema_kernel(
    const float* __restrict__ input,
    float* __restrict__ output,
    int n,
    float alpha
) {
    // EMA is inherently sequential, but we can parallelize multiple series
    // This kernel handles a single series

    if (blockIdx.x > 0 || threadIdx.x > 0) return;

    output[0] = input[0];
    for (int i = 1; i < n; ++i) {
        output[i] = alpha * input[i] + (1.0f - alpha) * output[i - 1];
    }
}

/**
 * @brief Batch EMA kernel for multiple series
 */
__global__ void batch_ema_kernel(
    const float* __restrict__ input,  // [num_series, n]
    float* __restrict__ output,
    int num_series,
    int n,
    float alpha
) {
    int series_idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (series_idx >= num_series) return;

    const float* in = input + series_idx * n;
    float* out = output + series_idx * n;

    out[0] = in[0];
    for (int i = 1; i < n; ++i) {
        out[i] = alpha * in[i] + (1.0f - alpha) * out[i - 1];
    }
}

/**
 * @brief Rolling correlation kernel
 */
__global__ void rolling_correlation_kernel(
    const float* __restrict__ x,
    const float* __restrict__ y,
    float* __restrict__ output,
    int n,
    int window_size
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= n) return;

    if (idx < window_size - 1) {
        output[idx] = NAN;
        return;
    }

    // Calculate means
    float mean_x = 0.0f, mean_y = 0.0f;
    for (int i = idx - window_size + 1; i <= idx; ++i) {
        mean_x += x[i];
        mean_y += y[i];
    }
    mean_x /= window_size;
    mean_y /= window_size;

    // Calculate correlation
    float cov = 0.0f, var_x = 0.0f, var_y = 0.0f;
    for (int i = idx - window_size + 1; i <= idx; ++i) {
        float dx = x[i] - mean_x;
        float dy = y[i] - mean_y;
        cov += dx * dy;
        var_x += dx * dx;
        var_y += dy * dy;
    }

    float denom = sqrtf(var_x * var_y);
    output[idx] = (denom > 1e-10f) ? cov / denom : 0.0f;
}

/**
 * @brief Rolling min kernel
 */
__global__ void rolling_min_kernel(
    const float* __restrict__ input,
    float* __restrict__ output,
    int n,
    int window_size
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= n) return;

    if (idx < window_size - 1) {
        output[idx] = NAN;
        return;
    }

    float min_val = input[idx - window_size + 1];
    for (int i = idx - window_size + 2; i <= idx; ++i) {
        min_val = fminf(min_val, input[i]);
    }
    output[idx] = min_val;
}

/**
 * @brief Rolling max kernel
 */
__global__ void rolling_max_kernel(
    const float* __restrict__ input,
    float* __restrict__ output,
    int n,
    int window_size
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= n) return;

    if (idx < window_size - 1) {
        output[idx] = NAN;
        return;
    }

    float max_val = input[idx - window_size + 1];
    for (int i = idx - window_size + 2; i <= idx; ++i) {
        max_val = fmaxf(max_val, input[i]);
    }
    output[idx] = max_val;
}

/**
 * @brief Rolling z-score kernel
 */
__global__ void rolling_zscore_kernel(
    const float* __restrict__ input,
    float* __restrict__ output,
    int n,
    int window_size
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= n) return;

    if (idx < window_size - 1) {
        output[idx] = NAN;
        return;
    }

    float mean = 0.0f;
    for (int i = idx - window_size + 1; i <= idx; ++i) {
        mean += input[i];
    }
    mean /= window_size;

    float var = 0.0f;
    for (int i = idx - window_size + 1; i <= idx; ++i) {
        float diff = input[i] - mean;
        var += diff * diff;
    }
    float std = sqrtf(var / (window_size - 1));

    output[idx] = (std > 1e-10f) ? (input[idx] - mean) / std : 0.0f;
}

/**
 * @brief Rolling rank (percentile) kernel
 */
__global__ void rolling_rank_kernel(
    const float* __restrict__ input,
    float* __restrict__ output,
    int n,
    int window_size
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= n) return;

    if (idx < window_size - 1) {
        output[idx] = NAN;
        return;
    }

    float current = input[idx];
    int rank = 0;

    for (int i = idx - window_size + 1; i <= idx; ++i) {
        if (input[i] < current) ++rank;
    }

    output[idx] = (float)rank / (window_size - 1);
}

// ============================================================================
// Host Interface
// ============================================================================

/**
 * @brief Rolling statistics calculator
 */
class RollingStats {
public:
    RollingStats(int max_size = 1000000)
        : max_size_(max_size), stream_() {
        d_input_.resize(max_size);
        d_output_.resize(max_size);
    }

    /**
     * @brief Calculate rolling mean
     */
    void mean(const float* h_input, float* h_output, int n, int window) {
        d_input_.copy_from_host(h_input, n);

        int block_size = 256;
        int grid_size = grid_size_1d(n, block_size);

        rolling_mean_kernel<<<grid_size, block_size, 0, stream_>>>(
            d_input_.data(), d_output_.data(), n, window);

        d_output_.copy_to_host(h_output, n);
        stream_.synchronize();
    }

    /**
     * @brief Calculate rolling variance
     */
    void variance(const float* h_input, float* h_output, int n, int window) {
        d_input_.copy_from_host(h_input, n);

        int block_size = 256;
        int grid_size = grid_size_1d(n, block_size);

        rolling_variance_kernel<<<grid_size, block_size, 0, stream_>>>(
            d_input_.data(), d_output_.data(), n, window);

        d_output_.copy_to_host(h_output, n);
        stream_.synchronize();
    }

    /**
     * @brief Calculate rolling standard deviation
     */
    void std(const float* h_input, float* h_output, int n, int window) {
        d_input_.copy_from_host(h_input, n);

        int block_size = 256;
        int grid_size = grid_size_1d(n, block_size);

        rolling_std_kernel<<<grid_size, block_size, 0, stream_>>>(
            d_input_.data(), d_output_.data(), n, window);

        d_output_.copy_to_host(h_output, n);
        stream_.synchronize();
    }

    /**
     * @brief Calculate EMA
     */
    void ema(const float* h_input, float* h_output, int n, float alpha) {
        d_input_.copy_from_host(h_input, n);

        ema_kernel<<<1, 1, 0, stream_>>>(
            d_input_.data(), d_output_.data(), n, alpha);

        d_output_.copy_to_host(h_output, n);
        stream_.synchronize();
    }

    /**
     * @brief Calculate rolling z-score
     */
    void zscore(const float* h_input, float* h_output, int n, int window) {
        d_input_.copy_from_host(h_input, n);

        int block_size = 256;
        int grid_size = grid_size_1d(n, block_size);

        rolling_zscore_kernel<<<grid_size, block_size, 0, stream_>>>(
            d_input_.data(), d_output_.data(), n, window);

        d_output_.copy_to_host(h_output, n);
        stream_.synchronize();
    }

    /**
     * @brief Calculate rolling min
     */
    void min(const float* h_input, float* h_output, int n, int window) {
        d_input_.copy_from_host(h_input, n);

        int block_size = 256;
        int grid_size = grid_size_1d(n, block_size);

        rolling_min_kernel<<<grid_size, block_size, 0, stream_>>>(
            d_input_.data(), d_output_.data(), n, window);

        d_output_.copy_to_host(h_output, n);
        stream_.synchronize();
    }

    /**
     * @brief Calculate rolling max
     */
    void max(const float* h_input, float* h_output, int n, int window) {
        d_input_.copy_from_host(h_input, n);

        int block_size = 256;
        int grid_size = grid_size_1d(n, block_size);

        rolling_max_kernel<<<grid_size, block_size, 0, stream_>>>(
            d_input_.data(), d_output_.data(), n, window);

        d_output_.copy_to_host(h_output, n);
        stream_.synchronize();
    }

private:
    int max_size_;
    Stream stream_;
    DeviceArray<float> d_input_;
    DeviceArray<float> d_output_;
};

}  // namespace cuda
}  // namespace titans

#endif  // TITANS_CUDA_ENABLED
