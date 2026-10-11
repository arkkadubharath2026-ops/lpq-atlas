#!/usr/bin/env bash
# =============================================================================
# reproduce_benchmark: a baseline-vs-optimized latency benchmark on this machine.
#
#   scripts/demo/reproduce_benchmark.sh [options]
#
#   --repetitions N   measured repetitions per query            (default 11)
#   --warmup N        discarded warm-up repetitions              (default 3)
#   --cpu-core N      pin the benchmark to core N (default: the binary picks a
#                     core and avoids core 0; it reports [UNPINNED] if it cannot)
#   --compare SPEC    configurations, "name[:w=N,ring=N,front=N][@label];..."
#   --dataset ID      add a dataset (repeatable). Default: demo_road, plus
#                     dimacs_ny when datasets/processed/dimacs_ny.lpqgraph exists.
#
# Uses lpq_benchmark --interleave: queries in the outer loop and the order of the
# configurations shuffled per query (seeded), so slow drift of the host (thermal,
# frequency scaling, other processes) hits every configuration equally. Every
# answer is checked against the trusted reference search; a wrong path aborts.
#
# Output: one NEW file per dataset, results/raw/local/<UTC time>-<dataset>.json.
# Raw results are immutable (the binary refuses to overwrite) and git-ignored.
#
# Timings are specific to this machine. Cloud and virtual-machine timings are
# development-stage evidence, not publication results.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/../setup/_common.sh"

reps=11 warmup=3 core="" datasets=()
# Baseline first, then the other classical queues, then the learned queue. Which
# of these is fastest is exactly what this run measures; none is assumed to win.
spec="binary_heap@baseline;pairing_heap@pairing_heap;bucket_queue@bucket_queue;lpq_direct@lpq_direct"
while [ $# -gt 0 ]; do
    case "$1" in
        --repetitions) reps="$2"; shift 2 ;;
        --warmup) warmup="$2"; shift 2 ;;
        --cpu-core) core="$2"; shift 2 ;;
        --compare) spec="$2"; shift 2 ;;
        --dataset) datasets+=("$2"); shift 2 ;;
        -h | --help) sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done

cd "$ROOT"
engine_built || die "the engine is not built. Run scripts/setup/setup.sh first."
require_tool lpq_benchmark
[ -n "$(venv_python)" ] || die "no .venv found. Run scripts/setup/setup.sh first."
BENCH="$(tool_path lpq_benchmark)"

if [ ${#datasets[@]} -eq 0 ]; then
    datasets=(demo_road)
    if [ -f "$ROOT/datasets/processed/dimacs_ny.lpqgraph" ]; then
        datasets+=(dimacs_ny)
    else
        echo "note: dimacs_ny is not prepared, so only the demo graph is benchmarked."
        echo "      To add the real New York road network (downloads ~6 MB):"
        echo "        python scripts/data/prepare_datasets.py dimacs_ny"
    fi
fi

# The demo graph is generated from committed files; prepare it if setup has not.
if [ ! -f "$ROOT/datasets/processed/demo_road.lpqgraph" ] && [[ " ${datasets[*]} " == *" demo_road "* ]]; then
    say "preparing the demo dataset"
    "$PYTHON" scripts/demo/prepare_demo.py
fi

stamp="$(date -u +%Y%m%dT%H%M%SZ)"
outdir="$ROOT/results/raw/local"
mkdir -p "$outdir"
written=()

for ds in "${datasets[@]}"; do
    require_dataset "$ds"
    manifest="$ROOT/experiments/manifests/${ds}_queries_100.yaml"
    [ -f "$manifest" ] || manifest="$ROOT/experiments/manifests/${ds}_queries.yaml"
    require_file "$manifest" "query manifest for $ds" "python scripts/data/prepare_datasets.py $ds"
    out="$outdir/${stamp}-${ds}.json"

    args=(--algorithm dijkstra --dataset "$ROOT/datasets/processed/${ds}.lpqgraph"
        --queries-file "$manifest" --repetitions "$reps" --warmup "$warmup"
        --interleave --compare "$spec" --experiment-id "local-${stamp}-${ds}" --output "$out")
    [ -n "$core" ] && args+=(--cpu-core "$core")

    say "benchmark: $ds   ($(basename "$manifest"), $reps repetitions, $warmup warm-up)"
    "$BENCH" "${args[@]}" || die "lpq_benchmark failed for $ds (see the output above)"
    written+=("$out")
    echo
    "$PYTHON" scripts/demo/summarize_results.py "$out" || warn "summarize_results.py reported a problem for $out"
done

echo
say "done"
echo "  Raw results (new files, never overwritten):"
for f in "${written[@]}"; do echo "    $f"; done
cat << 'EOF'

  How to read the table above: "median ns" is the median over queries of each
  query's median time; "q-MAD" is the spread across queries (workload
  heterogeneity, not noise); "noise" is the within-query relative MAD. Compare
  rows within one table only. Timings depend on this machine; if any row says
  [UNPINNED] the benchmark could not pin itself to a core and the numbers are
  noisier. See docs/local-setup.md ("Reading the benchmark output").
EOF
