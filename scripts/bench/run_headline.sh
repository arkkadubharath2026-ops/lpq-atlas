#!/usr/bin/env bash
# Headline waterfall: classical baselines -> original LPQ -> optimised non-ML LPQ
# -> lpq_optimized (autotuned), paired and interleaved, one pinned core, alone.
#
#   scripts/bench/run_headline.sh [core]        # default core 2
#
# Raw results go to results/raw/headline/ and are never overwritten. Run it with
# nothing else benchmarking: concurrent load on a shared host changes the numbers.
set -euo pipefail
cd "$(dirname "$0")/../.."
CORE="${1:-2}"
BIN=build/release/bin/lpq_benchmark
OUT=results/raw/headline
mkdir -p "$OUT"

COMPARE="radix_heap;binary_heap;four_ary_heap;lpq;lpq_direct:front=0@lpq_direct_optimized_nonml;bucket_queue@bucket_legacy_w37;lpq_optimized;replay_floor"

run() { # dataset algorithm manifest [extra args...]
    local ds="$1" alg="$2" manifest="$3"; shift 3
    local out="$OUT/${ds}_${alg}.json"
    echo "== ${ds} ${alg}"
    "$BIN" --dataset "datasets/processed/${ds}.lpqgraph" --queries-file "$manifest" \
        --algorithm "$alg" --repetitions 7 --warmup 3 --cpu-core "$CORE" --seed 20260923 \
        --interleave --experiment-id "headline-${ds}-${alg}" --output "$out" \
        --compare "$COMPARE" "$@"
}

run dimacs_ny   dijkstra experiments/manifests/dimacs_ny_queries_100.yaml
run dimacs_ny   astar    experiments/manifests/dimacs_ny_queries_100.yaml --heuristic euclidean
run snap_facebook dijkstra experiments/manifests/snap_facebook_queries_first100.yaml
run dimacs_col  dijkstra experiments/manifests/dimacs_col_queries_first30.yaml
echo HEADLINE-DONE
