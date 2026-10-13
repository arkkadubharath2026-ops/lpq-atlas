#!/usr/bin/env bash
# =============================================================================
# Reports which hardware performance counters are actually available.
#
# ESD.md section 20 requires hardware counters to be collected "where
# supported", and PRD.md section 20 requires unavailable counters to be
# documented rather than quietly omitted. This script is the check that decides
# which of those two applies on the current host, so that a profiling run either
# produces real counters or fails loudly.
#
# Exit codes:
#   0  a CPU PMU is present and hardware counters can be collected
#   1  no CPU PMU; only software events are available
#   2  perf itself is missing
# =============================================================================
set -uo pipefail

echo "=== host ==="
uname -srm
if [ -r /proc/version ]; then
    if grep -qi microsoft /proc/version; then
        echo "kernel flavour: WSL2 (hardware counters are not exposed by the hypervisor)"
    fi
fi

echo
echo "=== perf event sources ==="
if [ -d /sys/bus/event_source/devices ]; then
    ls /sys/bus/event_source/devices/
else
    echo "(/sys/bus/event_source/devices is absent)"
fi

echo
echo "=== perf_event_paranoid ==="
cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "(unreadable)"

echo
if ! command -v perf > /dev/null 2>&1; then
    echo "RESULT: perf is not installed."
    exit 2
fi

# The presence of a `cpu` (or hybrid cpu_core/cpu_atom) event source is the
# actual test. perf_event_paranoid only governs permission; a restricted-but-
# present PMU can be unlocked, whereas an absent one cannot.
if ls /sys/bus/event_source/devices/ 2>/dev/null | grep -qE '^(cpu|cpu_core|cpu_atom)$'; then
    echo "=== perf stat probe ==="
    perf stat -e cycles,instructions,branch-misses,cache-misses true 2>&1 | tail -12
    echo
    echo "RESULT: CPU PMU present. Hardware counters are collectable."
    exit 0
fi

echo "=== perf stat probe (expected to report <not supported>) ==="
perf stat -e cycles,instructions,branch-misses,cache-misses true 2>&1 | tail -12
echo
cat <<'EOF'
RESULT: no CPU PMU on this host. Hardware counters are NOT collectable.

Cycles, instructions, branch misses, cache misses, LLC misses and memory
bandwidth cannot be measured here and must be reported as "not measured",
never estimated. See docs/experiments/profiling.md for the alternatives
(native Linux, or Intel VTune on Windows).

Wall-clock runtime, priority-queue operation counts, ML latency breakdowns
and peak RSS are unaffected and remain fully available.
EOF
exit 1
