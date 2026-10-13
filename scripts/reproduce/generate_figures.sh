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
require_file "$ablation" "the ablation results" \
    "scripts/reproduce/run_main_experiments.sh"
require_complete_results "$ablation" binary_heap radix_heap lpq lpq_model

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
rendered_any=0
for pair in "dimacs_ny_ablation_v1.json:order_lpq_model_last" \
            "dimacs_ny_order_check_v1.json:order_lpq_model_first"; do
    source_file="${RESULTS_DIR}/${pair%%:*}"
    tag="${pair##*:}"

    if [ ! -f "$source_file" ]; then
        # Not fatal on its own, but the pair exists for a reason, so the absence
        # is stated rather than passed over.
        printf "    %s is absent; the order control for this figure is missing\n" \
            "$source_file"
        continue
    fi

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
    rendered_any=1
done

[ "$rendered_any" -eq 1 ] ||
    die "no paired scatter could be rendered: neither results file was present"

# ESD.md section 36 asks the scripts to fail when an expected artifact is
# absent. A figure directory that silently ends up short is the figure-shaped
# version of an incomplete result.
say "checking every expected figure exists"
missing=()
for expected in ablation_waterfall.png lpq_vs_lpq_model__order_lpq_model_last.png; do
    [ -f "${FIGURES_DIR}/${expected}" ] || missing+=("$expected")
done
if [ ${#missing[@]} -gt 0 ]; then
    die "figures were not written: ${missing[*]}"
fi

if [ ! -f "${FIGURES_DIR}/lpq_vs_lpq_model__order_lpq_model_first.png" ]; then
    printf "\n\033[1;33mWARNING:\033[0m the order-reversed scatter is missing.\n"
    printf "  The remaining figure shows the matrix order that was measured to\n"
    printf "  inflate this effect by 45%%. Do not quote it alone.\n"
    printf "  Produce it with the order-check experiment before publishing.\n"
fi

say "generate_figures: complete"
say "figures in ${FIGURES_DIR}"
