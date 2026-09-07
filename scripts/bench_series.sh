#!/usr/bin/env bash
#
# Build a benchmark series: run titans_benchmark N times into a directory, one
# JSON per run, named so that lexical order is chronological order.
#
# WHY A SERIES AND NOT A RUN
# --------------------------
# `titans_benchmark` reports the spread across repetitions INSIDE one process.
# That is the right number for "is this measurement resolvable" and the wrong
# number for "did this commit make things slower", because repetitions inside a
# process share a cache state, a page mapping, a clock domain and a thermal
# state. Everything that differs between processes -- and between days -- is
# invisible to it.
#
# `titans_regression` needs a series precisely so it can estimate the noise at
# the timescale the gate operates at. See docs/REGRESSION.md.
#
# Usage:
#   scripts/bench_series.sh <out_dir> [runs] [--quick]
#
# In CI, one run per commit lands in the series instead; this script exists to
# build the local baseline and to make the false-positive property testable
# without waiting for a hundred commits.

set -u

OUT_DIR="${1:-results/benchmark_history/local}"
RUNS="${2:-30}"
shift $# 2>/dev/null || true

BUILD_DIR="${BUILD_DIR:-build}"
BENCH="$BUILD_DIR/titans_benchmark"

if [ ! -x "$BENCH" ]; then
  echo "error: $BENCH not found; build with -DTITANS_ENABLE_BENCHMARKS=ON" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

echo "Building a $RUNS-run series in $OUT_DIR"
echo "  NOTE: back-to-back runs share thermal and DVFS state, so the dispersion"
echo "  measured here is a LOWER BOUND on what CI will see across commits and"
echo "  days. titans_regression reports both and says which it used."

for i in $(seq 1 "$RUNS"); do
  SEQ=$(printf '%04d' "$i")
  STAMP=$(date +%s)
  if ! "$BENCH" --json "$OUT_DIR/bench_${SEQ}_${STAMP}.json" > /dev/null 2>&1; then
    echo "  run $SEQ failed" >&2
    exit 1
  fi
  printf '\r  %d/%d' "$i" "$RUNS"
done
echo
echo "  wrote $RUNS files"
