"""Policy tooling, part 3: export, the CLI, and the round trip through the real C++ loader."""

from __future__ import annotations

import json
import math
import os
import shutil
import subprocess
from pathlib import Path

import pytest
from lpq_tools.policy import export as ex
from lpq_tools.policy import profile as P
from lpq_tools.policy.dataset import sha256_file
from lpq_tools.policy.train import main as train_main
from test_policy_support import build_world

REPO = Path(__file__).resolve().parents[2]
LPQ_RUN = REPO / "build" / "release" / "bin" / "lpq_run"
NY_GRAPH = REPO / "datasets" / "processed" / "dimacs_ny.lpqgraph"
NY_PROFILE = REPO / "datasets" / "processed" / "dimacs_ny.lpqprofile"


def _policy(margin: float, ws_z: list[tuple[int, float]], coef: tuple[float, ...] = (0.0,) * 7) -> P.QueuePolicy:
    return P.QueuePolicy(margin=margin, candidates=[P.Candidate(w, z, coef) for w, z in ws_z])


# --- writer ---------------------------------------------------------------------------


def test_policy_text_round_trips_doubles_exactly() -> None:
    pol = _policy(0.05, [(1, -0.123456789012345678), (8, 1e-5)],
                  (0.1 / 3, -2.5e-7, 1.0, 0.0, 12345.678901234567, -0.0, 3.3))
    text = ex.format_policy(pol, "datasets=a,b seed=1")
    back = P.parse_policy_text(text)
    assert back.margin == 0.05 and back.trained_on.strip() == "datasets=a,b seed=1"
    assert [(c.width, c.intercept, c.coef) for c in back.candidates] == \
        [(c.width, c.intercept, c.coef) for c in pol.candidates]
    assert text.splitlines()[0] == "lpqpolicy 1"
    assert any(l.startswith("features log2_max_w log2_mean_w p50_over_max p99_over_max cv_w "
                            "log2_nodes mean_degree") for l in text.splitlines())


def test_writer_rejects_non_finite_and_multiline() -> None:
    with pytest.raises(P.PolicyFormatError, match="non-finite"):
        ex.format_policy(_policy(0.0, [(1, float("nan"))]), "x")
    with pytest.raises(P.PolicyFormatError, match="single line"):
        ex.format_policy(_policy(0.0, [(1, 0.0)]), "a\nb")


def test_export_refuses_to_overwrite(tmp_path: Path) -> None:
    out, rep = tmp_path / "p.lpqpolicy", tmp_path / "p.report.json"
    ex.check_writable(out, rep)
    rep.write_text("{}")
    with pytest.raises(FileExistsError):
        ex.check_writable(out, rep)


# --- CLI end to end on a synthetic world ---------------------------------------------------


def _run_cli(root: Path, paths: list[Path], out: Path, extra: list[str] | None = None) -> int:
    return train_main(["--results", *map(str, paths), "--profiles-dir", str(root / "profiles"),
                       "--metadata-dir", str(root / "meta"), "--out", str(out), "--seed", "20260923",
                       "--bootstrap", "100", *(extra or [])])


def test_cli_writes_policy_and_report_with_provenance(tmp_path: Path) -> None:
    paths, _ = build_world(tmp_path / "w", n_topologies=6, derived_per_topology=1)
    out = tmp_path / "models" / "policy" / "queue_policy_v1.lpqpolicy"
    assert _run_cli(tmp_path / "w", paths, out) == 0
    rep_path = out.with_suffix(".report.json")
    assert out.is_file() and rep_path.is_file()

    pol = P.load_policy(out)
    assert {c.width for c in pol.candidates} == {1, 8}
    for token in ("graphs=12", "topologies=6", "seed=20260923", "algorithm=dijkstra", "base_0",
                  "derived_0_1", "git="):
        assert token in pol.trained_on

    rep = json.loads(rep_path.read_text())
    assert rep["format"] == ex.REPORT_FORMAT
    assert rep["provenance"]["seed"] == 20260923
    assert {r["path"]: r["sha256"] for r in rep["provenance"]["result_files"]} == \
        {str(p): sha256_file(p) for p in paths}
    assert rep["policy_file"]["sha256"] == sha256_file(out)
    assert {p_["dataset"] for p_ in rep["provenance"]["profiles"]} == {
        p.stem for p in (tmp_path / "w" / "profiles").glob("*.lpqprofile")}
    assert all(len(p_["sha256"]) == 64 for p_ in rep["provenance"]["profiles"])
    ev = rep["evaluation"]
    assert ev["protocol"]["scheme"] == "nested leave-one-topology-out"
    assert len(ev["folds"]) == 12
    # accounting is part of the report: every candidate record is accounted for
    for row in rep["data_accounting"]["per_dataset_algorithm_width"]:
        assert row["candidate_records"] == (row["paired"] + row["unsupported_candidate"]
                                            + row["unsupported_radix"] + row["no_radix"])
    json.dumps(rep, allow_nan=False)  # strict JSON: no NaN/Infinity tokens

    # a second run refuses to overwrite, and leaves the first untouched
    before = out.read_bytes()
    assert _run_cli(tmp_path / "w", paths, out) == 2
    assert out.read_bytes() == before


