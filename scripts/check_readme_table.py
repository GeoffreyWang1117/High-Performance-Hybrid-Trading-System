#!/usr/bin/env python3
"""Assert the README's latency table still matches the artifact it names.

WHY THIS EXISTS
---------------
Because the table once did not, and for ten days nobody could have known.

The nine rows under "Measured performance" were taken from
results/benchmark_GW-X570-Taichi_20260830.json at commit 24dc066. Commit
f5e59b4 -- the one that introduced scripts/reproduce.sh, whose entire purpose is
to re-derive every number in the README -- ran the benchmark and wrote it to
results/benchmark_$(hostname)_$(date +%Y%m%d).json, which on that day was the
same path. The artifact the table cites was overwritten by the script written to
verify it.

Every other claim in this repository is checked by re-running its source and
comparing. This one could not be, because a latency table is not re-derivable --
absolute latency depends on what else the host was doing, which is why the
README says so at length. What IS checkable is that the prose still agrees with
the file it points at, and that is the only thing this script does.

Usage:
  scripts/check_readme_table.py README.md results/<the artifact the README names>

Exit 0 when every row agrees on cost, minimum and throughput; 1 otherwise, with
each disagreement named.
"""
import json, re, sys

readme, artifact = sys.argv[1], sys.argv[2]

# The README names operations in markdown; the JSON names them as the harness
# does. One map, stated here rather than guessed by fuzzy matching.
ALIAS = {
    "SPSCQueue::try_push":                    "SPSCQueue::try_push",
    "SPSCQueue::try_pop":                     "SPSCQueue::try_pop",
    "ObjectPool::allocate":                   "ObjectPool::allocate",
    "ObjectPool::deallocate":                 "ObjectPool::deallocate",
    "operator new/delete (baseline)":         "operator new/delete (baseline)",
    "L2OrderBook::update_level":              "L2OrderBook::update_level (steady state)",
    "L2OrderBook::best_bid":                  "L2OrderBook::best_bid",
    "EventBus::publish (pre-stamped)":        "EventBus::publish (timestamp preset)",
    "EventBus::publish (auto-timestamp)":     "EventBus::publish (auto-timestamp)",
}


def normalise(label):
    """Strip markdown so `operator new`/`delete` *(baseline)* is one name."""
    label = label.replace("`", "").replace("*", "")
    return re.sub(r"\s+", " ", label).strip()

d = json.load(open(artifact))
by_name = {r["name"]: r for r in d["results"]}

row = re.compile(
    r"^\|\s*(.+?)\s*\|\s*([\d.]+) ns \(([\d.]+)\)\s*\|\s*(\d+) M ops/s\s*\|")

checked, problems = 0, []
for line in open(readme):
    m = row.match(line)
    if not m:
        continue
    label = normalise(m.group(1))
    cost, mn, tput = float(m.group(2)), float(m.group(3)), int(m.group(4))
    key = ALIAS.get(label)
    if key is None:
        continue                      # a table row that is not a benchmark row
    r = by_name.get(key)
    if r is None:
        problems.append(f"{label}: not in the artifact")
        continue
    checked += 1
    if abs(r["cost_ns"] - cost) > 0.005:
        problems.append(f"{label}: README {cost} ns, artifact {r['cost_ns']:.4f} ns")
    if abs(r["min_ns"] - mn) > 0.005:
        problems.append(f"{label}: README min {mn} ns, artifact {r['min_ns']:.4f} ns")
    if abs(r["ops_per_sec"] / 1e6 - tput) > 0.5:
        problems.append(f"{label}: README {tput} M ops/s, artifact {r['ops_per_sec']/1e6:.1f} M")

if checked != len(ALIAS):
    problems.append(f"matched {checked} of {len(ALIAS)} rows; the table's format moved")
if problems:
    print("  README table does NOT match the artifact it names:")
    for p in problems:
        print(f"    {p}")
    sys.exit(1)
print(f"  README table matches {artifact} on all {checked} rows "
      f"(cost, minimum and throughput)")
