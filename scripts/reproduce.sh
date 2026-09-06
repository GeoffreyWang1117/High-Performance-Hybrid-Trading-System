#!/usr/bin/env bash
#
# Regenerate every number this repository claims.
#
# A README full of measurements is only as good as the reader's ability to
# re-derive them. This script runs each claim's source and prints what it got,
# so a discrepancy shows up as a discrepancy rather than as a number nobody
# checked.
#
# Usage:
#   scripts/reproduce.sh              # everything that needs no network or GPU
#   scripts/reproduce.sh --with-data  # also download real market data (~18 MB)
#   scripts/reproduce.sh --with-walkforward   # 28 days of data (~2.8 GB), ~8 min
#   scripts/reproduce.sh --with-llm   # also run the live-model experiment
#
# Exit code is the number of stages that failed.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
RESULTS_DIR="${RESULTS_DIR:-results}"
WITH_DATA=0
WITH_LLM=0
WITH_WF=0
LLM_PORT="${LLM_PORT:-8000}"
LLM_MODEL="${LLM_MODEL:-}"
DATA_DATE="${DATA_DATE:-2024-01-15}"
SYMBOL="${SYMBOL:-BTCUSDT}"
WF_RANGE="${WF_RANGE:-2024-01-08:2024-02-04}"

for arg in "$@"; do
  case "$arg" in
    --with-data) WITH_DATA=1 ;;
    --with-walkforward) WITH_WF=1 ;;
    --with-llm)  WITH_LLM=1; WITH_DATA=1 ;;
    --help|-h)
      sed -n '3,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

FAILURES=0
STAGE=0

stage() {
  STAGE=$((STAGE + 1))
  printf '\n\033[1m=== [%d] %s ===\033[0m\n' "$STAGE" "$1"
}

check() {
  # check <description> <command...>
  local desc="$1"; shift
  if "$@"; then
    printf '  \033[32mok\033[0m   %s\n' "$desc"
  else
    printf '  \033[31mFAIL\033[0m %s (exit %d)\n' "$desc" "$?"
    FAILURES=$((FAILURES + 1))
  fi
}

# ---------------------------------------------------------------------------
stage "Build"
# ---------------------------------------------------------------------------
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DTITANS_ENABLE_TESTS=ON \
    >/dev/null || { echo "cmake configure failed"; exit 1; }
cmake --build "$BUILD_DIR" -j"$(nproc)" >/dev/null || {
  echo "build failed"; exit 1; }
echo "  built $(ls "$BUILD_DIR"/titans_* 2>/dev/null | wc -l) executables"

# ---------------------------------------------------------------------------
stage "Unit tests (README: 12 modules)"
# ---------------------------------------------------------------------------
"$BUILD_DIR"/tests/titans_tests > /tmp/titans_tests.out 2>&1
TEST_EXIT=$?
grep -E '^(Passed|Failed):' /tmp/titans_tests.out | sed 's/^/  /'
if [ $TEST_EXIT -ne 0 ]; then
  grep -E '✗' /tmp/titans_tests.out | sed 's/^/  /'
  FAILURES=$((FAILURES + 1))
fi

echo "  --- claims re-derived from this run ---"
grep -E 'best context-free global rule|per-entity rolling median' /tmp/titans_tests.out | sed 's/^/  /'
grep -E 'adversarial writer:|fast-lane p99' /tmp/titans_tests.out | sed 's/^/  /'
grep -E 'Student-t p=' /tmp/titans_tests.out | sed 's/^/  /'
grep -E 'drift_adjust=' /tmp/titans_tests.out | sed 's/^/  /'

# ---------------------------------------------------------------------------
stage "Fast-path benchmarks (README latency table)"
# ---------------------------------------------------------------------------
mkdir -p "$RESULTS_DIR"
BENCH_JSON="$RESULTS_DIR/benchmark_$(hostname)_$(date +%Y%m%d).json"
"$BUILD_DIR"/titans_benchmark --json "$BENCH_JSON" > /tmp/titans_bench.out 2>&1
BENCH_EXIT=$?
if [ $BENCH_EXIT -ne 0 ]; then
  echo "  benchmark self-check FAILED; the harness is not calibrated here"
  tail -20 /tmp/titans_bench.out | sed 's/^/  /'
  FAILURES=$((FAILURES + 1))
