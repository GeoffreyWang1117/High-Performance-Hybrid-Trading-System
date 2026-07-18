# Titans Debugging Guide

## Quick Reference

### Debug Macros

```cpp
#include "titans/core/debug.hpp"
using namespace titans::debug;

// Logging
LOG_TRACE(LogCategory::Context, "Detailed trace info");
LOG_DEBUG(LogCategory::LLM, "Variable x = " + std::to_string(x));
LOG_INFO(LogCategory::Experiment, "Starting experiment");
LOG_WARN(LogCategory::Network, "Connection slow");
LOG_ERROR(LogCategory::GPU, "CUDA error: " + err_msg);
LOG_FATAL(LogCategory::Memory, "Out of memory");

// Assertions
TITANS_ASSERT(ptr != nullptr);
TITANS_ASSERT_MSG(x > 0, "x must be positive");
TITANS_ASSERT_EQ(expected, actual);

// Profiling
TITANS_PROFILE_FUNCTION();  // Profile entire function
TITANS_PROFILE_SCOPE("loop_body");  // Profile specific scope
TITANS_PROFILE("custom_name");  // Record to profiler

// Memory tracking
TITANS_TRACK_ALLOC("context_buffer", 1024 * 1024);
TITANS_TRACK_DEALLOC("context_buffer", 1024 * 1024);
```

---

## 1. Logging System

### Log Levels

| Level | Use Case |
|-------|----------|
| `Trace` | Very detailed debugging (disabled by default) |
| `Debug` | Development debugging information |
| `Info` | Normal operational messages |
| `Warn` | Warning conditions (recoverable) |
| `Error` | Error conditions (may continue) |
| `Fatal` | Fatal errors (will abort) |

### Log Categories

| Category | Description |
|----------|-------------|
| `General` | General application logs |
| `Context` | Context management (versioned entities) |
| `LLM` | LLM backend communication |
| `Experiment` | Experiment execution |
| `GPU` | GPU operations and metrics |
| `Network` | HTTP and network operations |
| `Memory` | Memory allocation tracking |
| `Performance` | Performance profiling |

### Configuration

```cpp
// Set minimum log level
Logger::instance().set_level(LogLevel::Debug);

// Enable/disable colors
Logger::instance().set_color(true);

// Log to file
Logger::instance().set_file("titans.log");
```

### Environment Variable

```bash
export TITANS_LOG_LEVEL=debug  # trace, debug, info, warn, error
export TITANS_LOG_FILE=experiment.log
```

---

## 2. Assertions

### Basic Assertions

```cpp
// Simple assertion
TITANS_ASSERT(condition);

// With custom message
TITANS_ASSERT_MSG(condition, "Explanation of what went wrong");

// Comparison assertions
TITANS_ASSERT_EQ(a, b);  // a == b
TITANS_ASSERT_NE(a, b);  // a != b
TITANS_ASSERT_LT(a, b);  // a < b
TITANS_ASSERT_LE(a, b);  // a <= b
TITANS_ASSERT_GT(a, b);  // a > b
TITANS_ASSERT_GE(a, b);  // a >= b

// Debug-only (compiled out in Release)
TITANS_DEBUG_ASSERT(expensive_check());
```

### Assertion Output

When an assertion fails:
```
FATAL GEN debug.hpp:123 | Assertion failed: x > 0
  Location: experiment.cpp:45 in run_experiment
  Message: x must be positive, got -3
Stack trace:
  #0 titans::debug::assertion_failed(...)
  #1 run_experiment(...)
  #2 main(...)
```

---

## 3. Performance Profiling

### Scoped Profiling

```cpp
void process_events() {
    TITANS_PROFILE_FUNCTION();  // Profiles entire function

    {
        TITANS_PROFILE_SCOPE("preprocessing");
        // ... preprocessing code ...
    }

    {
        TITANS_PROFILE_SCOPE("inference");
        // ... inference code ...
    }
}
```

### Manual Profiling

```cpp
auto start = std::chrono::high_resolution_clock::now();
// ... code to profile ...
auto end = std::chrono::high_resolution_clock::now();
auto ms = std::chrono::duration<double, std::milli>(end - start).count();
Profiler::instance().record("my_operation", ms);
```

### Viewing Results

```cpp
// Print profiling report
Profiler::instance().print_report();

// Get raw stats
auto stats = Profiler::instance().get_stats();
for (const auto& s : stats) {
    std::cout << s.name << ": " << s.avg_time_ms() << " ms avg\n";
}

// Clear stats
Profiler::instance().clear();
```

### Sample Output

```
╔═══════════════════════════════════════════════════════════════════════╗
║                      PROFILING REPORT                                 ║
╠═══════════════════════════════════════════════════════════════════════╣
║ Name                      │ Calls  │ Total(ms) │ Avg(ms) │ Min/Max    ║
╠═══════════════════════════╪════════╪═══════════╪═════════╪════════════╣
║ llm_inference             │    100 │   15234.5 │ 152.345 │ 89.2/312.4 ║
║ context_building          │    100 │     523.2 │   5.232 │  2.1/12.8  ║
║ contamination_injection   │    100 │      45.6 │   0.456 │  0.2/1.2   ║
╚═══════════════════════════════════════════════════════════════════════╝
```

---

## 4. Memory Tracking

### Usage

```cpp
// Track allocations
void* buffer = malloc(1024 * 1024);
TITANS_TRACK_ALLOC("context_buffer", 1024 * 1024);

// Track deallocations
free(buffer);
TITANS_TRACK_DEALLOC("context_buffer", 1024 * 1024);

// Print report
MemoryTracker::instance().print_report();
```

### With RAII

