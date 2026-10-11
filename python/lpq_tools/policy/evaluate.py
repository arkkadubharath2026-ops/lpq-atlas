"""Nested leave-one-TOPOLOGY-out evaluation of the queue-selection policy.

For every graph ``G`` in turn:

1. every dataset sharing ``G``'s base topology (``G`` itself, its base, and every
   sibling derived from the same base) is removed from the training set --
   :func:`assert_no_leakage` checks it and raises, so a ``derived_ny_*`` variant can
   never train a model evaluated on ``dimacs_ny``;
2. a policy is trained on the remaining graphs, *including* its own inner
   cross-validation for ``lambda`` and the margin (see :mod:`.train`);
3. the policy is applied to ``G``'s profile exactly as the engine would
   (:meth:`QueuePolicy.choose`, ring-feasibility rule included);
4. the REALISED paired ratio of the chosen queue versus radix on ``G`` is read from
   the measured rows.  If the chosen candidate was not measured on ``G`` the fold
   is marked ``unmeasured`` and counted separately -- nothing is imputed.

Baselines, evaluated on the same graphs: ``always radix`` (ratio 1) and ``always
the single best fixed candidate`` chosen on the training graphs only (the pooled
width with the lowest mean log-ratio among those measured on EVERY training graph).

Statistics are descriptive: with a handful of graphs a bootstrap interval over
graphs is wide, and the sign test is exact but low-powered.  Both are reported with
``n`` so the reader can see that.
"""

from __future__ import annotations

import math
import zlib
from typing import Any

import numpy as np

from lpq_tools.policy.train import (
    TrainConfig,
    TrainingGraph,
    TrainingRefused,
    train_policy,
)


class LeakageError(AssertionError):
    """A training set contained the held-out graph or one of its topology siblings."""


def assert_no_leakage(heldout: TrainingGraph, train: list[TrainingGraph]) -> None:
    bad = sorted(g.dataset for g in train
                 if g.dataset == heldout.dataset or g.topology == heldout.topology)
    if bad:
        raise LeakageError(
            f"leakage: held-out {heldout.dataset!r} (topology {heldout.topology!r}) shares "
            f"topology with training dataset(s) {bad}")


def _finite(x: float | None) -> float | None:
    return None if x is None or not math.isfinite(x) else float(x)


def _sign_test_p(wins: int, losses: int) -> float | None:
    n = wins + losses
    if n == 0:
        return None
    k = min(wins, losses)
    tail = sum(math.comb(n, i) for i in range(k + 1)) / 2.0**n
    return min(1.0, 2.0 * tail)


def paired_comparison(
    name: str, log_diff: list[float], seed: int, tie_tol: float, resamples: int
) -> dict[str, Any]:
    """Policy minus baseline in log-ratio (negative: the policy is faster)."""
    d = np.array(log_diff, dtype=float)
    n = int(d.size)
    out: dict[str, Any] = {"baseline": name, "n_graphs": n}
    if n == 0:
        out.update(geo_mean_ratio_policy_over_baseline=None, ci95=None, wins=0, losses=0,
                   ties=0, sign_test_p=None)
        return out
    tol = math.log1p(tie_tol)
    wins = int((d < -tol).sum())
    losses = int((d > tol).sum())
    out["geo_mean_ratio_policy_over_baseline"] = float(math.exp(d.mean()))
    if n >= 2:
        rng = np.random.default_rng(np.random.SeedSequence([seed & 0xFFFFFFFF, seed >> 32,
                                                            zlib.crc32(name.encode())]))
        means = d[rng.integers(0, n, size=(resamples, n))].mean(axis=1)
        lo, hi = np.percentile(means, [2.5, 97.5])
        out["ci95"] = [float(math.exp(lo)), float(math.exp(hi))]
    else:
        out["ci95"] = None
    out.update(wins=wins, losses=losses, ties=n - wins - losses,
               sign_test_p=_sign_test_p(wins, losses), tie_tolerance=tie_tol,
               bootstrap_resamples=resamples if n >= 2 else 0)
    return out


