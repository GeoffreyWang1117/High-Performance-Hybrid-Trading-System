/**
 * @file dataset_main.cpp
 * @brief Label real Binance trades and audit the label for leakage.
 *
 * Prints, for one aggTrades file:
 *   1. the toxic-flow label distribution;
 *   2. how well each field OF THE EVENT ITSELF predicts the label -- this must
 *      be near chance, or the task is solvable without context and cannot
 *      discriminate between context-management strategies;
 *   3. how well a simple CONTEXT feature (recent signed order flow) predicts
 *      it -- this must beat chance, or the task is unlearnable and every
 *      strategy will look equally bad.
 *
 * The two together are the same necessary/sufficient pair that
 * test_task_design.cpp enforces on synthetic data, applied to real trades.
 * A dataset that fails either is not usable for this experiment, and this tool
 * exits non-zero so a pipeline notices.
 *
 * Usage:
 *   titans_dataset data/raw/BTCUSDT-aggTrades-2024-01-15.csv
 *       [--horizon-ms 1000] [--threshold-bps 5] [--max-rows N]
 */

#include "titans/context/binance_dataset.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

using namespace titans;
using namespace titans::context;

namespace {

/**
 * @brief Area under the ROC curve for a single continuous score.
 *
 * Computed via the rank-sum identity (Mann-Whitney U), which is exact and
 * needs no threshold sweep. 0.5 is chance; the value is symmetric, so a score
 * that predicts the label backwards shows up as < 0.5 and |AUC - 0.5| is the
 * quantity of interest.
 */
double auc(const std::vector<double>& scores, const std::vector<bool>& labels) {
    const size_t n = scores.size();
    if (n == 0 || n != labels.size()) return 0.5;

    std::vector<size_t> idx(n);
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(),
              [&](size_t a, size_t b) { return scores[a] < scores[b]; });

    // Average ranks over ties, so a constant score gives exactly 0.5 rather
    // than an artefact of sort order.
    std::vector<double> rank(n);
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j + 1 < n && scores[idx[j + 1]] == scores[idx[i]]) ++j;
        const double avg = 0.5 * (static_cast<double>(i) + static_cast<double>(j)) + 1.0;
        for (size_t k = i; k <= j; ++k) rank[idx[k]] = avg;
        i = j + 1;
    }

    double sum_pos_rank = 0.0;
    size_t n_pos = 0;
    for (size_t k = 0; k < n; ++k) {
        if (labels[k]) { sum_pos_rank += rank[k]; ++n_pos; }
    }
    const size_t n_neg = n - n_pos;
    if (n_pos == 0 || n_neg == 0) return 0.5;

    return (sum_pos_rank - n_pos * (n_pos + 1) / 2.0) /
           (static_cast<double>(n_pos) * static_cast<double>(n_neg));
}

