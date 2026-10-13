"""D-20: the order-check experiment has a versioned producer, and figure generation cannot succeed without it.

The reversed-order run behind the historical 45% position-effect figure was never committed, so its
output file cannot be recovered. This experiment is a NEW measurement of the same question: the same
ladder as ``dimacs_ny_ablation_v1`` in the opposite matrix order.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest
import yaml

_ROOT = Path(__file__).resolve().parents[2]
_EXPERIMENTS = _ROOT / "configs" / "experiments"
_REPRODUCE = _ROOT / "scripts" / "reproduce"


def _load(name: str) -> dict:
    return yaml.safe_load((_EXPERIMENTS / name).read_text(encoding="utf-8"))


def test_the_order_check_config_is_the_ablation_ladder_in_reverse_and_nothing_else():
    ablation = _load("dimacs_ny_ablation_v1.yaml")
    reversed_run = _load("dimacs_ny_order_check_v1.yaml")

    assert reversed_run["experiment"] == "dimacs_ny_order_check_v1"
    assert reversed_run["pq"] == list(reversed(ablation["pq"]))
    assert reversed_run["pq"][0] == "lpq_model", "the model runs first, which is the point"

    # Everything else must match, or the two runs would differ in more than matrix order.
    ablation.pop("experiment"), reversed_run.pop("experiment")
    ablation.pop("pq"), reversed_run.pop("pq")
    assert reversed_run == ablation


def test_the_order_check_experiment_writes_the_file_the_figure_script_reads():
    run = (_REPRODUCE / "run_main_experiments.sh").read_text(encoding="utf-8")
    figures = (_REPRODUCE / "generate_figures.sh").read_text(encoding="utf-8")
    assert "dimacs_ny_order_check_v1.yaml" in run
    assert "dimacs_ny_order_check_v1.json" in run
    assert "dimacs_ny_order_check_v1.json" in figures


# -----------------------------------------------------------------------------
# generate_figures.sh must not report success without both results files
# -----------------------------------------------------------------------------


@pytest.fixture
def tree(tmp_path: Path) -> Path:
    """A minimal copy of the repository layout the script derives its paths from."""
    if shutil.which("bash") is None:
        pytest.skip("needs bash")
    (tmp_path / "scripts" / "reproduce").mkdir(parents=True)
    for name in ("common.sh", "generate_figures.sh"):
        shutil.copy(_REPRODUCE / name, tmp_path / "scripts" / "reproduce" / name)
    (tmp_path / "python").symlink_to(_ROOT / "python", target_is_directory=True)
    (tmp_path / "results" / "raw").mkdir(parents=True)
    return tmp_path


def _run_figures(tree: Path) -> subprocess.CompletedProcess[str]:
    env = {**os.environ, "LPQ_PYTHON": shutil.which("python3") or "python"}
    return subprocess.run(
        ["bash", str(tree / "scripts" / "reproduce" / "generate_figures.sh")],
        capture_output=True, text=True, env=env, check=False,
    )


def _no_figures(tree: Path) -> bool:
    figures = tree / "results" / "figures"
    return not figures.exists() or not any(figures.iterdir())


def test_missing_order_check_results_fail_the_script_and_name_the_file(tree):
    (tree / "results" / "raw" / "dimacs_ny_ablation_v1.json").write_text("{}", encoding="utf-8")
    result = _run_figures(tree)
    assert result.returncode != 0, result.stdout
    assert "dimacs_ny_order_check_v1.json" in result.stderr
    assert "run_main_experiments.sh" in result.stderr
    assert _no_figures(tree), "nothing is drawn from a partial set of results"


def test_missing_ablation_results_fail_the_script_and_name_the_file(tree):
    (tree / "results" / "raw" / "dimacs_ny_order_check_v1.json").write_text("{}", encoding="utf-8")
    result = _run_figures(tree)
    assert result.returncode != 0
    assert "dimacs_ny_ablation_v1.json" in result.stderr
    assert _no_figures(tree)


def test_no_results_at_all_fail_the_script(tree):
    assert _run_figures(tree).returncode != 0


def test_an_order_check_file_missing_a_configuration_is_refused(tree):
    # Present and parseable, but only one of the four queues was measured.
    partial = '{"records": [{"pq": "lpq", "supported": true}]}'
    for name in ("dimacs_ny_ablation_v1.json", "dimacs_ny_order_check_v1.json"):
        (tree / "results" / "raw" / name).write_text(partial, encoding="utf-8")
    result = _run_figures(tree)
    assert result.returncode != 0
    assert "incomplete" in result.stderr
    assert _no_figures(tree)
