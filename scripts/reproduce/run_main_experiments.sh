#!/usr/bin/env bash
# =============================================================================
# Run the experiments the findings rest on (ESD.md section 36).
#
#   scripts/reproduce/run_main_experiments.sh
#
# Datasets are NOT downloaded here. Acquisition is a separate, network-dependent
# step with its own checksum verification (scripts/data/prepare_datasets.py),
# and folding it in would mean a reproduction that silently re-downloads several
# gigabytes, or silently proceeds with whatever happens to be on disk. This
# script checks the datasets are present and correct, and stops if they are not.
#
# WHAT IS AND IS NOT PINNED HERE
#
# Every seed is explicit. The trained model is rebuilt from the training set and
# TRAINING_SEED rather than reused from models/exported/, which is git-ignored:
# a reproduction that depended on an unversioned artifact would reproduce
# whatever happened to be lying there.
#
# Timings will not match the recorded ones and are not expected to. The machine
# differs. The comparison counts WILL match exactly -- they are deterministic
# properties of the algorithm and the query set -- and that is the check worth
# making, so the script makes it.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

require_python
require_tool lpq_benchmark
require_tool lpq_collect

BENCHMARK="$(tool_path lpq_benchmark)"
COLLECT="$(tool_path lpq_collect)"

mkdir -p "$RESULTS_DIR" "${MODELS_DIR}/training" "${MODELS_DIR}/exported"

# -----------------------------------------------------------------------------
say "checking datasets"
# -----------------------------------------------------------------------------
for dataset in "${MAIN_DATASETS[@]}"; do
    require_dataset "$dataset"
    printf "    %s\n" "$dataset"
done

# The query manifests carry the graph checksum and refuse a graph they were not
# generated against, so this check runs through the tool that enforces it rather
# than reimplementing it here.
say "verifying each query manifest matches its graph"
for dataset in "${MAIN_DATASETS[@]}"; do
    manifest="${REPO_ROOT}/experiments/manifests/${dataset}_queries_100.yaml"
    [ -f "$manifest" ] || manifest="${REPO_ROOT}/experiments/manifests/${dataset}_queries.yaml"
    require_file "$manifest" "query manifest for ${dataset}" \
        "python scripts/data/prepare_datasets.py ${dataset}"

    # A one-query run is enough: the manifest is validated against the graph
    # before any query runs, so a checksum mismatch fails immediately.
    "$BENCHMARK" --dataset "${REPO_ROOT}/datasets/processed/${dataset}.lpqgraph" \
        --queries-file "$manifest" --pq binary_heap --repetitions 3 --warmup 1 \
        > /dev/null 2>&1 ||
        die "the query manifest for ${dataset} does not match its graph.
       Regenerate both: python scripts/data/prepare_datasets.py ${dataset}"
    printf "    %s\n" "$dataset"
done

# -----------------------------------------------------------------------------
say "collecting training sets"
# -----------------------------------------------------------------------------
# ESD.md section 30: test-query outcomes must never influence model parameters.
#
# The training labels come from a query set that is disjoint from the benchmark.
# "Disjoint" is checked at SOURCE level, not pair level: lpq_collect labels from a
# full single-source search per source, so a training query that shares a source
# with a benchmark query reuses that search whatever its target is. An earlier
# version of this pipeline trained on the benchmark queries themselves, which
# inflated the measured speedup by about a third
# (docs/experiments/ablation-waterfall.md); a later one used a set that shared no
# (source, target) pair but did share sources on snap_facebook
# (docs/experiments/static-scorer-training.md, "Protocols compared").
# select_training_manifest (common.sh) picks the set and refuses to continue if
# it shares a source with any benchmark manifest.
declare -A TRAINING_MANIFEST
for dataset in "${MAIN_DATASETS[@]}"; do
    manifest="$(select_training_manifest "$dataset")"
    TRAINING_MANIFEST["$dataset"]="$manifest"
    printf "    %s\n" "$(basename "$manifest")"

    "$COLLECT" --dataset "${REPO_ROOT}/datasets/processed/${dataset}.lpqgraph" \
        --queries-file "$manifest" \
        --output "${MODELS_DIR}/training/${dataset}.lpqtrain" > /dev/null ||
        die "collecting the training set for ${dataset} failed"
    printf "    %s\n" "${MODELS_DIR}/training/${dataset}.lpqtrain"