```cpp
template <typename T>
class TrackedVector {
public:
    void push_back(const T& value) {
        size_t old_cap = vec_.capacity();
        vec_.push_back(value);
        size_t new_cap = vec_.capacity();
        if (new_cap > old_cap) {
            TITANS_TRACK_ALLOC("TrackedVector", (new_cap - old_cap) * sizeof(T));
        }
    }
    // ...
};
```

---

## 5. Debug Utilities

### Hex Dump

```cpp
uint8_t data[] = {0x48, 0x65, 0x6c, 0x6c, 0x6f};
std::cout << Dumper::hex_dump(data, sizeof(data));
// Output:
// 00000000: 48 65 6c 6c 6f                                   | Hello
```

### JSON Pretty Print

```cpp
std::string json = R"({"name":"test","values":[1,2,3]})";
std::cout << Dumper::json_pretty(json);
// Output:
// {
//   "name": "test",
//   "values": [
//     1,
//     2,
//     3
//   ]
// }
```

---

## 6. Debug Console

### Built-in Commands

| Command | Description |
|---------|-------------|
| `help` | Show available commands |
| `profile` | Show profiling statistics |
| `memory` | Show memory usage |
| `loglevel <level>` | Set log level |
| `quit` | Exit console |

### Custom Commands

```cpp
DebugConsole::instance().register_command(
    "experiment",
    "Show current experiment status",
    [](const std::vector<std::string>& args) {
        return "Running: experiment_001\nProgress: 45%";
    }
);

// Run interactive console
DebugConsole::instance().run_interactive();
```

### Interactive Session

```
Titans Debug Console. Type 'help' for commands.
> help
Available commands:
  profile - Show profiling report
  memory - Show memory report
  loglevel - Set log level (trace/debug/info/warn/error)
  experiment - Show current experiment status
  help - Show this help
  quit - Exit console
> loglevel debug
Log level set to debug
> profile
llm_inference: 100 calls, 152.345 ms avg
context_building: 100 calls, 5.232 ms avg
> quit
```

---

## 7. Debugging Specific Components

### Context Contamination Debugging

```cpp
// Enable detailed context tracing
Logger::instance().set_level(LogLevel::Trace);

// In your code:
LOG_TRACE(LogCategory::Context,
    "Entity " + entity.id() + " state v" + std::to_string(state.version) +
    " valid=" + std::to_string(state.is_current(now)));

// Check contamination detection
auto issue = checker.check_state_validity(state, now, entity);
if (issue) {
    LOG_WARN(LogCategory::Context,
        "Contamination detected: " + issue->description);
}
```

### LLM Backend Debugging

```cpp
// Log request/response
LOG_DEBUG(LogCategory::LLM, "Request: " + Dumper::json_pretty(request_json));
LOG_DEBUG(LogCategory::LLM, "Response: " + Dumper::json_pretty(response.body));

// Check timing
{
    TITANS_PROFILE_SCOPE("llm_complete");
    auto response = backend->complete(request);
}
```

### GPU Debugging

```cpp
// Monitor GPU during experiment
GPUMonitor monitor(1000);  // 1s interval
monitor.add_callback([](const GPUSnapshot& s) {
    if (s.total_gpu_utilization() > 95) {
        LOG_WARN(LogCategory::GPU, "GPU near saturation");
    }
    if (s.max_temperature() > 80) {
        LOG_WARN(LogCategory::GPU, "GPU overheating");
    }
});
monitor.start();
```

---

## 8. Common Issues

### Issue: Experiment Accuracy Low

```cpp
// Enable verbose output
LOG_INFO(LogCategory::Experiment, "Contamination rate: " +
    std::to_string(config.contamination_rate));

// Check individual predictions
for (const auto& output : outputs) {
    if (!output.is_correct) {
        LOG_DEBUG(LogCategory::Experiment,
            "Misprediction: task=" + output.task_id +
            " predicted=" + output.predicted_class +
            " actual=" + output.ground_truth_class +
            " contaminations=" + std::to_string(output.contamination_types_present.size()));
    }
}
```

### Issue: LLM Timeout

```cpp
// Increase timeout
OllamaConfig config;
config.timeout_ms = 300000;  // 5 minutes

// Check backend health
if (!backend->is_available()) {
    LOG_ERROR(LogCategory::LLM, "Backend not available");
}

// Log detailed timing
{
    TITANS_PROFILE("llm_request");
    auto response = backend->complete(request);
    LOG_DEBUG(LogCategory::LLM, "Latency: " + std::to_string(response.latency_ms) + " ms");
}
```

### Issue: Memory Growing

```cpp
// Track key allocations
TITANS_TRACK_ALLOC("history_buffer", history.size() * sizeof(Event));

// Check periodically
if (MemoryTracker::instance().total_allocated() > 1024 * 1024 * 1024) {
    LOG_WARN(LogCategory::Memory, "Memory usage exceeds 1GB");
    MemoryTracker::instance().print_report();
}
```

---

## 9. Build Configuration

### Debug Build

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug
# Enables:
# - TITANS_DEBUG_ASSERT
# - Address sanitizer
# - Undefined behavior sanitizer
# - No optimization
```

### Release with Debug Info

```bash
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
# Enables:
# - Optimizations
# - Debug symbols
# - Still allows profiling
```

### Verbose Build

```bash
make VERBOSE=1  # Show compiler commands
```

---

## 10. Troubleshooting Checklist

- [ ] Set log level to Debug/Trace
- [ ] Enable log file output
- [ ] Add TITANS_PROFILE to slow code paths
- [ ] Check memory usage with MemoryTracker
- [ ] Verify GPU status with GPUQuery
- [ ] Use assertions to validate assumptions
- [ ] Check LLM backend availability
- [ ] Review contamination injection logs
- [ ] Compare baseline vs versioned context results
