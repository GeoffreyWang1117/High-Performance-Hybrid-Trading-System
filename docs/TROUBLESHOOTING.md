# Titans Troubleshooting Guide

## Quick Diagnostics

Run the built-in diagnostics:

```bash
# Check LLM backends
./titans_llm_experiment

# Check GPU availability
nvidia-smi

# Check system resources
htop
```

---

## Build Issues

### CMake Error: Could not find CUDA

**Problem:**
```
CMake Error: CUDA compiler not found
```

**Solutions:**

1. Install CUDA Toolkit:
```bash
# Ubuntu/Debian
sudo apt install nvidia-cuda-toolkit

# Or download from NVIDIA
wget https://developer.download.nvidia.com/compute/cuda/repos/...
```

2. Disable CUDA (CPU-only build):
```bash
cmake .. -DTITANS_ENABLE_CUDA=OFF
```

3. Set CUDA path:
```bash
export CUDA_HOME=/usr/local/cuda
export PATH=$CUDA_HOME/bin:$PATH
cmake ..
```

### Linker Error: undefined reference to pthread

**Problem:**
```
undefined reference to `pthread_create`
```

**Solution:**
```bash
cmake .. -DCMAKE_CXX_FLAGS="-pthread"
# Or ensure Threads::Threads is linked
```

### Compiler Error: C++20 features not supported

**Problem:**
```
error: 'std::format' was not declared
```

**Solution:**
```bash
# Use GCC 10+ or Clang 10+
export CXX=g++-11
cmake ..

# Or install newer compiler
sudo apt install g++-11
```

---

## Runtime Issues

### Ollama Not Available

**Problem:**
```
✗ No LLM backend available!
```

**Solutions:**

1. Start Ollama:
```bash
ollama serve
```

2. Check if running:
```bash
curl http://localhost:11434/api/tags
```

3. Pull a model:
```bash
ollama pull llama3.1:8b
```

4. Check port:
```cpp
OllamaConfig config;
config.port = 11434;  // Default port
```

### vLLM Connection Refused

**Problem:**
```
HTTP error: Connection refused
```

**Solutions:**

1. Start vLLM server:
```bash
python -m vllm.entrypoints.openai.api_server \
    --model meta-llama/Llama-3.1-8B-Instruct \
    --port 8000
```

2. Check binding:
```bash
netstat -tlnp | grep 8000
```

3. Update config:
```cpp
VLLMConfig config;
config.host = "0.0.0.0";  // If running in container
config.port = 8000;
```

### LLM Timeout

**Problem:**
```
Receive timeout
```

**Solutions:**

1. Increase timeout:
```cpp
OllamaConfig config;
config.timeout_ms = 300000;  // 5 minutes
```

2. Use smaller model:
```bash
ollama pull llama3.1:8b  # Instead of 70b
```

3. Check GPU memory:
```bash
nvidia-smi
# If OOM, use quantized model
ollama pull llama3.1:8b-q4_0
```

### JSON Parse Error

**Problem:**
```
Failed to parse JSON response
```

**Solutions:**

1. Enable debug logging:
```cpp
Logger::instance().set_level(LogLevel::Debug);
```

2. Check raw response:
```cpp
LOG_DEBUG(LogCategory::LLM, "Raw response: " + response.body);
```

3. Request JSON format:
```cpp
request.response_format = "json";
```

---

## GPU Issues

### nvidia-smi Not Found

**Problem:**
```
nvidia-smi: command not found
```

**Solutions:**

1. Install NVIDIA driver:
```bash
ubuntu-drivers autoinstall
# Or
sudo apt install nvidia-driver-535
```

2. Reboot:
```bash
sudo reboot
```

3. Verify:
```bash
nvidia-smi
```

### CUDA Out of Memory

**Problem:**
```
CUDA error: out of memory
```

**Solutions:**

1. Check current usage:
```bash
nvidia-smi
```

2. Kill other processes:
```bash
fuser -v /dev/nvidia*
kill <pid>
```

3. Use smaller batch size:
```cpp
config.batch_size = 4;  // Reduce from 16
```

4. Use quantized model:
```bash
ollama pull llama3.1:8b-q4_0  # 4-bit quantization
```

### GPU Not Detected in Code

**Problem:**
```cpp
GPUQuery::is_nvidia_available()  // returns false
```

**Solutions:**

1. Check permissions:
```bash
ls -la /dev/nvidia*
sudo usermod -aG video $USER
# Log out and back in
```

2. Check nvidia-smi works:
```bash
nvidia-smi --query-gpu=name --format=csv
```

3. Check library path:
```bash
export LD_LIBRARY_PATH=/usr/local/cuda/lib64:$LD_LIBRARY_PATH
```

---

## Experiment Issues

### Low Accuracy

**Symptoms:**
- Accuracy below 60%
- High contamination delta

**Diagnosis:**
```cpp
// Enable detailed logging
Logger::instance().set_level(LogLevel::Debug);

// Check individual predictions
for (const auto& out : outputs) {
    LOG_DEBUG(LogCategory::Experiment,
        "Task: " + out.task_id +
        " Pred: " + out.predicted_class +
        " GT: " + out.ground_truth_class +
        " Correct: " + std::to_string(out.is_correct));
}
```

**Solutions:**

