#!/usr/bin/env bash
# Run the queue-policy tests. They include a round trip through the real C++ loader
# (build/release/bin/lpq_run) that skips cleanly if the binary, the dimacs_ny graph or
# its profile is missing.
#
# Usage: scripts/policy/test_policy.sh [pytest args]
# Set LPQ_POLICY_CPUS=<cpu list> to keep the run off a core reserved for benchmarks.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT/python"

PY="${PYTHON:-}"
if [[ -z "$PY" ]]; then
    if [[ -x ../.venv/bin/python ]]; then PY=../.venv/bin/python; else PY=python3; fi
fi

run=(nice -n 10)
if [[ -n "${LPQ_POLICY_CPUS:-}" ]]; then
    run+=(taskset -c "$LPQ_POLICY_CPUS")
fi

exec "${run[@]}" "$PY" -m pytest -q tests/test_policy_core.py tests/test_policy_model.py \
    tests/test_policy_export.py "$@"