struct Options {
    std::string path;
    int64_t horizon_ms = 1000;
    double threshold_bps = 5.0;
    size_t max_rows = 0;
    int flow_window = 50;   ///< trades of history in the context feature
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--horizon-ms" && i + 1 < argc)        o.horizon_ms = std::atoll(argv[++i]);
        else if (a == "--threshold-bps" && i + 1 < argc) o.threshold_bps = std::atof(argv[++i]);
        else if (a == "--max-rows" && i + 1 < argc)      o.max_rows = std::atoll(argv[++i]);
        else if (a == "--flow-window" && i + 1 < argc)   o.flow_window = std::atoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::printf("Usage: titans_dataset <aggTrades.csv> [--horizon-ms N] "
                        "[--threshold-bps X] [--max-rows N] [--flow-window N]\n");
            std::exit(0);
        }
        else if (o.path.empty()) o.path = a;
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    const Options opts = parse(argc, argv);
    if (opts.path.empty()) {
        std::fprintf(stderr,
            "usage: titans_dataset <aggTrades.csv>\n\n"
            "Get data with:\n"
            "  python python/data/fetch_binance.py --symbol BTCUSDT --date 2024-01-15\n");
        return 2;
    }

    ToxicFlowLabelConfig cfg;
    cfg.horizon_ms = opts.horizon_ms;
    cfg.threshold_bps = opts.threshold_bps;

    BinanceToxicFlowDataset ds(cfg);
    std::printf("Loading %s\n", opts.path.c_str());
    if (!ds.load(opts.path, opts.max_rows)) {
        std::fprintf(stderr, "ERROR: %s\n", ds.error().c_str());
        return 1;
    }

    const auto events = ds.build_events();
    std::printf("\nLabel: toxic if the mid moves >= %.1f bps in the aggressor's\n"
                "favour within %ld ms of the trade.\n\n",
                cfg.threshold_bps, static_cast<long>(cfg.horizon_ms));
    ds.stats().print();

    if (events.empty()) {
        std::fprintf(stderr, "\nERROR: no labelled events\n");
        return 1;
    }

    // ----------------------------------------------------------------------
    // Leakage audit: can the label be read off the event itself?
    // ----------------------------------------------------------------------
    const auto& trades = ds.trades();
    std::vector<bool> labels;
    std::vector<double> f_quantity, f_price, f_side, f_flow;
    labels.reserve(events.size());

    // Signed order-flow imbalance over a trailing window: a CONTEXT feature.
    // It needs history, which is the whole point -- it is the thing a context
    // strategy either preserves or loses.
    std::vector<double> signed_qty;
    signed_qty.reserve(trades.size());
    for (const auto& t : trades) {
        signed_qty.push_back(t.aggressor_sign() * t.quantity);
    }

    size_t ti = 0;
    for (const auto& e : events) {
        // events are a prefix-aligned subsequence of trades, in order
        while (ti < trades.size() &&
               "agg_" + std::to_string(trades[ti].agg_trade_id) != e.event_id) {
            ++ti;
        }
        if (ti >= trades.size()) break;

        const auto& t = trades[ti];
        labels.push_back(e.is_anomaly);
        f_quantity.push_back(t.quantity);
        f_price.push_back(t.price);
        f_side.push_back(static_cast<double>(t.aggressor_sign()));

        double flow = 0.0;
        const size_t lo = ti >= static_cast<size_t>(opts.flow_window)
                              ? ti - static_cast<size_t>(opts.flow_window) : 0;
        for (size_t k = lo; k < ti; ++k) flow += signed_qty[k];
        f_flow.push_back(t.aggressor_sign() * flow);   // flow aligned with the aggressor
        ++ti;
    }

    const double auc_qty  = auc(f_quantity, labels);
    const double auc_px   = auc(f_price, labels);
    const double auc_side = auc(f_side, labels);
    const double auc_flow = auc(f_flow, labels);

    auto dev = [](double a) { return std::abs(a - 0.5); };

    std::printf("\nLEAKAGE AUDIT (AUC; 0.5 = chance)\n");
    std::printf("  event-only features -- these must stay near chance:\n");
    std::printf("    trade quantity          %.4f   (|dev| %.4f)\n", auc_qty, dev(auc_qty));
    std::printf("    trade price             %.4f   (|dev| %.4f)\n", auc_px, dev(auc_px));
    std::printf("    aggressor side          %.4f   (|dev| %.4f)\n", auc_side, dev(auc_side));
    std::printf("  context feature -- this must beat chance:\n");
    std::printf("    signed flow, last %-3d   %.4f   (|dev| %.4f)\n",
                opts.flow_window, auc_flow, dev(auc_flow));

    // ----------------------------------------------------------------------
    // Verdict
    // ----------------------------------------------------------------------
    bool ok = true;
    std::printf("\nVERDICT\n");

    const double kLeakLimit = 0.10;      // AUC within [0.40, 0.60]
    const double kLearnFloor = 0.02;     // context must clear chance by this
    struct Check { const char* name; double d; };
    bool marginal = false;
    for (const Check& c : {Check{"trade quantity", dev(auc_qty)},
                           Check{"trade price", dev(auc_px)},
                           Check{"aggressor side", dev(auc_side)}}) {
        if (c.d > kLeakLimit) {
            std::printf("  [LEAK] %s alone reaches AUC deviation %.4f (limit %.2f).\n"
                        "         The label is partly readable from the event, so\n"
                        "         context strategies cannot be separated on this data.\n",
                        c.name, c.d, kLeakLimit);
            ok = false;
        } else if (c.d > 0.8 * kLeakLimit) {
            // Passing by a small margin is worth saying out loud. A threshold
            // that a dataset only just clears is one bad session away from
            // failing, and a reader deserves to know the residual is there
            // rather than see an unqualified "pass".
            std::printf("  [near] %s sits at AUC deviation %.4f, within 20%% of the\n"
                        "         %.2f limit. Residual signal remains -- typically\n"
                        "         drift that varies within the session and is not\n"
                        "         fully removed by a single sample-mean correction.\n"
                        "         Treat small strategy differences on this data with\n"
                        "         suspicion.\n",
                        c.name, c.d, kLeakLimit);
            marginal = true;
        }
    }
    if (ok) {
        std::printf("  [pass] No single event field clears the leakage limit%s\n",
                    marginal ? ", though see the margin noted above."
                             : "; a model must consult context to beat chance.");
    }

    if (dev(auc_flow) < kLearnFloor) {
        std::printf("  [FLAT] Trailing signed flow reaches only %.4f deviation from\n"
                    "         chance. If context carries no signal either, every\n"
                    "         strategy scores the same and the comparison is empty.\n"
                    "         Try a shorter horizon or a lower bps threshold.\n",
                    dev(auc_flow));
        ok = false;
    } else {
        std::printf("  [pass] Trailing signed order flow predicts the label above\n"
                    "         chance (AUC %.4f), so the task is learnable from\n"
                    "         context and strategies have room to differ.\n", auc_flow);
    }

    std::printf("\n%s\n", ok
        ? "Dataset is usable for context-management experiments."
        : "Dataset is NOT usable as configured; see the failures above.");
    return ok ? 0 : 1;
}
