/**
 * @file alpha_kernels.cuh
 * @brief GPU-Accelerated Alpha Factor Computation
 *
 * Implements common quantitative alpha factors using CUDA:
 * - Momentum factors (returns, volatility)
 * - Mean reversion factors
 * - Order book imbalance factors
 * - Technical indicators
 *
 * Optimized for batch processing of multiple symbols.
 */

#pragma once

#ifdef TITANS_CUDA_ENABLED

#include "cuda_utils.cuh"
#include "rolling_statistics.cuh"

namespace titans {
namespace cuda {

// ============================================================================
// Alpha Factor Kernels
// ============================================================================

/**
 * @brief Calculate returns from prices
 */
__global__ void returns_kernel(
    const float* __restrict__ prices,
    float* __restrict__ returns,
    int n,
    int period
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= n) return;

    if (idx < period) {
        returns[idx] = NAN;
        return;
    }

    returns[idx] = (prices[idx] - prices[idx - period]) / prices[idx - period];
}

/**
 * @brief Calculate log returns
 */
__global__ void log_returns_kernel(
    const float* __restrict__ prices,
    float* __restrict__ returns,
    int n,
    int period
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= n) return;

    if (idx < period) {
        returns[idx] = NAN;
        return;
    }

    returns[idx] = logf(prices[idx] / prices[idx - period]);
}

/**
 * @brief RSI (Relative Strength Index) kernel
 */
__global__ void rsi_kernel(
    const float* __restrict__ prices,
    float* __restrict__ rsi,
    int n,
    int period
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= n) return;

    if (idx < period) {
        rsi[idx] = NAN;
        return;
    }

    float avg_gain = 0.0f;
    float avg_loss = 0.0f;

    for (int i = idx - period + 1; i <= idx; ++i) {
        float change = prices[i] - prices[i - 1];
        if (change > 0) {
            avg_gain += change;
        } else {
            avg_loss -= change;  // Make positive
        }
    }

    avg_gain /= period;
    avg_loss /= period;

    if (avg_loss < 1e-10f) {
        rsi[idx] = 100.0f;
    } else {
        float rs = avg_gain / avg_loss;
        rsi[idx] = 100.0f - (100.0f / (1.0f + rs));
    }
}

/**
 * @brief MACD (Moving Average Convergence Divergence) kernel
 */
__global__ void macd_line_kernel(
    const float* __restrict__ ema_fast,
    const float* __restrict__ ema_slow,
    float* __restrict__ macd,
    int n
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= n) return;

    macd[idx] = ema_fast[idx] - ema_slow[idx];
}

/**
 * @brief Bollinger Bands kernel
 */
__global__ void bollinger_bands_kernel(
    const float* __restrict__ prices,
    float* __restrict__ upper,
    float* __restrict__ middle,
    float* __restrict__ lower,
    int n,
    int period,
    float num_std
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= n) return;

    if (idx < period - 1) {
        upper[idx] = NAN;
        middle[idx] = NAN;
        lower[idx] = NAN;
        return;
    }

    // Calculate SMA
    float sma = 0.0f;
    for (int i = idx - period + 1; i <= idx; ++i) {
        sma += prices[i];
    }
    sma /= period;

    // Calculate std
    float var = 0.0f;
    for (int i = idx - period + 1; i <= idx; ++i) {
        float diff = prices[i] - sma;
        var += diff * diff;
    }
    float std = sqrtf(var / (period - 1));

    middle[idx] = sma;
    upper[idx] = sma + num_std * std;
    lower[idx] = sma - num_std * std;
}

/**
 * @brief Order book imbalance kernel
 */
__global__ void book_imbalance_kernel(
    const float* __restrict__ bid_qty,
    const float* __restrict__ ask_qty,
    float* __restrict__ imbalance,
    int n
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= n) return;

    float total = bid_qty[idx] + ask_qty[idx];
    if (total < 1e-10f) {
        imbalance[idx] = 0.0f;
    } else {
        imbalance[idx] = (bid_qty[idx] - ask_qty[idx]) / total;
    }
}

