#!/usr/bin/env bash
# =============================================================================
# Regenerate the figures (ESD.md section 36).
#
#   scripts/reproduce/generate_figures.sh
#
# Reads results/raw/ and writes results/figures/. It never runs an experiment:
# if a results file is missing or incomplete the script stops and names it,
# because a figure drawn from a partial matrix looks exactly like one drawn from
# a complete matrix.
#
# WHY THE PAIRED SCATTER IS RENDERED TWICE
#
# The model-versus-control comparison was measured in two matrix orders, and the
# effect size differed by 45% between them (docs/experiments/ablation-waterfall.md).
# Rendering only one would present a position-inflated number as the result.
# Both are written, named by the order that produced them, so neither can be
# quoted as "the" figure without the other existing beside it.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

require_python

FIGURES_DIR="${REPO_ROOT}/results/figures"
mkdir -p "$FIGURES_DIR"

"$PYTHON" -c "import matplotlib" 2> /dev/null ||
    die "matplotlib is not installed. Run:
       pip install -r python/requirements.txt"

ablation="${RESULTS_DIR}/dimacs_ny_ablation_v1.json"
order_check="${RESULTS_DIR}/dimacs_ny_order_check_v1.json"

# Both results files are required before anything is drawn. The paired scatter
# exists in two matrix orders precisely so that neither can be quoted without the
# other; a script that drew one and exited 0 would hand out the position-inflated
# figure on its own.
require_file "$ablation" "the ablation results" \
    "scripts/reproduce/run_main_experiments.sh"
require_file "$order_check" "the order-check results (the same ladder with lpq_model first)" \
    "scripts/reproduce/run_main_experiments.sh   (configs/experiments/dimacs_ny_order_check_v1.yaml)"
require_complete_results "$ablation" binary_heap radix_heap lpq lpq_model
require_complete_results "$order_check" binary_heap radix_heap lpq lpq_model

say "ablation waterfall"
"$PYTHON" - "$(native_path "$ablation")" "$(native_path "${FIGURES_DIR}/ablation_waterfall.png")" <<'PYEOF' ||
import sys
from lpq_tools.evaluation.ablation import build_waterfall
from lpq_tools.evaluation.results import read_results
from lpq_tools.plotting.figures import plot_ablation_waterfall

results = read_results(sys.argv[1])
dataset = results.records[0].dataset if results.records else "unknown"
print("    " + str(plot_ablation_waterfall(
    build_waterfall(results), sys.argv[2], dataset=dataset
)))
PYEOF
    die "rendering the ablation figure failed"

say "paired scatter, both matrix orders"
for pair in "dimacs_ny_ablation_v1.json:order_lpq_model_last" \
            "dimacs_ny_order_check_v1.json:order_lpq_model_first"; do
    source_file="${RESULTS_DIR}/${pair%%:*}"
    tag="${pair##*:}"

    "$PYTHON" - "$(native_path "$source_file")" "$(native_path "${FIGURES_DIR}/lpq_vs_lpq_model__${tag}.png")" "$tag" <<'PYEOF' ||
import sys
from lpq_tools.evaluation.results import read_results
from lpq_tools.plotting.figures import plot_paired_scatter

results = read_results(sys.argv[1])
baseline, treatment = results.paired_medians("lpq", "lpq_model")
dataset = results.records[0].dataset if results.records else "unknown"
label = sys.argv[3].replace("_", " ")
print("    " + str(plot_paired_scatter(
    baseline, treatment, sys.argv[2],
    baseline_name="lpq", treatment_name="lpq_model",
    dataset=f"{dataset} ({label})",
)))
PYEOF
        die "rendering the paired scatter for ${tag} failed"
done

# ESD.md section 36 asks the scripts to fail when an expected artifact is
# absent. A figure directory that silently ends up short is the figure-shaped
# version of an incomplete result.
say "checking every expected figure exists"
missing=()
for expected in ablation_waterfall.png \
                lpq_vs_lpq_model__order_lpq_model_last.png \
                lpq_vs_lpq_model__order_lpq_model_first.png; do
    [ -f "${FIGURES_DIR}/${expected}" ] || missing+=("$expected")
done
if [ ${#missing[@]} -gt 0 ]; then
    die "figures were not written: ${missing[*]}"
fi

say "generate_figures: complete"
say "figures in ${FIGURES_DIR}"
