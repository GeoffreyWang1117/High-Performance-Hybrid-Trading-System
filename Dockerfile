# Titans Trading System - Docker Build
# Multi-stage build for optimized image size

# Stage 1: Build environment
FROM nvidia/cuda:12.2.0-devel-ubuntu22.04 AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    git \
    wget \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /build

# Copy source code.
# examples/ is required: CMakeLists builds titans_experiment,
# titans_llm_experiment, titans_distributed_experiment and
# titans_config_experiment from it, so omitting it fails at configure time.
COPY CMakeLists.txt ./
COPY include/ ./include/
COPY src/ ./src/
COPY tests/ ./tests/
COPY examples/ ./examples/

# Build
RUN mkdir build && cd build && \
    cmake .. \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DTITANS_ENABLE_CUDA=ON \
        -DTITANS_ENABLE_TESTS=ON \
        -DTITANS_ENABLE_BENCHMARKS=ON && \
    ninja

# Run tests
RUN cd build && ctest --output-on-failure

# Stage 2: Runtime environment
FROM nvidia/cuda:12.2.0-runtime-ubuntu22.04 AS runtime

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    libstdc++6 \
    python3 \
    python3-pip \
    && rm -rf /var/lib/apt/lists/*

# Copy binaries from builder
COPY --from=builder /build/build/titans_engine /usr/local/bin/
COPY --from=builder /build/build/titans_replay /usr/local/bin/
COPY --from=builder /build/build/titans_benchmark /usr/local/bin/
COPY --from=builder /build/build/titans_dataset /usr/local/bin/
COPY --from=builder /build/build/titans_lanes /usr/local/bin/
COPY --from=builder /build/build/titans_experiment /usr/local/bin/
COPY --from=builder /build/build/titans_llm_experiment /usr/local/bin/

# Configuration the engine actually reads. --config is fatal on a missing file,
# so shipping the default matters.
COPY config/ /opt/titans/config/

# Copy Python components
COPY python/ /opt/titans/python/
RUN pip3 install -r /opt/titans/python/requirements.txt

# Create data directories
RUN mkdir -p /data/logs /data/replay /data/config

# Set environment variables
ENV TITANS_DATA_PATH=/data
ENV TITANS_LOG_PATH=/data/logs
ENV PYTHONPATH=/opt/titans/python

# Default command
CMD ["titans_engine", "--mode", "shadow", "--symbols", "BTCUSDT"]

# Expose ports
# Streamlit dashboard
EXPOSE 8501
# Metrics endpoint (future)
EXPOSE 9090

# Stage 3: Development environment
FROM builder AS dev

# Install development tools
RUN apt-get update && apt-get install -y \
    gdb \
    valgrind \
    clang-format \
    clang-tidy \
    python3 \
    python3-pip \
    vim \
    && rm -rf /var/lib/apt/lists/*

# Install Python dev dependencies
COPY python/requirements.txt /tmp/
RUN pip3 install -r /tmp/requirements.txt

# Set working directory
WORKDIR /workspace

# Mount point for source code
VOLUME ["/workspace"]

CMD ["/bin/bash"]
