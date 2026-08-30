/**
 * @file platform.hpp
 * @brief CPU pinning and machine fingerprinting for reproducible benchmarks.
 *
 * A latency number without the machine state that produced it is not a
 * result, it is an anecdote. Every benchmark run emits a MachineFingerprint
 * alongside its measurements, including the conditions we could NOT control
 * (see `caveats()`), so a reader can judge how much weight the numbers carry.
 */

#pragma once

#include <sched.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace titans {
namespace bench {

// ============================================================================
// Small file/command helpers
// ============================================================================

inline std::string read_file_trimmed(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

inline std::string run_command(const std::string& cmd) {
    FILE* pipe = popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!pipe) return {};
    std::string out;
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

// ============================================================================
// CPU topology
// ============================================================================

/**
 * @brief Physical-core -> logical-CPU mapping, derived from sysfs.
 *
 * Needed because pinning to a logical CPU whose SMT sibling is busy gives
 * results that silently depend on unrelated load.
 */
struct CpuTopology {
    struct Core {
        int core_id = -1;
        std::vector<int> logical_cpus;  // size 2 when SMT is on
    };
    std::vector<Core> cores;
    bool smt_active = false;

    static CpuTopology detect() {
        CpuTopology topo;
        topo.smt_active = (read_file_trimmed("/sys/devices/system/cpu/smt/active") == "1");

        const long n = sysconf(_SC_NPROCESSORS_CONF);
        for (long cpu = 0; cpu < n; ++cpu) {
            const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu);
            const std::string core_str = read_file_trimmed(base + "/topology/core_id");
            if (core_str.empty()) continue;
            const int core_id = std::atoi(core_str.c_str());

            Core* found = nullptr;
            for (auto& c : topo.cores) {
                if (c.core_id == core_id) { found = &c; break; }
            }
            if (!found) {
                topo.cores.push_back(Core{core_id, {}});
                found = &topo.cores.back();
            }
            found->logical_cpus.push_back(static_cast<int>(cpu));
        }
        return topo;
    }

    /// @brief First logical CPU of physical core @p index, or -1.
    int primary_cpu_of_core(size_t index) const {
        if (index >= cores.size() || cores[index].logical_cpus.empty()) return -1;
        return cores[index].logical_cpus.front();
    }

    /// @brief The SMT sibling of @p cpu, or -1 if none.
    int sibling_of(int cpu) const {
        for (const auto& c : cores) {
            if (c.logical_cpus.size() < 2) continue;
            if (c.logical_cpus[0] == cpu) return c.logical_cpus[1];
            if (c.logical_cpus[1] == cpu) return c.logical_cpus[0];
        }
        return -1;
    }
};

// ============================================================================
// Pinning
// ============================================================================

/// @brief Pin the calling thread to logical CPU @p cpu. Returns false on failure.
inline bool pin_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

inline int current_cpu() { return sched_getcpu(); }

// ============================================================================
// Machine fingerprint
// ============================================================================

/**
 * @brief Everything about the host that can move a latency number.
 *
 * Serialized into every results file. `caveats()` is deliberately blunt: it
 * lists the sources of error we did NOT eliminate, so that nobody quotes a
 * p99 from this machine as if it came from a tuned trading host.
 */
struct MachineFingerprint {
    std::string cpu_model;
    int physical_cores = 0;
    int logical_cpus = 0;
    bool smt_active = false;
    std::string governor;
    std::string boost_enabled;
    std::string clocksource;
    bool invariant_tsc = false;
    bool isolcpus_set = false;
    std::string kernel;
    std::string compiler;
    std::string build_flags;
    std::string hostname;
    int pinned_cpu = -1;
    double tsc_ghz = 0.0;
    double noise_floor_ns = 0.0;