/**
 * @brief Volume-weighted imbalance kernel
 */
__global__ void vwap_imbalance_kernel(
    const float* __restrict__ bid_prices,
    const float* __restrict__ bid_qtys,
    const float* __restrict__ ask_prices,
    const float* __restrict__ ask_qtys,
    float* __restrict__ imbalance,
    int n,
    int levels
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= n) return;

    int base = idx * levels;

    float bid_value = 0.0f;
    float ask_value = 0.0f;

    for (int i = 0; i < levels; ++i) {
        bid_value += bid_prices[base + i] * bid_qtys[base + i];
        ask_value += ask_prices[base + i] * ask_qtys[base + i];
    }

    float total = bid_value + ask_value;
    if (total < 1e-10f) {
        imbalance[idx] = 0.0f;
    } else {
        imbalance[idx] = (bid_value - ask_value) / total;
    }
}

/**
 * @brief Price momentum kernel (rate of change)
 */
__global__ void momentum_kernel(
    const float* __restrict__ prices,
    float* __restrict__ momentum,
    int n,
    int period
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= n) return;

    if (idx < period) {
        momentum[idx] = NAN;
        return;
    }

    momentum[idx] = prices[idx] / prices[idx - period] - 1.0f;
}

/**
 * @brief Mean reversion score kernel (z-score of price vs moving average)
 */
__global__ void mean_reversion_kernel(
    const float* __restrict__ prices,
    float* __restrict__ score,
    int n,
    int window
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= n) return;

    if (idx < window - 1) {
        score[idx] = NAN;
        return;
    }

    // Calculate moving average and std
    float mean = 0.0f;
    for (int i = idx - window + 1; i <= idx; ++i) {
        mean += prices[i];
    }
    mean /= window;

    float var = 0.0f;
    for (int i = idx - window + 1; i <= idx; ++i) {
        float diff = prices[i] - mean;
        var += diff * diff;
    }
    float std = sqrtf(var / (window - 1));

    // Z-score (negative for mean reversion signal)
    if (std > 1e-10f) {
        score[idx] = -(prices[idx] - mean) / std;
    } else {
        score[idx] = 0.0f;
    }
}

/**
 * @brief Volatility (realized volatility) kernel
 */
__global__ void volatility_kernel(
    const float* __restrict__ returns,
    float* __restrict__ vol,
    int n,
    int window
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= n) return;

    if (idx < window - 1) {
        vol[idx] = NAN;
        return;
    }

    // Calculate mean return
    float mean = 0.0f;
    for (int i = idx - window + 1; i <= idx; ++i) {
        if (!isnan(returns[i])) {
            mean += returns[i];
        }
    }
    mean /= window;

    // Calculate variance
    float var = 0.0f;
    for (int i = idx - window + 1; i <= idx; ++i) {
        if (!isnan(returns[i])) {
            float diff = returns[i] - mean;
            var += diff * diff;
        }
    }

    // Annualized volatility (assuming daily returns, 252 trading days)
    vol[idx] = sqrtf(var / (window - 1)) * sqrtf(252.0f);
}

// ============================================================================
// Host Interface
// ============================================================================

/**
 * @brief Alpha factor calculator
 */
class AlphaCalculator {
public:
    AlphaCalculator(int max_size = 1000000)
        : max_size_(max_size), stream_() {
        d_input_.resize(max_size);
        d_output_.resize(max_size);
        d_temp_.resize(max_size);
    }

    /**
     * @brief Calculate returns
     */
    void returns(const float* prices, float* returns, int n, int period = 1) {
        d_input_.copy_from_host(prices, n);

        int block_size = 256;
        int grid_size = grid_size_1d(n, block_size);

        returns_kernel<<<grid_size, block_size, 0, stream_>>>(
            d_input_.data(), d_output_.data(), n, period);

        d_output_.copy_to_host(returns, n);
        stream_.synchronize();
    }