done

# -----------------------------------------------------------------------------
say "training models (seed ${TRAINING_SEED})"
# -----------------------------------------------------------------------------
for dataset in "${MAIN_DATASETS[@]}"; do
    # Paths are passed as ARGUMENTS, not interpolated into the Python source.
    # An interpolated Windows path carries backslashes that Python would read as
    # escape sequences, and an interpolated POSIX path is one Windows Python
    # cannot open -- native_path fixes the second and argv avoids the first.
    "$PYTHON" - \
        "$(native_path "${MODELS_DIR}/training/${dataset}.lpqtrain")" \
        "$(native_path "${MODELS_DIR}/exported/${dataset}_static.lpqmodel")" \
        "$(native_path "${MODELS_DIR}/exported/${dataset}_static.report.json")" \
        "$TRAINING_SEED" \
        "$(native_path "${TRAINING_MANIFEST[$dataset]}")" <<'PYEOF' || die "training failed for ${dataset}"
import sys
from lpq_tools.training.train import train_from_file, format_report

training_set, model_out, report_out, seed, manifest = sys.argv[1:6]
# The manifest the training set was collected from is recorded in the report (name and
# SHA-256), so the model can later be tied to the exact queries it was trained on.
print(format_report(train_from_file(
    training_set, model_out, report_out, seed=int(seed), training_manifest=manifest
)))
PYEOF
done

# -----------------------------------------------------------------------------
say "running the ablation waterfall"
# -----------------------------------------------------------------------------
ablation_out="${RESULTS_DIR}/dimacs_ny_ablation_v1.json"
if [ -f "$ablation_out" ]; then
    # Raw results are immutable (ESD.md section 31); the runner refuses to
    # overwrite, so the stale file is moved aside rather than deleted.
    mv "$ablation_out" "${ablation_out}.$(date +%Y%m%d%H%M%S).bak"
    say "moved an existing results file aside; raw results are never overwritten"
fi

"$BENCHMARK" --experiment "${REPO_ROOT}/configs/experiments/dimacs_ny_ablation_v1.yaml" \
    --model "${MODELS_DIR}/exported/dimacs_ny_static.lpqmodel" \
    --output "$ablation_out" ||
    die "the ablation run failed"

# ESD.md section 36: fail on an incomplete result. A results file parses
# perfectly whether it holds four configurations or one.
require_complete_results "$ablation_out" binary_heap radix_heap lpq lpq_model

# -----------------------------------------------------------------------------
say "checking the comparison counts against the recorded ones"
# -----------------------------------------------------------------------------
#
# Timings depend on the machine and are not checked. Comparison counts do not:
# they are exact, deterministic, and reproduce to the unit on any host. If these
# differ, the algorithm or the query set changed, and every conclusion in
# docs/experiments/ built on them needs revisiting -- which is worth knowing
# immediately rather than after a figure is drawn.
"$PYTHON" - "$(native_path "$ablation_out")" <<'PYEOF' || die "comparison counts do not match the recorded findings"
import sys
from lpq_tools.evaluation.results import read_results

# Recorded in docs/experiments/ablation-waterfall.md.
EXPECTED = {
    "binary_heap": 203_801_439,
    "radix_heap": 71_839_857,
    "lpq": 177_580_603,
    # The leakage-free model, trained on the disjoint query set. The earlier
    # value here was 177_732_559, produced by a model trained on the benchmark
    # queries themselves; this check caught the change when that was corrected,
    # which is the behaviour it exists for.
    "lpq_model": 177_732_303,
}

results = read_results(sys.argv[1])
ok = True
for configuration, expected in EXPECTED.items():
    actual = results.total_comparisons(configuration)
    mark = "ok " if actual == expected else "MISMATCH"
    if actual != expected:
        ok = False
    print(f"    {mark} {configuration:<12} {actual:>14,}  expected {expected:>14,}")

if not ok:
    print(
        "\n    Comparison counts are exact and machine-independent. A mismatch means\n"
        "    the algorithm or the query set changed, so the recorded findings no\n"
        "    longer describe this code.",
        file=sys.stderr,
    )
sys.exit(0 if ok else 1)
PYEOF

say "run_main_experiments: complete"
say "results in ${RESULTS_DIR}"
