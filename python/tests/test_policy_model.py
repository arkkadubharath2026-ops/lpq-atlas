"""Policy tooling, part 2: training and nested leave-one-topology-out evaluation."""

from __future__ import annotations

import math
from dataclasses import replace
from pathlib import Path

import numpy as np
import pytest
from lpq_tools.policy.dataset import build_table, load_graphs
from lpq_tools.policy.evaluate import (
    LeakageError,
    assert_no_leakage,
    nested_leave_one_topology_out,
    paired_comparison,
)
from lpq_tools.policy.profile import FEATURE_NAMES
from lpq_tools.policy.train import (
    TrainConfig,
    TrainingGraph,
    TrainingRefused,
    fit_ridge,
    make_training_graphs,
    select_hyperparameters,
    train_policy,
)
from test_policy_support import build_world, make_profile, true_log_ratio

P50 = FEATURE_NAMES.index("p50_over_max")


def _graphs(tmp_path: Path, **kw) -> tuple[list[TrainingGraph], dict[str, float]]:
    paths, ratios = build_world(tmp_path, **kw)
    t = build_table(paths, seed=3, bootstrap=100)
    infos = load_graphs(t.datasets("dijkstra"), tmp_path / "profiles", tmp_path / "meta", t.shapes)
    return make_training_graphs(t, infos, "dijkstra"), ratios


# --- ridge -------------------------------------------------------------------------


def test_ridge_with_no_penalty_recovers_a_linear_truth_on_raw_features() -> None:
    rng = np.random.default_rng(0)
    x = rng.normal([10.0, 5000.0, 0.3], [2.0, 900.0, 0.1], size=(40, 3))
    true_b, true_c = 0.7, np.array([0.3, -0.0004, 2.5])
    y = true_b + x @ true_c
    b, c = fit_ridge(x, y, 0.0)
    assert b == pytest.approx(true_b, abs=1e-8)
    np.testing.assert_allclose(c, true_c, atol=1e-10)  # standardisation is folded in


def test_ridge_shrinks_and_ignores_constant_columns() -> None:
    rng = np.random.default_rng(1)
    x = np.column_stack([rng.normal(size=30), np.full(30, 4.0)])
    y = 1.5 * x[:, 0] + 2.0
    _, c0 = fit_ridge(x, y, 0.0)
    _, c1 = fit_ridge(x, y, 50.0)
    assert c0[1] == 0.0 and c1[1] == 0.0
    assert abs(c1[0]) < abs(c0[0])
    b, c = fit_ridge(x, y, 0.0)
    assert float(np.mean(b + x @ c - y)) == pytest.approx(0.0, abs=1e-9)


# --- training on a world with known truth -------------------------------------------


def test_training_recovers_the_known_truth(tmp_path: Path) -> None:
    graphs, ratios = _graphs(tmp_path, n_topologies=6, derived_per_topology=1)
    fitted = train_policy(graphs, "dijkstra", TrainConfig(lambdas=(1e-6, 1e-3)))
    assert fitted.n_graphs == 12 and fitted.n_topologies == 6
    by = {g.dataset: g for g in graphs}
    # Raw coefficients of the nuisance features are not identifiable here (mean_degree is
    # nearly constant across topologies), so check what matters: the predictions.
    for c in fitted.policy.candidates:
        for ds, r in ratios.items():
            assert c.log_ratio(list(by[ds].x)) == pytest.approx(true_log_ratio(c.width, r), abs=0.1)
    # with the one informative feature the coefficients themselves are recovered
    only = train_policy(graphs, "dijkstra", TrainConfig(feature_subset=("p50_over_max",),
                                                       lambdas=(1e-6,)))
    w1 = next(c for c in only.policy.candidates if c.width == 1)
    assert w1.coef[P50] == pytest.approx(2.0, abs=0.1)
    assert w1.intercept == pytest.approx(-0.5, abs=0.05)
    # the decisions: pooled where the truth says it wins, radix where radix wins
    chosen_pooled = {ds: not fitted.policy.choose(by[ds].profile).is_radix for ds in by}
    for ds, r in ratios.items():
        if r < 0.15:
            assert chosen_pooled[ds], (ds, r)
        if r > 0.35:
            assert not chosen_pooled[ds], (ds, r)