    static MachineFingerprint capture() {
        MachineFingerprint fp;

        fp.cpu_model = run_command("lscpu | grep 'Model name' | head -1 | sed 's/.*: *//'");
        fp.logical_cpus = static_cast<int>(sysconf(_SC_NPROCESSORS_CONF));

        const CpuTopology topo = CpuTopology::detect();
        fp.physical_cores = static_cast<int>(topo.cores.size());
        fp.smt_active = topo.smt_active;

        fp.governor = read_file_trimmed(
            "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
        if (fp.governor.empty()) fp.governor = "unknown";

        fp.boost_enabled = read_file_trimmed("/sys/devices/system/cpu/cpufreq/boost");
        if (fp.boost_enabled.empty()) fp.boost_enabled = "unknown";

        fp.clocksource = read_file_trimmed(
            "/sys/devices/system/clocksource/clocksource0/current_clocksource");

        const std::string flags = run_command(
            "grep -o 'constant_tsc\\|nonstop_tsc' /proc/cpuinfo | sort -u | tr '\\n' ' '");
        fp.invariant_tsc = (flags.find("constant_tsc") != std::string::npos &&
                            flags.find("nonstop_tsc") != std::string::npos);

        const std::string cmdline = read_file_trimmed("/proc/cmdline");
        fp.isolcpus_set = (cmdline.find("isolcpus") != std::string::npos);

        fp.kernel = run_command("uname -r");
        fp.hostname = run_command("hostname");

#if defined(__clang__)
        fp.compiler = "clang " __clang_version__;
#elif defined(__GNUC__)
        fp.compiler = "gcc " + std::to_string(__GNUC__) + "." +
                      std::to_string(__GNUC_MINOR__) + "." +
                      std::to_string(__GNUC_PATCHLEVEL__);
#else
        fp.compiler = "unknown";
#endif

#if defined(TITANS_BUILD_FLAGS)
        fp.build_flags = TITANS_BUILD_FLAGS;
#else
        fp.build_flags = "unrecorded";
#endif
        return fp;
    }

    /**
     * @brief Uncontrolled error sources present on this host.
     *
     * Empty means the host is properly quiesced. On a general-purpose
     * workstation it never is, and the numbers must be read accordingly.
     */
    std::vector<std::string> caveats() const {
        std::vector<std::string> out;
        if (!isolcpus_set) {
            out.push_back(
                "No isolcpus/nohz_full: the kernel schedules other work on the "
                "measured core. Tail percentiles (p99.9+) include unrelated "
                "interference and should be treated as upper bounds.");
        }
        if (smt_active) {
            out.push_back(
                "SMT is enabled: the sibling hyperthread shares execution "
                "resources with the measured core. Benchmarks pin to one "
                "logical CPU but do not idle its sibling.");
        }
        if (governor != "performance") {
            out.push_back("CPU governor is '" + governor +
                          "', not 'performance': frequency scaling adds variance.");
        }
        if (boost_enabled == "1") {
            out.push_back(
                "Turbo/boost is enabled: sustained-load and burst measurements "
                "run at different clocks.");
        }
        if (!invariant_tsc) {
            out.push_back(
                "TSC is not invariant (missing constant_tsc/nonstop_tsc): "
                "cycle-based timing on this host is UNRELIABLE.");
        }
        return out;
    }

    std::string to_json(int indent = 2) const {
        const std::string pad(indent, ' ');
        std::ostringstream os;
        os << "{\n";
        os << pad << "\"cpu_model\": \"" << cpu_model << "\",\n";
        os << pad << "\"physical_cores\": " << physical_cores << ",\n";
        os << pad << "\"logical_cpus\": " << logical_cpus << ",\n";
        os << pad << "\"smt_active\": " << (smt_active ? "true" : "false") << ",\n";
        os << pad << "\"governor\": \"" << governor << "\",\n";
        os << pad << "\"boost_enabled\": \"" << boost_enabled << "\",\n";
        os << pad << "\"clocksource\": \"" << clocksource << "\",\n";
        os << pad << "\"invariant_tsc\": " << (invariant_tsc ? "true" : "false") << ",\n";
        os << pad << "\"isolcpus_set\": " << (isolcpus_set ? "true" : "false") << ",\n";
        os << pad << "\"kernel\": \"" << kernel << "\",\n";
        os << pad << "\"compiler\": \"" << compiler << "\",\n";
        os << pad << "\"build_flags\": \"" << build_flags << "\",\n";
        os << pad << "\"hostname\": \"" << hostname << "\",\n";
        os << pad << "\"pinned_cpu\": " << pinned_cpu << ",\n";
        os << pad << "\"tsc_ghz\": " << tsc_ghz << ",\n";
        os << pad << "\"noise_floor_ns\": " << noise_floor_ns << ",\n";
        os << pad << "\"caveats\": [";
        const auto cv = caveats();
        for (size_t i = 0; i < cv.size(); ++i) {
            os << "\n" << pad << "  \"" << cv[i] << "\"";
            if (i + 1 < cv.size()) os << ",";
        }
        if (!cv.empty()) os << "\n" << pad;
        os << "]\n";
        os << std::string(indent > 2 ? indent - 2 : 0, ' ') << "}";
        return os.str();
    }

    void print() const {
        std::printf("Machine:  %s\n", cpu_model.c_str());
        std::printf("          %d physical cores / %d logical, SMT %s\n",
                    physical_cores, logical_cpus, smt_active ? "on" : "off");
        std::printf("          governor=%s boost=%s clocksource=%s invariant_tsc=%s\n",
                    governor.c_str(), boost_enabled.c_str(), clocksource.c_str(),
                    invariant_tsc ? "yes" : "NO");
        std::printf("          kernel %s, %s\n", kernel.c_str(), compiler.c_str());
        std::printf("Pinned:   logical CPU %d\n", pinned_cpu);

        const auto cv = caveats();
        if (!cv.empty()) {
            std::printf("\nUNCONTROLLED ERROR SOURCES (%zu):\n", cv.size());
            for (const auto& c : cv) std::printf("  - %s\n", c.c_str());
        }
    }
};

}  // namespace bench
}  // namespace titans