def test_cli_refuses_to_train_on_too_few_graphs(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    paths, _ = build_world(tmp_path / "w", n_topologies=3, derived_per_topology=0)
    out = tmp_path / "p.lpqpolicy"
    assert _run_cli(tmp_path / "w", paths, out) == 2
    assert "refusing to train" in capsys.readouterr().err
    assert not out.exists() and not out.with_suffix(".report.json").exists()


def test_cli_accepts_a_feature_subset_and_a_missing_profile_is_a_clear_error(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    paths, _ = build_world(tmp_path / "w", n_topologies=6, derived_per_topology=0)
    out = tmp_path / "a.lpqpolicy"
    assert _run_cli(tmp_path / "w", paths, out, ["--features", "p50_over_max,log2_mean_w"]) == 0
    for c in P.load_policy(out).candidates:
        nz = {n for n, v in zip(P.FEATURE_NAMES, c.coef) if v != 0.0}
        assert nz <= {"p50_over_max", "log2_mean_w"}
    (tmp_path / "w" / "profiles" / "base_2.lpqprofile").unlink()
    assert _run_cli(tmp_path / "w", paths, tmp_path / "b.lpqpolicy") == 2
    assert "base_2.lpqprofile" in capsys.readouterr().err


# --- the real C++ loader ------------------------------------------------------------------


def _engine_ready() -> bool:
    return LPQ_RUN.is_file() and NY_GRAPH.is_file() and NY_PROFILE.is_file()


def _run_engine(tmp_path: Path, policy: Path) -> tuple[int, dict]:
    """Runs lpq_run on a symlinked copy of dimacs_ny so the profile cache lives in tmp_path."""
    graph = tmp_path / "dimacs_ny.lpqgraph"
    if not graph.exists():
        graph.symlink_to(NY_GRAPH)
        shutil.copy(NY_PROFILE, tmp_path / "dimacs_ny.lpqprofile")
    result = tmp_path / f"{policy.stem}.result.json"
    proc = subprocess.run(
        [str(LPQ_RUN), "--dataset", str(graph), "--source", "87719", "--target", "87423",
         "--pq", "lpq_optimized", "--policy", str(policy), "--repetitions", "1", "--warmup", "0",
         "--result-out", str(result), "--run-id", "t"],
        capture_output=True, text=True, timeout=120, env={**os.environ})
    return proc.returncode, json.loads(result.read_text())


@pytest.mark.skipif(not _engine_ready(), reason="lpq_run, dimacs_ny.lpqgraph or its profile is missing")
def test_cpp_loader_resolves_what_python_predicts(tmp_path: Path) -> None:
    x = P.features(P.load_profile(NY_PROFILE))
    # coefficients that make the prediction depend on the real feature vector, so a wrong
    # feature order, formula or exp() in the Python mirror would show up as a different choice
    coef_a = (0.05, -0.02, 0.4, -0.3, 0.1, 0.01, 0.02)
    dot_a = sum(c * v for c, v in zip(coef_a, x))
    cases = {
        "two_pooled": _policy(0.05, [(1, math.log(0.5)), (8, math.log(0.4))]),
        "radix_by_margin": _policy(0.7, [(1, math.log(0.5)), (8, math.log(0.4))]),
        "radix_by_ratio": _policy(0.0, [(1, math.log(1.5))]),
        "feature_dependent": _policy(0.02, [(1, math.log(0.55) - dot_a),
                                            (4, math.log(0.60) - dot_a),
                                            (16, math.log(0.45) - dot_a + 1.0)], coef_a),
        "tie_first_wins": _policy(0.0, [(2, math.log(0.5)), (4, math.log(0.5))]),
        "empty": _policy(0.03, []),
    }
    seen = set()
    for name, pol in cases.items():
        path = tmp_path / f"{name}.lpqpolicy"
        path.write_text(ex.format_policy(pol, f"datasets=roundtrip case={name}"))
        rc, res = _run_engine(tmp_path, path)
        assert rc == 0, res
        assert res["optimizer"]["profile_source"] == "cached"
        py = P.load_policy(path).choose(P.load_profile(tmp_path / "dimacs_ny.lpqprofile"), "dijkstra")
        assert res["optimizer"]["resolved_queue"] == py.describe(), name
        assert res["optimizer"]["predicted_ratio_vs_radix"] == pytest.approx(py.predicted_ratio, rel=1e-12)
        seen.add(py.describe())
    # the cases genuinely exercise different outcomes
    assert "radix_heap" in seen and len(seen) >= 4


@pytest.mark.skipif(not _engine_ready(), reason="lpq_run, dimacs_ny.lpqgraph or its profile is missing")
def test_cpp_loader_accepts_an_exported_policy_and_rejects_a_foreign_feature_list(tmp_path: Path) -> None:
    paths, _ = build_world(tmp_path / "w", n_topologies=6, derived_per_topology=1)
    out = tmp_path / "trained.lpqpolicy"
    assert _run_cli(tmp_path / "w", paths, out) == 0
    rc, res = _run_engine(tmp_path, out)
    assert rc == 0
    py = P.load_policy(out).choose(P.load_profile(tmp_path / "dimacs_ny.lpqprofile"), "dijkstra")
    assert res["optimizer"]["resolved_queue"] == py.describe()

    bad = tmp_path / "bad.lpqpolicy"
    bad.write_text(out.read_text().replace("cv_w", "cvw"))
    rc, res = _run_engine(tmp_path, bad)
    assert rc != 0 and res["status"] == "failed" and "feature list" in res["error"]["message"]