def test_feature_subset_zeroes_the_rest(tmp_path: Path) -> None:
    graphs, _ = _graphs(tmp_path)
    cfg = TrainConfig(feature_subset=("p50_over_max",), lambdas=(1e-6,))
    fitted = train_policy(graphs, "dijkstra", cfg)
    for c in fitted.policy.candidates:
        assert all(v == 0.0 for i, v in enumerate(c.coef) if i != P50)
        assert c.coef[P50] != 0.0
    with pytest.raises(ValueError, match="unknown"):
        TrainConfig(feature_subset=("nope",))


def test_when_pooled_never_wins_the_policy_always_picks_radix(tmp_path: Path) -> None:
    graphs, _ = _graphs(tmp_path)
    shifted = [replace(g, y={w: abs(y) + 0.3 for w, y in g.y.items()}) for g in graphs]
    fitted = train_policy(shifted, "dijkstra", TrainConfig())
    assert all(fitted.policy.choose(g.profile).is_radix for g in shifted)


def test_training_refuses_too_few_graphs_and_too_few_topologies(tmp_path: Path) -> None:
    graphs, _ = _graphs(tmp_path, n_topologies=6, derived_per_topology=0)
    with pytest.raises(TrainingRefused, match="at least 4"):
        train_policy(graphs[:3], "dijkstra", TrainConfig())
    same_topology = [replace(g, topology="one") for g in graphs]
    with pytest.raises(TrainingRefused, match="topology"):
        train_policy(same_topology, "dijkstra", TrainConfig())
    # a width measured on fewer than min_graphs graphs is not fitted -- and is reported
    sparse = [replace(g, y={**g.y, 99: 0.1}) if i < 2 else g for i, g in enumerate(graphs)]
    fitted = train_policy(sparse, "dijkstra", TrainConfig())
    assert fitted.skipped_widths == {99: 2}
    assert 99 not in {c.width for c in fitted.policy.candidates}


def test_hyperparameter_selection_is_deterministic_and_inner_folds_are_topologies(tmp_path: Path) -> None:
    graphs, _ = _graphs(tmp_path, n_topologies=5, derived_per_topology=2)
    a = select_hyperparameters(graphs, "dijkstra", TrainConfig())
    b = select_hyperparameters(graphs, "dijkstra", TrainConfig())
    assert (a.lam, a.margin) == (b.lam, b.margin)
    assert a.inner_folds == 5  # topologies, not the 15 graphs


# --- leakage guard and nested evaluation ---------------------------------------------


def _g(ds: str, topo: str) -> TrainingGraph:
    return TrainingGraph(ds, topo, make_profile(10, 30, 10.0, 2.0, 0.2), (0.0,) * 7, {1: 0.0})


def test_leakage_guard_flags_self_and_topology_siblings() -> None:
    held = _g("dimacs_ny", "dimacs_ny")
    assert_no_leakage(held, [_g("snap_facebook", "snap_facebook")])
    with pytest.raises(LeakageError, match="derived_ny_u16"):
        assert_no_leakage(held, [_g("snap_facebook", "snap_facebook"), _g("derived_ny_u16", "dimacs_ny")])
    with pytest.raises(LeakageError):
        assert_no_leakage(held, [_g("dimacs_ny", "elsewhere")])  # the same dataset id, whatever the group


def test_no_fold_trains_on_a_sibling_of_its_held_out_graph(tmp_path: Path) -> None:
    graphs, _ = _graphs(tmp_path, n_topologies=6, derived_per_topology=2)
    topo = {g.dataset: g.topology for g in graphs}
    ev = nested_leave_one_topology_out(graphs, "dijkstra", TrainConfig(), seed=1, resamples=200)
    assert len(ev["folds"]) == 18
    for f in ev["folds"]:
        assert f["heldout"] not in f["train_datasets"]
        assert all(topo[d] != topo[f["heldout"]] for d in f["train_datasets"])
        # exactly the other topologies' graphs: 5 topologies x 3 graphs
        assert len(f["train_datasets"]) == 15
        assert f["status"] == "evaluated"


def test_a_held_out_fold_is_blind_to_its_siblings_labels(tmp_path: Path) -> None:
    graphs, _ = _graphs(tmp_path, n_topologies=6, derived_per_topology=1)
    target = next(g for g in graphs if g.dataset == "base_0")
    sibling = next(g for g in graphs if g.dataset == "derived_0_1")
    poisoned = [replace(g, y={w: 5.0 for w in g.y}) if g is sibling else g for g in graphs]
    cfg = TrainConfig()
    a = nested_leave_one_topology_out(graphs, "dijkstra", cfg, seed=1, resamples=50)
    b = nested_leave_one_topology_out(poisoned, "dijkstra", cfg, seed=1, resamples=50)
    fa = next(f for f in a["folds"] if f["heldout"] == target.dataset)
    fb = next(f for f in b["folds"] if f["heldout"] == target.dataset)
    assert fa["chosen"] == fb["chosen"] and fa["predicted_ratio"] == fb["predicted_ratio"]
    assert fa["hyper"] == fb["hyper"]