1. Verify ground truth:
```cpp
// Check anomaly rate
int anomalies = 0;
for (const auto& e : events) {
    if (e.is_anomaly) anomalies++;
}
LOG_INFO(LogCategory::Experiment,
    "Anomaly rate: " + std::to_string(100.0 * anomalies / events.size()) + "%");
```

2. Reduce contamination:
```cpp
config.contamination_rate = 0.05;  // Start low
```

3. Check context method:
```cpp
// Try without context
config.method = ContextMethod::NoHistory;
```

### Results Not Reproducible

**Symptoms:**
- Different results on each run
- Variance higher than expected

**Solutions:**

1. Fix random seed:
```cpp
config.seed = 42;  // Fixed seed
```

2. Check all random sources:
```cpp
// Model temperature
request.temperature = 0.0;  // Deterministic

// Injector seed
ContaminationInjector injector(config.seed);
```

3. Multiple runs:
```cpp
grid_config.seeds_per_config = 10;  // More samples
```

### Experiment Hangs

**Symptoms:**
- Progress stops
- No error message

**Diagnosis:**
```cpp
// Add timeout
config.timeout_ms = 60000;  // 1 minute

// Add progress logging
LOG_INFO(LogCategory::Experiment,
    "Processing event " + std::to_string(i) + "/" + std::to_string(n));
```

**Solutions:**

1. Check LLM backend:
```bash
curl http://localhost:11434/api/tags
```

2. Reduce workload:
```cpp
config.num_events = 100;  // Start small
```

3. Add watchdog:
```cpp
std::thread watchdog([&]() {
    std::this_thread::sleep_for(std::chrono::minutes(10));
    if (still_running) {
        LOG_ERROR(LogCategory::Experiment, "Watchdog timeout!");
        std::abort();
    }
});
```

---

## Performance Issues

### Slow Experiments

**Symptoms:**
- < 1 event/second
- High latency

**Diagnosis:**
```cpp
TITANS_PROFILE_FUNCTION();
// Check profiler report
Profiler::instance().print_report();
```

**Solutions:**

1. Use faster model:
```cpp
request.model = "llama3.1:8b";  // Not 70b
```

2. Reduce context:
```cpp
config.method = ContextMethod::FixedWindow;
// Or reduce window size
FixedWindowContext ctx(50);  // Not 100
```

3. Batch requests:
```cpp
auto responses = backend->complete_batch(requests, 4);
```

4. Parallel workers:
```cpp
LocalDistributedRunner runner(8);  // More workers
```

### High Memory Usage

**Symptoms:**
- Memory grows over time
- OOM errors

**Diagnosis:**
```cpp
MemoryTracker::instance().print_report();
```

**Solutions:**

1. Track allocations:
```cpp
TITANS_TRACK_ALLOC("history", history.size() * sizeof(Event));
```

2. Clear history:
```cpp
if (history.size() > 10000) {
    history.erase(history.begin(), history.begin() + 5000);
}
```

3. Use selective forgetting:
```cpp
ForgettingCriteria criteria;
criteria.max_age = 60000000000LL;  // 60 seconds
auto retained = forgetter.apply(history, now, criteria);
```

---

## Network Issues

### HTTP Connection Failed

**Problem:**
```
Connection failed: Connection refused
```

**Solutions:**

1. Check service is running:
```bash
curl -v http://localhost:11434/
```

2. Check firewall:
```bash
sudo ufw status
sudo ufw allow 11434
```

3. Check binding:
```bash
# In Ollama
OLLAMA_HOST=0.0.0.0 ollama serve
```

### DNS Resolution Failed

**Problem:**
```
DNS resolution failed: Name or service not known
```

**Solutions:**

1. Use IP address:
```cpp
config.host = "127.0.0.1";  // Instead of "localhost"
```

2. Check DNS:
```bash
nslookup localhost
cat /etc/hosts
```

---

## Data Issues

### Data File Not Found

**Problem:**
```
Failed to open: data/lobster.csv
```

**Solutions:**

1. Check path:
```cpp
// Use absolute path
source->open("/absolute/path/to/data.csv");
```

2. Check permissions:
```bash
ls -la data/
chmod 644 data/*.csv
```

### Invalid Data Format

**Problem:**
```
Parse error at line 42
```

**Solutions:**

1. Check file format:
```bash
head -5 data/file.csv
```

2. Check encoding:
```bash
file data/file.csv
# Convert if needed
iconv -f ISO-8859-1 -t UTF-8 file.csv > file_utf8.csv
```

3. Check line endings:
```bash
cat -A data/file.csv | head
# Convert DOS to Unix
dos2unix data/file.csv
```

---

## Getting Help

1. **Enable verbose logging:**
```cpp
Logger::instance().set_level(LogLevel::Trace);
Logger::instance().set_file("debug.log");
```

2. **Collect system info:**
```bash
uname -a
nvidia-smi
cmake --version
g++ --version
```

3. **Create minimal reproduction:**
```cpp
// Simplest failing case
int main() {
    auto backend = LLMBackendFactory::create_ollama();
    std::cout << backend->is_available() << std::endl;
    return 0;
}
```

4. **Check the logs:**
```bash
cat debug.log | grep ERROR
cat debug.log | grep -A5 "Assertion failed"
```