    /**
     * @brief Calculate log returns
     */
    void log_returns(const float* prices, float* returns, int n, int period = 1) {
        d_input_.copy_from_host(prices, n);

        int block_size = 256;
        int grid_size = grid_size_1d(n, block_size);

        log_returns_kernel<<<grid_size, block_size, 0, stream_>>>(
            d_input_.data(), d_output_.data(), n, period);

        d_output_.copy_to_host(returns, n);
        stream_.synchronize();
    }

    /**
     * @brief Calculate RSI
     */
    void rsi(const float* prices, float* rsi_out, int n, int period = 14) {
        d_input_.copy_from_host(prices, n);

        int block_size = 256;
        int grid_size = grid_size_1d(n, block_size);

        rsi_kernel<<<grid_size, block_size, 0, stream_>>>(
            d_input_.data(), d_output_.data(), n, period);

        d_output_.copy_to_host(rsi_out, n);
        stream_.synchronize();
    }

    /**
     * @brief Calculate Bollinger Bands
     */
    void bollinger_bands(const float* prices,
                         float* upper, float* middle, float* lower,
                         int n, int period = 20, float num_std = 2.0f) {
        d_input_.copy_from_host(prices, n);

        DeviceArray<float> d_upper(n);
        DeviceArray<float> d_middle(n);
        DeviceArray<float> d_lower(n);

        int block_size = 256;
        int grid_size = grid_size_1d(n, block_size);

        bollinger_bands_kernel<<<grid_size, block_size, 0, stream_>>>(
            d_input_.data(),
            d_upper.data(), d_middle.data(), d_lower.data(),
            n, period, num_std);

        d_upper.copy_to_host(upper, n);
        d_middle.copy_to_host(middle, n);
        d_lower.copy_to_host(lower, n);
        stream_.synchronize();
    }

    /**
     * @brief Calculate book imbalance
     */
    void book_imbalance(const float* bid_qty, const float* ask_qty,
                        float* imbalance, int n) {
        DeviceArray<float> d_bid(n);
        DeviceArray<float> d_ask(n);

        d_bid.copy_from_host(bid_qty, n);
        d_ask.copy_from_host(ask_qty, n);

        int block_size = 256;
        int grid_size = grid_size_1d(n, block_size);

        book_imbalance_kernel<<<grid_size, block_size, 0, stream_>>>(
            d_bid.data(), d_ask.data(), d_output_.data(), n);

        d_output_.copy_to_host(imbalance, n);
        stream_.synchronize();
    }

    /**
     * @brief Calculate momentum
     */
    void momentum(const float* prices, float* mom, int n, int period = 10) {
        d_input_.copy_from_host(prices, n);

        int block_size = 256;
        int grid_size = grid_size_1d(n, block_size);

        momentum_kernel<<<grid_size, block_size, 0, stream_>>>(
            d_input_.data(), d_output_.data(), n, period);

        d_output_.copy_to_host(mom, n);
        stream_.synchronize();
    }

    /**
     * @brief Calculate mean reversion score
     */
    void mean_reversion(const float* prices, float* score, int n, int window = 20) {
        d_input_.copy_from_host(prices, n);

        int block_size = 256;
        int grid_size = grid_size_1d(n, block_size);

        mean_reversion_kernel<<<grid_size, block_size, 0, stream_>>>(
            d_input_.data(), d_output_.data(), n, window);

        d_output_.copy_to_host(score, n);
        stream_.synchronize();
    }

    /**
     * @brief Calculate volatility
     */
    void volatility(const float* returns, float* vol, int n, int window = 20) {
        d_input_.copy_from_host(returns, n);

        int block_size = 256;
        int grid_size = grid_size_1d(n, block_size);

        volatility_kernel<<<grid_size, block_size, 0, stream_>>>(
            d_input_.data(), d_output_.data(), n, window);

        d_output_.copy_to_host(vol, n);
        stream_.synchronize();
    }

private:
    int max_size_;
    Stream stream_;
    DeviceArray<float> d_input_;
    DeviceArray<float> d_output_;
    DeviceArray<float> d_temp_;
};

}  // namespace cuda
}  // namespace titans

#endif  // TITANS_CUDA_ENABLED