def test_nested_evaluation_beats_radix_where_truth_says_it_should(tmp_path: Path) -> None:
    graphs, ratios = _graphs(tmp_path, n_topologies=8, derived_per_topology=1, n_queries=25)
    ev = nested_leave_one_topology_out(graphs, "dijkstra", TrainConfig(), seed=2, resamples=500)
    agg = ev["aggregate_all_graphs"]
    assert agg["n_graphs"] == 16 and agg["n_with_realised_ratio"] == 16
    assert agg["n_policy_chose_unmeasured"] == 0
    # truth: radix is the best choice for r>0.25; pooled for r<0.25. A recovered policy must
    # gain over always-radix on the held-out graphs and cannot be better than the oracle.
    assert agg["geo_mean_realised_ratio"] < 1.0
    assert agg["geo_mean_realised_ratio"] >= agg["geo_mean_oracle_ratio"] - 1e-12
    assert agg["geo_mean_regret"] >= 1.0 - 1e-12
    assert agg["fraction_lost_to_radix"] <= 0.15
    vs_radix = next(c for c in agg["comparisons"] if c["baseline"] == "always_radix")
    assert vs_radix["geo_mean_ratio_policy_over_baseline"] < 1.0 and vs_radix["wins"] > vs_radix["losses"]
    ci = vs_radix["ci95"]
    assert ci is not None and ci[0] <= vs_radix["geo_mean_ratio_policy_over_baseline"] <= ci[1]
    fixed = next(c for c in agg["comparisons"] if c["baseline"].startswith("best_fixed"))
    assert fixed["n_graphs"] == 16
    # per-fold bookkeeping
    for f in ev["folds"]:
        assert f["oracle_best_ratio"] <= 1.0
        assert f["regret"] >= 1.0 - 1e-12
        if f["chosen"] == "radix_heap":
            assert f["realised_ratio"] == 1.0
    real = ev["aggregate_real_graphs_only"]
    assert real["n_graphs"] == 8


def test_unmeasured_choice_is_counted_not_imputed(tmp_path: Path) -> None:
    graphs, ratios = _graphs(tmp_path, n_topologies=6, derived_per_topology=0, n_queries=20)
    # Hide the pooled widths on the graph where the policy will clearly choose pooled.
    low = min(graphs, key=lambda g: ratios[g.dataset])
    blind = [replace(g, y={}) if g is low else g for g in graphs]
    ev = nested_leave_one_topology_out(blind, "dijkstra", TrainConfig(), seed=1, resamples=50)
    f = next(f for f in ev["folds"] if f["heldout"] == low.dataset)
    assert f["status"] == "evaluated" and f["chosen"] != "radix_heap"
    assert f["unmeasured"] is True and f["realised_ratio"] is None and f["regret"] is None
    agg = ev["aggregate_all_graphs"]
    assert agg["n_policy_chose_unmeasured"] == 1
    assert agg["n_with_realised_ratio"] == 5  # excluded from the aggregates, not given a value


def test_too_few_graphs_leaves_every_fold_untrainable(tmp_path: Path) -> None:
    graphs, _ = _graphs(tmp_path, n_topologies=4, derived_per_topology=0)  # 3 left per fold
    ev = nested_leave_one_topology_out(graphs, "dijkstra", TrainConfig(), seed=1, resamples=50)
    assert all(f["status"] == "skipped_untrainable" for f in ev["folds"])
    agg = ev["aggregate_all_graphs"]
    assert agg["n_skipped_untrainable"] == 4 and agg["geo_mean_realised_ratio"] is None


def test_paired_comparison_counts_and_sign_test() -> None:
    d = [math.log(0.8)] * 5 + [math.log(1.01)] + [math.log(1.5)]
    c = paired_comparison("x", d, seed=1, tie_tol=0.02, resamples=300)
    assert (c["wins"], c["ties"], c["losses"]) == (5, 1, 1)
    assert c["sign_test_p"] == pytest.approx(2 * (1 + 6) / 64)
    assert paired_comparison("x", [], 1, 0.02, 10)["n_graphs"] == 0
