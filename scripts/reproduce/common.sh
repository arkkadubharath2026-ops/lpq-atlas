#!/usr/bin/env bash
# =============================================================================
# Shared helpers for the reproduction scripts (ESD.md section 36).
#
# Sourced, never executed. Everything here exists to make the four scripts fail
# LOUDLY rather than quietly, which ESD.md section 36 requires by name: a
# reproduction script that carries on past a missing dataset or an incomplete
# result produces output that looks like a reproduction and is not one.
#
# The failure modes this guards against are all silent by nature:
#
#   * a missing dataset, where an experiment simply measures fewer graphs;
#   * a wrong checksum, where it measures a DIFFERENT graph under the right
#     name, and every number is plausible;
#   * an incomplete result, where a table is built from half a matrix;
#   * an absent metric, where a figure loses a series without saying so.
#
# None of those crash. All of them are caught here or not at all.
# =============================================================================

set -euo pipefail

# Resolved from this file rather than from the caller's working directory, so
# the scripts behave the same run from anywhere.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly REPO_ROOT

readonly BUILD_DIR="${LPQ_BUILD_DIR:-${REPO_ROOT}/build/release}"
readonly BIN_DIR="${BUILD_DIR}/bin"
readonly RESULTS_DIR="${REPO_ROOT}/results/raw"
readonly MODELS_DIR="${REPO_ROOT}/models"

# The datasets the main experiments need. Adding one here is the only change
# required to include it, which is why the list is data rather than repeated
# inline in each script.
readonly MAIN_DATASETS=(dimacs_ny snap_facebook)

# Seeds. Every one is explicit and preserved so a rerun reproduces exactly; a
# default buried in a library would make a rerun depend on that default not
# changing.
readonly TRAINING_SEED=20260923
readonly STATS_SEED=20260923

say() { printf "\033[1m==>\033[0m %s\n" "$1"; }

# Converts a path to the form the PLATFORM's Python understands.
#
# Under Git Bash on Windows the shell hands out POSIX paths like
# /d/project/models, which native Windows Python cannot open at all -- it is not
# a Cygwin build and has never heard of /d. The C++ tools survive it because the
# MSYS layer rewrites their arguments at the exec boundary; Python invoked
# through a heredoc receives the raw string and fails with a FileNotFoundError
# naming a path the reader can see plainly exists.
#
# cygpath -m gives forward slashes, which are safe to embed in a Python string
# where a backslash form would be read as an escape. On Linux cygpath is absent
# and the path passes through unchanged.
native_path() {
    if command -v cygpath > /dev/null 2>&1; then
        cygpath -m "$1"
    else
        printf '%s' "$1"
    fi
}

die() {
    printf "\033[1;31mFAILED:\033[0m %s\n" "$1" >&2
    exit 1
}

# A tool the scripts need, with the command that produces it.
require_tool() {
    local name="$1"
    local path="${BIN_DIR}/${name}"
    if [ ! -x "$path" ] && [ ! -x "${path}.exe" ]; then
        die "$name is not built. Run scripts/reproduce/build_all.sh first, or:
       cmake --build ${BUILD_DIR} --target ${name}"
    fi
}

# Returns the executable path, handling the .exe suffix on Windows.
tool_path() {
    local name="$1"
    if [ -x "${BIN_DIR}/${name}.exe" ]; then
        printf '%s' "${BIN_DIR}/${name}.exe"
    else
        printf '%s' "${BIN_DIR}/${name}"
    fi
}

require_file() {
    local path="$1" what="$2" fix="$3"
    [ -f "$path" ] || die "$what is missing: $path
       $fix"
}

require_dataset() {
    local id="$1"
    require_file "${REPO_ROOT}/datasets/processed/${id}.lpqgraph" \
        "dataset '${id}'" \
        "python scripts/data/prepare_datasets.py ${id}"
    require_file "${REPO_ROOT}/datasets/metadata/${id}.json" \
        "metadata for '${id}'" \
        "python scripts/data/prepare_datasets.py ${id}"
}

