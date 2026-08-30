/**
 * @file test_task_design.cpp
 * @brief Guards the property that makes the contamination experiment meaningful.
 *
 * The experiment compares CONTEXT MANAGEMENT strategies. That comparison is
 * only informative if the underlying task actually needs context. Two failure
 * modes destroy it, and both were present before this file existed:
 *
 *   - LABEL LEAKAGE: a field of the event encodes the answer, so the task is
 *     to copy a string and every strategy scores ~100%.
 *   - GLOBAL SEPARABILITY: the label is recoverable from the single event
 *     without history, so every strategy scores the same and any difference
 *     between them is noise.
 *
 * These tests assert the task is context-NECESSARY (no context-free rule does
 * much better than chance) and context-SUFFICIENT (a rule that uses per-entity
 * history does well). If a future change to the generator breaks either, the
 * experiment silently stops measuring what it claims to, so this fails the build
 * instead.
 */

#include "titans/context/experiment_harness.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace titans;
using namespace titans::context;

namespace {

/// @brief Balanced accuracy: mean of per-class recall. Chance is 0.5.
double balanced_accuracy(int tp, int fn, int tn, int fp) {
    const double tpr = (tp + fn) ? static_cast<double>(tp) / (tp + fn) : 0.0;
    const double tnr = (tn + fp) ? static_cast<double>(tn) / (tn + fp) : 0.0;
    return 0.5 * (tpr + tnr);
}

// --------------------------------------------------------------------------

/**
 * @brief No field of the event may reveal the label.
 *
 * Checks the categorical field specifically: if `event_type` were predictive,
 * a model reading the prompt would not need context at all.
 */
bool test_no_label_leakage_in_event_type() {
    SyntheticDataGenerator gen(11);
    const auto events = gen.generate_event_stream(20000, 40, 1000000, 0.10);

    std::map<std::string, std::pair<int, int>> by_type;  // type -> {anomaly, total}
    for (const auto& e : events) {
        auto& c = by_type[e.event_type];
        c.second += 1;
        if (e.is_anomaly) c.first += 1;
    }

    const double overall = [&] {
        int a = 0;
        for (const auto& e : events) a += e.is_anomaly ? 1 : 0;
        return static_cast<double>(a) / events.size();
    }();

    std::printf("    overall anomaly rate %.4f; by event_type:\n", overall);
    for (const auto& [type, c] : by_type) {
        const double rate = static_cast<double>(c.first) / c.second;
        std::printf("      %-8s n=%6d  anomaly rate %.4f\n", type.c_str(), c.second, rate);

        if (type == "anomaly" || type == "normal") {
            std::fprintf(stderr,
                         "FAIL: event_type takes the value \"%s\", which names the "
                         "label. The ground truth is being written into the event.\n",
                         type.c_str());
            return false;
        }
        // A category that is predictive is leakage by another name.
        if (std::abs(rate - overall) > 0.03) {
            std::fprintf(stderr,
                         "FAIL: event_type \"%s\" has anomaly rate %.4f against an "
                         "overall rate of %.4f; the category leaks the label.\n",
                         type.c_str(), rate, overall);
            return false;
        }
    }
    return true;
}

/**
 * @brief No single global threshold on `value` may solve the task.
 *
 * Sweeps every threshold that could matter and takes the best one, i.e. gives
 * the context-free classifier an oracle's choice of cutoff. Even so it should
 * land near chance, because "anomalous" is defined relative to each entity's
 * own level.
 */
bool test_task_requires_context() {
    SyntheticDataGenerator gen(7);
    const auto events = gen.generate_event_stream(20000, 40, 1000000, 0.10);

    std::vector<double> values;
    values.reserve(events.size());
    for (const auto& e : events) values.push_back(e.value);
    std::sort(values.begin(), values.end());

    // Two-sided: an anomaly can be high or low, so evaluate |value - t| > w
    // over a grid of centres and widths. This is strictly more powerful than a
    // one-sided cut and still must fail.
    double best = 0.0;
    for (size_t ci = 0; ci < values.size(); ci += values.size() / 200 + 1) {
        const double centre = values[ci];
        for (double w = 0.0; w <= 400.0; w += 5.0) {
            int tp = 0, fn = 0, tn = 0, fp = 0;
            for (const auto& e : events) {
                const bool pred = std::abs(e.value - centre) > w;
                if (e.is_anomaly) { pred ? ++tp : ++fn; }
                else              { pred ? ++fp : ++tn; }
            }
            best = std::max(best, balanced_accuracy(tp, fn, tn, fp));
        }
    }

    std::printf("    best context-free global rule: %.4f balanced accuracy "
                "(chance = 0.5)\n", best);
    if (best > 0.65) {
        std::fprintf(stderr,
                     "FAIL: a context-free threshold reaches %.4f balanced "
                     "accuracy. The task is solvable without history, so it "
                     "cannot discriminate between context management "
                     "strategies.\n", best);
        return false;
    }
    return true;
}

/**
 * @brief With per-entity history the task IS solvable.
 *
 * Otherwise the previous test could pass for the wrong reason -- an impossible
 * task also defeats every global rule, and would equally fail to separate the
 * strategies under comparison.
 *
 * The reference detector uses a ROLLING MEDIAN and MAD over each entity's
 * recent history, not a running mean and standard deviation. That choice is
 * forced by the data, not stylistic: at a 10% anomaly rate with 4-8 sigma
 * excursions, the anomalies contribute roughly 0.1 * (6 sigma)^2 to the
 * variance, inflating the estimated sd to about 2.1 sigma and turning a
 * "3 sd" rule into a 6.4 sigma rule that misses most of the anomaly range.
 * A mean/sd version of this test scores 0.69; the robust version scores well
 * above the bar. The detector must not be poisoned by the very events it is
 * meant to find.
 */
bool test_task_solvable_with_context() {
    SyntheticDataGenerator gen(7);
    const auto events = gen.generate_event_stream(20000, 40, 1000000, 0.10);

    constexpr size_t kWindow = 60;   // recent history per entity
    constexpr size_t kMinObs = 30;
    constexpr double kMadToSigma = 1.4826;   // MAD -> sd for normal data
    constexpr double kZ = 3.5;

    std::map<std::string, std::vector<double>> history;

    auto median_of = [](std::vector<double> v) {
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    };

    int tp = 0, fn = 0, tn = 0, fp = 0;

    for (const auto& e : events) {
        auto& h = history[e.entity_id];

        if (h.size() >= kMinObs) {
            const double med = median_of(h);
            std::vector<double> dev;
            dev.reserve(h.size());
            for (double v : h) dev.push_back(std::abs(v - med));
            const double mad = median_of(std::move(dev));
            const double sigma = mad * kMadToSigma;

            const bool pred = sigma > 0 && std::abs(e.value - med) > kZ * sigma;
            if (e.is_anomaly) { pred ? ++tp : ++fn; }
            else              { pred ? ++fp : ++tn; }
        }

        // Append AFTER predicting: the detector never sees the current label.
        h.push_back(e.value);
        if (h.size() > kWindow) h.erase(h.begin());
    }

    const double acc = balanced_accuracy(tp, fn, tn, fp);
    std::printf("    per-entity rolling median/MAD rule: %.4f balanced accuracy "
                "(tp=%d fn=%d tn=%d fp=%d)\n", acc, tp, fn, tn, fp);
    if (acc < 0.85) {
        std::fprintf(stderr,
                     "FAIL: even with per-entity history the task only reaches "
                     "%.4f balanced accuracy. The task may be unsolvable, which "
                     "would flatten every strategy comparison just as leakage "
                     "would.\n", acc);
        return false;
    }
    return true;
}

}  // namespace

bool run_task_design_tests() {
    struct Case { const char* name; bool (*fn)(); };
    const Case cases[] = {
        {"event_type does not leak the label",       test_no_label_leakage_in_event_type},
        {"task is not solvable without context",     test_task_requires_context},
        {"task IS solvable with per-entity context", test_task_solvable_with_context},
    };
    bool all = true;
    for (const auto& c : cases) {
        std::printf("  [ RUN ] %s\n", c.name);
        const bool ok = c.fn();
        std::printf("  [ %s ] %s\n", ok ? "OK  " : "FAIL", c.name);
        all = all && ok;
    }
    return all;
}