else
  sed -n '/SECTION 0/,/^$/p' /tmp/titans_bench.out | sed 's/^/  /'
  grep -E 'SPSCQueue|ObjectPool|EventBus|L2OrderBook|operator new' \
      /tmp/titans_bench.out | sed 's/^/  /'
  echo "  written: $BENCH_JSON"
  echo
  echo "  NOTE: these will not match the README exactly, and are not supposed"
  echo "  to. Absolute latency depends on what else the host is doing; the"
  echo "  'spread' column is the honest indicator (median vs interference-free"
  echo "  minimum). What must hold across machines is the ORDERING and the"
  echo "  order of magnitude: pool allocation an order below malloc, queue"
  echo "  operations in single-digit ns, EventBus dominated by its clock read."
  echo "  The caveats block in $BENCH_JSON records the host state."
fi

# ---------------------------------------------------------------------------
stage "Contamination experiment (assumed-degradation stand-in)"
# ---------------------------------------------------------------------------
"$BUILD_DIR"/titans_experiment > /tmp/titans_exp.out 2>&1
check "titans_experiment ran" test $? -eq 0
echo "  --- ablation table, including INERT rows ---"
sed -n '/^Configuration/,/^$/p' /tmp/titans_exp.out | sed 's/^/  /'
sed -n '/were INERT/,/^$/p' /tmp/titans_exp.out | sed 's/^/  /'

# ---------------------------------------------------------------------------
stage "Honesty gates (these MUST fail)"
# ---------------------------------------------------------------------------
# Each of these represents a way the repository used to produce numbers from
# nothing. They are asserted to fail, so a regression that restores the silent
# fallback shows up here.
"$BUILD_DIR"/titans_llm_experiment --backend vllm --port 59999 >/dev/null 2>&1
check "titans_llm_experiment refuses to run with no backend" test $? -ne 0

python3 python/research/generate_figures.py --results /nonexistent >/dev/null 2>&1
check "generate_figures.py refuses to fabricate data" test $? -ne 0

"$BUILD_DIR"/titans_engine --config /nonexistent.json >/dev/null 2>&1
check "titans_engine refuses an unreadable --config" test $? -ne 0

# ---------------------------------------------------------------------------
if [ "$WITH_DATA" -eq 1 ]; then
stage "Real market data (README leakage audit)"
# ---------------------------------------------------------------------------
  python3 python/data/fetch_binance.py --symbol "$SYMBOL" --date "$DATA_DATE" \
      --out data/raw > /tmp/titans_fetch.out 2>&1
  check "fetch + sha256 verify" test $? -eq 0
  grep -E 'sha256 verified|manifest' /tmp/titans_fetch.out | sed 's/^/  /'

  CSV="data/raw/${SYMBOL}-aggTrades-${DATA_DATE}.csv"
  # Audit the WHOLE file: the drift correction is a sample-mean statistic and a
  # prefix gives a different, worse answer. See docs/ARCHITECTURE.md.
  "$BUILD_DIR"/titans_dataset "$CSV" > /tmp/titans_dataset.out 2>&1
  check "leakage audit passes on the full session" test $? -eq 0
  sed -n '/LEAKAGE AUDIT/,/^$/p;/VERDICT/,/^$/p' /tmp/titans_dataset.out | sed 's/^/  /'

  stage "End-to-end lane replay: what the slow lane ships"
  # The controlled comparison. Same rule, same trades, same threshold to four
  # significant figures; the only difference is whether the slow lane ships the
  # DECISION or the PARAMETER behind it.
  for KIND in decision parameter; do
    "$BUILD_DIR"/titans_lanes "$CSV" --max-rows 100000 --speed 500 --ttl-ms 1000 \
        --repeat 5 --advisory "$KIND" > "/tmp/titans_lanes_$KIND.out" 2>&1
    echo "  --- --advisory $KIND (exit $?) ---"
    grep -E 'declared a horizon|SLO: offered-age|survived delivery|median |RESOLVED' \
        "/tmp/titans_lanes_$KIND.out" | sed 's/^ */    /'
  done

  # Shipping a decision must BREACH its freshness SLO: it declares the signal's
  # 1000 ms horizon and arrives at a p99 age above it. That is the finding, so
  # a run where it suddenly passes means the contract stopped being enforced.
  "$BUILD_DIR"/titans_lanes "$CSV" --max-rows 100000 --speed 500 --ttl-ms 1000 \
      --repeat 3 --advisory decision >/dev/null 2>&1
  check "shipping a decision breaches its freshness SLO (exit 5)" test $? -eq 5

  # Shipping a parameter must pass: a calibrated threshold declares the horizon
  # it was estimated over, and the same age is well inside it.
  "$BUILD_DIR"/titans_lanes "$CSV" --max-rows 100000 --speed 500 --ttl-ms 1000 \
      --repeat 3 --advisory parameter >/dev/null 2>&1
  check "shipping a parameter meets its freshness SLO" test $? -eq 0