# Verifies a results file holds every configuration it was supposed to measure.
#
# ESD.md section 36 asks the scripts to fail on an INCOMPLETE result, and this
# is the check that does it. A results file is valid JSON and parses perfectly
# whether it holds four configurations or one, so nothing downstream notices a
# matrix that ran halfway.
require_complete_results() {
    local path="$1"; shift
    require_file "$path" "results file" "re-run the experiment that produces it"

    local missing=()
    local configuration
    for configuration in "$@"; do
        if ! "${PYTHON:-python}" - "$(native_path "$path")" "$configuration" <<'PYEOF'
import json, sys
payload = json.loads(open(sys.argv[1], encoding="utf-8").read())
wanted = sys.argv[2]
present = any(
    r.get("pq") == wanted and r.get("supported", True)
    for r in payload.get("records", [])
)
sys.exit(0 if present else 1)
PYEOF
        then
            missing+=("$configuration")
        fi
    done

    if [ ${#missing[@]} -gt 0 ]; then
        die "$path is incomplete: no supported records for ${missing[*]}
       The experiment did not measure every configuration it declares, so any
       table or figure built from it would be a partial result presented as a
       whole."
    fi
}

# Python, preferring an explicit interpreter so a virtualenv is honoured.
PYTHON="${LPQ_PYTHON:-python}"
export PYTHONPATH="${REPO_ROOT}/python${PYTHONPATH:+:${PYTHONPATH}}"

require_python() {
    command -v "$PYTHON" > /dev/null 2>&1 ||
        die "python not found on PATH. Set LPQ_PYTHON to an interpreter."
    "$PYTHON" -c "import numpy, sklearn" 2> /dev/null ||
        die "the analysis stack is not installed. Run:
       pip install -r python/requirements.txt"
}

# -----------------------------------------------------------------------------
# Training query sets (protocol C, source-disjoint)
# -----------------------------------------------------------------------------
#
# lpq_collect labels a training set from a full single-source search per query, so a training query
# that shares a SOURCE with a benchmark query reuses that search even when the pair differs. A
# training set therefore has to share no source with any benchmark manifest the pipeline measures or
# reports against. These are those manifests; a dataset without an entry here is refused rather than
# trained on an unchecked set.
benchmark_manifests_for() {
    case "$1" in
        dimacs_ny)     printf '%s\n' dimacs_ny_queries_100 dimacs_ny_queries_first30 ;;
        snap_facebook) printf '%s\n' snap_facebook_queries snap_facebook_queries_first100 \
                                     snap_facebook_queries_first30 ;;
        *) die "no benchmark manifests are declared for dataset '$1' in common.sh, so its training
       set cannot be checked for source overlap" ;;
    esac
}

# Prints the path of the training manifest to use for a dataset, after verifying it.
#
#   select_training_manifest <dataset> [manifests_dir]
#
# A manifest named <dataset>_training_queries_source_disjoint.yaml takes precedence; otherwise
# <dataset>_training_queries.yaml is used, but only if the check below passes. It must be for the same
# graph as the benchmark manifests and share no source with any of them. The protocol note goes to
# stderr so stdout is only the path.
select_training_manifest() {
    local dataset="$1"
    local dir="${2:-${REPO_ROOT}/experiments/manifests}"
    local path="${dir}/${dataset}_training_queries_source_disjoint.yaml"
    [ -f "$path" ] || path="${dir}/${dataset}_training_queries.yaml"
    [ -f "$path" ] || die "no disjoint training query set for ${dataset} in ${dir}
       Training on the benchmark queries would violate ESD.md section 30, so the
       pipeline stops rather than falling back to them. Generate one with:
       python scripts/data/generate_training_queries.py --benchmark <benchmark manifest> ..."

    local benchmarks=() name
    while IFS= read -r name; do
        require_file "${dir}/${name}.yaml" "benchmark manifest" "git restore experiments/manifests/${name}.yaml"
        benchmarks+=("$(native_path "${dir}/${name}.yaml")")
    done < <(benchmark_manifests_for "$dataset")

    "$PYTHON" - "$(native_path "$path")" "${benchmarks[@]}" >&2 <<'PYEOF' ||
import sys
from pathlib import Path
from lpq_tools.data.queries import read_manifest, shared_sources

training = read_manifest(Path(sys.argv[1]))
bad = []
for name in sys.argv[2:]:
    benchmark = read_manifest(Path(name))
    if (benchmark.dataset_id, benchmark.node_count, benchmark.graph_checksum) != (
        training.dataset_id, training.node_count, training.graph_checksum
    ):
        bad.append(f"{Path(name).name}: a different graph (dataset id, node count or checksum)")
        continue
    shared = shared_sources(training, benchmark)
    if shared:
        bad.append(f"{Path(name).name}: {len(shared)} shared source node(s)")
if bad:
    print(f"{Path(sys.argv[1]).name} is not source-disjoint from the benchmark manifests:")
    for line in bad:
        print("  " + line)
    sys.exit(1)
print(
    f"    {Path(sys.argv[1]).name}: source-disjoint from {len(sys.argv) - 2} benchmark "
    f"manifest(s), {len(training.pairs)} queries, seed {training.seed}"
)
PYEOF
        die "the training query set for ${dataset} shares source nodes with a benchmark manifest.
       lpq_collect labels from a full single-source search per source, so that is leakage
       under the corrected protocol. Generate a source-disjoint set with:
       python scripts/data/generate_training_queries.py --benchmark <benchmark manifest> ..."

    printf '%s' "$path"
}