def _aggregate(folds: list[dict[str, Any]], seed: int, tie_tol: float, resamples: int) -> dict[str, Any]:
    ok = [f for f in folds if f["status"] == "evaluated"]
    measured = [f for f in ok if f["realised_ratio"] is not None]
    agg: dict[str, Any] = {
        "n_graphs": len(folds),
        "n_skipped_untrainable": sum(f["status"] != "evaluated" for f in folds),
        "n_policy_chose_unmeasured": sum(f["status"] == "evaluated" and f["unmeasured"] for f in folds),
        "n_with_realised_ratio": len(measured),
        "n_chose_radix": sum(f["chosen"] == "radix_heap" for f in measured),
    }
    if not measured:
        agg.update(geo_mean_realised_ratio=None, geo_mean_oracle_ratio=None,
                   geo_mean_regret=None, fraction_beat_radix=None, fraction_lost_to_radix=None,
                   fraction_tied=None, comparisons=[])
        return agg
    lr = np.array([math.log(f["realised_ratio"]) for f in measured])
    lo = np.array([math.log(f["oracle_best_ratio"]) for f in measured])
    n = len(measured)
    beat = int((lr < -math.log1p(tie_tol)).sum())
    lost = int((lr > math.log1p(tie_tol)).sum())
    agg.update(
        geo_mean_realised_ratio=float(math.exp(lr.mean())),
        geo_mean_oracle_ratio=float(math.exp(lo.mean())),
        geo_mean_regret=float(math.exp((lr - lo).mean())),
        fraction_beat_radix=beat / n, n_beat_radix=beat,
        fraction_lost_to_radix=lost / n, n_lost_to_radix=lost,
        fraction_tied=(n - beat - lost) / n, tie_tolerance=tie_tol,
        n_strictly_below_1=int((lr < 0).sum()),
    )
    comps = [paired_comparison("always_radix", [float(v) for v in lr], seed, tie_tol, resamples)]
    fx = [f for f in measured if f["best_fixed"] and f["best_fixed"]["realised_ratio"] is not None]
    comps.append(paired_comparison(
        "best_fixed_candidate_from_training_graphs",
        [math.log(f["realised_ratio"]) - math.log(f["best_fixed"]["realised_ratio"]) for f in fx],
        seed, tie_tol, resamples))
    agg["comparisons"] = comps
    agg["n_best_fixed_defined"] = len(fx)
    return agg


def nested_leave_one_topology_out(
    graphs: list[TrainingGraph],
    algorithm: str,
    cfg: TrainConfig,
    seed: int,
    tie_tol: float = 0.02,
    resamples: int = 2000,
) -> dict[str, Any]:
    folds: list[dict[str, Any]] = []
    for held in graphs:
        train = [g for g in graphs if g.topology != held.topology]
        assert_no_leakage(held, train)
        fold: dict[str, Any] = {
            "heldout": held.dataset, "topology": held.topology, "derived": held.derived,
            "train_datasets": sorted(g.dataset for g in train),
            "measured_widths": sorted(held.y),
        }
        measured_ratios = {w: math.exp(y) for w, y in held.y.items()}
        best_w = min(measured_ratios, key=measured_ratios.get) if measured_ratios else None
        oracle = min(1.0, measured_ratios[best_w]) if best_w is not None else 1.0
        fold["oracle_best_ratio"] = oracle
        fold["oracle_choice"] = ("radix_heap" if best_w is None or measured_ratios[best_w] >= 1.0
                                 else f"pooled_w{best_w}")
        try:
            fitted = train_policy(train, algorithm, cfg)
        except TrainingRefused as exc:
            fold.update(status="skipped_untrainable", reason=str(exc), chosen=None,
                        realised_ratio=None, unmeasured=False, regret=None, best_fixed=None)
            folds.append(fold)
            continue
        choice = fitted.policy.choose(held.profile, algorithm)
        fold.update(status="evaluated", hyper={"lambda": fitted.hyper.lam,
                                               "margin": fitted.hyper.margin},
                    trained_widths=sorted(c.width for c in fitted.policy.candidates),
                    chosen="radix_heap" if choice.is_radix else f"pooled_w{choice.width}",
                    chosen_ring=choice.ring, predicted_ratio=_finite(choice.predicted_ratio))
        if choice.is_radix:
            realised: float | None = 1.0
        elif choice.width in measured_ratios:
            realised = measured_ratios[choice.width]
        else:
            realised = None
        fold["unmeasured"] = realised is None
        fold["realised_ratio"] = realised
        fold["regret"] = None if realised is None else realised / oracle

        # best single fixed candidate, from the training graphs only
        common = set.intersection(*(set(g.y) for g in train)) if train else set()
        if common:
            w = min(sorted(common), key=lambda c: float(np.mean([g.y[c] for g in train])))
            fold["best_fixed"] = {"width": w,
                                  "train_mean_log_ratio": float(np.mean([g.y[w] for g in train])),
                                  "realised_ratio": measured_ratios.get(w)}
        else:
            fold["best_fixed"] = None
        folds.append(fold)

    sel_real = [f for f, g in zip(folds, graphs) if not g.derived]
    return {
        "protocol": {
            "scheme": "nested leave-one-topology-out",
            "inner": "leave-one-topology-out CV for ridge lambda and margin on the training graphs",
            "algorithm": algorithm,
            "feature_subset": list(cfg.feature_subset),
            "lambda_grid": list(cfg.lambdas), "margin_grid": list(cfg.margins),
            "min_graphs": cfg.min_graphs, "seed": seed,
            "unmeasured_policy": "a chosen candidate not measured on the held-out graph is counted "
                                 "separately and excluded from realised-ratio aggregates; never imputed",
            "tie_tolerance": tie_tol,
        },
        "folds": folds,
        "aggregate_all_graphs": _aggregate(folds, seed, tie_tol, resamples),
        "aggregate_real_graphs_only": _aggregate(sel_real, seed, tie_tol, resamples),
    }
