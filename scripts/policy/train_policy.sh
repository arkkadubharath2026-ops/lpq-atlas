#!/usr/bin/env bash
# Train, nested-evaluate and export the queue-selection policy.
#
# Reads the paired interleaved results under results/raw/policy/*.json (override with
# RESULTS), the engine-written profiles in datasets/processed and the dataset metadata,
# and writes models/policy/queue_policy_v1.lpqpolicy plus its .report.json. It refuses
# to overwrite either file: pass OUT=<new path> for a new version.
#
# Usage:
#   scripts/policy/train_policy.sh                       # defaults
#   OUT=models/policy/queue_policy_v2.lpqpolicy SEED=7 scripts/policy/train_policy.sh
#   scripts/policy/train_policy.sh --algorithm dijkstra --features p50_over_max,log2_mean_w
#
# Extra arguments are passed to `python -m lpq_tools.policy.train`.
#
# This is CPU-light but not free. If a pinned benchmark is running, keep this off its
# core, e.g.  LPQ_POLICY_CPUS=0,1,3 scripts/policy/train_policy.sh  (unset: only `nice`).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

PY="${PYTHON:-}"
if [[ -z "$PY" ]]; then
    if [[ -x .venv/bin/python ]]; then PY=.venv/bin/python; else PY=python3; fi
fi
OUT="${OUT:-models/policy/queue_policy_v1.lpqpolicy}"
SEED="${SEED:-20260923}"
RESULTS="${RESULTS:-results/raw/policy/*.json}"

if [[ -e "$OUT" ]]; then
    echo "error: $OUT already exists; refusing to overwrite (set OUT=<new path>)." >&2
    exit 2
fi

run=(nice -n 10)
if [[ -n "${LPQ_POLICY_CPUS:-}" ]]; then
    run+=(taskset -c "$LPQ_POLICY_CPUS")
fi

# RESULTS is deliberately unquoted: the shell expands the glob (the Python CLI also
# accepts a quoted glob).
# shellcheck disable=SC2086
PYTHONPATH="python${PYTHONPATH:+:$PYTHONPATH}" exec "${run[@]}" "$PY" -m lpq_tools.policy.train \
    --results $RESULTS \
    --profiles-dir datasets/processed \
    --metadata-dir datasets/metadata \
    --out "$OUT" \
    --seed "$SEED" \
    "$@"