fi

# ---------------------------------------------------------------------------
if [ "$WITH_WF" -eq 1 ]; then
stage "Walk-forward (README: out-of-sample policy evaluation)"
# ---------------------------------------------------------------------------
  echo "  fetching $WF_RANGE -- about 2.8 GB, skipped where already present"
  python3 python/data/fetch_binance.py --symbol "$SYMBOL" --dates "$WF_RANGE" \
      --out data/raw > /tmp/titans_wf_fetch.out 2>&1
  check "fetch + sha256 verify every day in the range" test $? -eq 0
  grep -cE 'sha256 verified|already present' /tmp/titans_wf_fetch.out \
      | sed 's/^/  days available: /'

  mkdir -p "$RESULTS_DIR/walkforward"
  WF_JSON="$RESULTS_DIR/walkforward/${SYMBOL}_$(echo "$WF_RANGE" | tr ':' '_').json"
  # shellcheck disable=SC2086
  "$BUILD_DIR"/titans_walkforward data/raw/${SYMBOL}-aggTrades-*.csv \
      --json "$WF_JSON" > /tmp/titans_wf.out 2>&1
  WF_EXIT=$?
  check "walk-forward ran without a protocol violation" test $WF_EXIT -eq 0
  sed -n '/LABEL AUDIT/,$p' /tmp/titans_wf.out | sed 's/^/  /'
  echo
  echo "  NOTE: the out-of-sample number here is measured WITHOUT advisory"
  echo "  staleness, so it is an upper bound on what the live lane can reach."
  echo "  titans_lanes measures the same policy with staleness and does not"
  echo "  resolve it. The gap between the two is the cost of delivery, and it"
  echo "  is the most interesting number this repository produces."
fi

# ---------------------------------------------------------------------------
if [ "$WITH_LLM" -eq 1 ]; then
stage "Live-model contamination experiment"
# ---------------------------------------------------------------------------
  if ! curl -s -m 5 -o /dev/null "http://localhost:${LLM_PORT}/v1/models"; then
    echo "  no backend on port ${LLM_PORT}; start one:"
    echo "    vllm serve <model> --port ${LLM_PORT}"
    echo "    or python python/serving/cpu_shim.py --port ${LLM_PORT}"
    FAILURES=$((FAILURES + 1))
  else
    ARGS=(--backend vllm --port "$LLM_PORT" --events 400 --entities 12
          --contamination 0.4 --seed 42 --out "$RESULTS_DIR/llm")
    [ -n "$LLM_MODEL" ] && ARGS+=(--model "$LLM_MODEL")
    "$BUILD_DIR"/titans_llm_experiment "${ARGS[@]}" > /tmp/titans_llm.out 2>&1
    LLM_EXIT=$?
    sed -n '/^Method/,/^$/p' /tmp/titans_llm.out | sed 's/^/  /'
    if [ $LLM_EXIT -eq 3 ]; then
      echo "  model is degenerate; deltas above are meaningless (exit 3)"
      sed -n '/DEGENERATE MODEL/,/^$/p' /tmp/titans_llm.out | sed 's/^/  /'
      FAILURES=$((FAILURES + 1))
    elif [ $LLM_EXIT -ne 0 ]; then
      echo "  experiment failed (exit $LLM_EXIT)"
      FAILURES=$((FAILURES + 1))
    fi
  fi
fi

# ---------------------------------------------------------------------------
printf '\n\033[1m=== Summary ===\033[0m\n'
if [ "$FAILURES" -eq 0 ]; then
  printf '  \033[32mAll %d stages reproduced.\033[0m\n' "$STAGE"
else
  printf '  \033[31m%d of %d stages failed.\033[0m\n' "$FAILURES" "$STAGE"
fi
if [ "$WITH_DATA" -eq 0 ]; then
  echo "  (market-data and lane-replay stages skipped; pass --with-data)"
fi
if [ "$WITH_WF" -eq 0 ]; then
  echo "  (walk-forward stage skipped; pass --with-walkforward -- 2.8 GB, ~8 min)"
fi
if [ "$WITH_LLM" -eq 0 ]; then
  echo "  (live-model stage skipped; pass --with-llm)"
fi
exit "$FAILURES"
