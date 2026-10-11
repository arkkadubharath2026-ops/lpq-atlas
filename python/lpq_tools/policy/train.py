"""Ridge regression per pooled-bucket width, with nested-CV hyperparameters.

Model (one per candidate width ``w``)::

    log( t_pooled_w / t_radix )  ~  intercept + coef . features(profile)

fitted on graph-level labels (see :mod:`lpq_tools.policy.dataset`) with a ridge
penalty on *standardised* features.  The standardisation is folded back into
``intercept`` and ``coef`` so the C++ reader, which has no scaler, can apply the
coefficients to the raw ``policy_features()`` values.

Hyperparameters -- the ridge strength ``lambda`` (shared by all widths) and the
decision ``margin`` -- are chosen by an inner cross-validation that leaves out one
whole TOPOLOGY at a time (a derived graph never helps validate its own base).
``lambda`` minimises out-of-fold squared error of the predicted log-ratio; the
``margin`` minimises the out-of-fold geometric-mean realised ratio of the policy's
decisions, counting only candidates that were measured on the held-out graph
(inner selection compares measured options; it never imputes a missing one).

Small N: fewer than ``min_graphs`` (default 4) graphs, or fewer than two
topologies, and training is refused -- with so few points an inner CV is not
meaningful and a fitted line would only look like a model.

CLI::

    python -m lpq_tools.policy.train --results results/raw/policy/*.json \\
        --profiles-dir datasets/processed --metadata-dir datasets/metadata \\
        --out models/policy/queue_policy_v1.lpqpolicy --seed 20260923
"""

from __future__ import annotations

import argparse
import glob
import math
import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from lpq_tools.policy.dataset import GraphInfo, PolicyTable
from lpq_tools.policy.profile import (
    FEATURE_NAMES,
    Candidate,
    QueuePolicy,
    WeightProfile,
    choose_from_log_ratios,
    features,
    policy_max_step,
)

DEFAULT_MARGINS: tuple[float, ...] = (0.0, 0.02, 0.05, 0.10)
DEFAULT_LAMBDAS: tuple[float, ...] = tuple(float(10.0**k) for k in np.arange(-3.0, 3.01, 0.5))


class TrainingRefused(ValueError):
    """Training was refused (too little data to say anything honest)."""


@dataclass(frozen=True)
class TrainConfig:
    lambdas: tuple[float, ...] = DEFAULT_LAMBDAS
    margins: tuple[float, ...] = DEFAULT_MARGINS
    feature_subset: tuple[str, ...] = FEATURE_NAMES
    min_graphs: int = 4  # refuse to train on fewer graphs (and per-candidate)
    min_inner_graphs: int = 3  # per-candidate minimum inside an inner CV fold

    def __post_init__(self) -> None:
        unknown = [f for f in self.feature_subset if f not in FEATURE_NAMES]
        if unknown or not self.feature_subset:
            raise ValueError(f"unknown/empty feature subset {self.feature_subset!r}; "
                             f"valid: {', '.join(FEATURE_NAMES)}")
        if not self.lambdas or any(l < 0 or not math.isfinite(l) for l in self.lambdas):
            raise ValueError("lambdas must be finite and >= 0")
        if not self.margins or any(not (0.0 <= m < 1.0) for m in self.margins):
            raise ValueError("margins must lie in [0, 1)")

    @property
    def columns(self) -> list[int]:
        return [FEATURE_NAMES.index(f) for f in self.feature_subset]


@dataclass(frozen=True)
class TrainingGraph:
    """One graph as the trainer sees it: features and MEASURED log-ratios."""

    dataset: str
    topology: str
    profile: WeightProfile
    x: tuple[float, ...]
    y: dict[int, float]  # width -> log geometric-mean paired ratio vs radix


def make_training_graphs(
    table: PolicyTable, infos: dict[str, GraphInfo], algorithm: str
) -> list[TrainingGraph]:
    out = []
    for ds, rows in sorted(table.by_dataset(algorithm).items()):
        gi = infos[ds]
        out.append(TrainingGraph(ds, gi.topology, gi.profile, tuple(features(gi.profile)),
                                 {w: r.y for w, r in rows.items()}))
    return out


# ----------------------------------------------------------------------------------
# Ridge with the standardisation folded into raw-feature coefficients
# ----------------------------------------------------------------------------------


def fit_ridge(x: np.ndarray, y: np.ndarray, lam: float) -> tuple[float, np.ndarray]:
    """Ridge on standardised columns; returns ``(intercept, coef)`` for RAW features.

    The intercept is not penalised.  A constant column has zero spread, is left
    unscaled, contributes nothing and gets coefficient 0.
    """
    n, p = x.shape
    mu = x.mean(axis=0)
    sd = x.std(axis=0)
    sd = np.where(sd > 0.0, sd, 1.0)
    z = (x - mu) / sd
    yc = y - y.mean()
    a = z.T @ z + lam * np.eye(p)
    beta = np.linalg.solve(a, z.T @ yc) if p else np.zeros(0)
    coef = beta / sd
    intercept = float(y.mean() - coef @ mu)
    return intercept, coef


def _expand(coef_subset: np.ndarray, columns: list[int]) -> tuple[float, ...]:
    full = np.zeros(len(FEATURE_NAMES))
    full[columns] = coef_subset
    return tuple(float(v) for v in full)


def fit_candidates(
    graphs: list[TrainingGraph], cfg: TrainConfig, lam: float, min_graphs: int
) -> tuple[dict[int, Candidate], dict[int, int]]:
    """One ridge per width over the graphs where it was measured.

    Returns the fitted candidates and ``{width: n_graphs}`` for every width that
    was NOT fitted for having fewer than ``min_graphs`` graphs.
    """
    widths = sorted({w for g in graphs for w in g.y})
    fitted: dict[int, Candidate] = {}
    skipped: dict[int, int] = {}
    cols = cfg.columns
    for w in widths:
        gs = [g for g in graphs if w in g.y]
        if len(gs) < min_graphs:
            skipped[w] = len(gs)
            continue
        x = np.array([[g.x[c] for c in cols] for g in gs], dtype=float)
        y = np.array([g.y[w] for g in gs], dtype=float)
        b, coef = fit_ridge(x, y, lam)
        fitted[w] = Candidate(width=w, intercept=b, coef=_expand(coef, cols))
    return fitted, skipped


def predict_log_ratios(cands: dict[int, Candidate], g: TrainingGraph) -> dict[int, float]:
    return {w: c.log_ratio(list(g.x)) for w, c in cands.items()}


# ----------------------------------------------------------------------------------
# Hyperparameter selection (inner leave-one-topology-out CV)
# ----------------------------------------------------------------------------------


@dataclass
class Hyperparameters:
    lam: float
    margin: float
    inner_mse_by_lambda: dict[float, float]
    inner_objective_by_margin: dict[float, float]  # mean log realised ratio (lower is better)
    inner_folds: int


def _topologies(graphs: list[TrainingGraph]) -> list[str]:
    return sorted({g.topology for g in graphs})


def select_hyperparameters(
    graphs: list[TrainingGraph], algorithm: str, cfg: TrainConfig
) -> Hyperparameters:
    topos = _topologies(graphs)
    if len(topos) < 2:
        raise TrainingRefused(
            f"only {len(topos)} distinct topology among the training graphs: an inner "
            f"leave-one-topology-out CV is impossible")
    # oof[lam][dataset] -> {width: z}
    oof: dict[float, dict[str, dict[int, float]]] = {l: {} for l in cfg.lambdas}
    sq: dict[float, list[float]] = {l: [] for l in cfg.lambdas}
    for topo in topos:
        train = [g for g in graphs if g.topology != topo]
        test = [g for g in graphs if g.topology == topo]
        for lam in cfg.lambdas:
            cands, _ = fit_candidates(train, cfg, lam, cfg.min_inner_graphs)
            for g in test:
                pred = predict_log_ratios(cands, g)
                oof[lam][g.dataset] = pred
                sq[lam].extend((z - g.y[w]) ** 2 for w, z in pred.items() if w in g.y)
    mse = {l: (float(np.mean(v)) if v else math.inf) for l, v in sq.items()}
    if all(math.isinf(v) for v in mse.values()):
        raise TrainingRefused("the inner CV produced no out-of-fold prediction "
                              "(no candidate is measured on enough graphs)")
    best_mse = min(mse.values())
    # ties -> the stronger penalty (the simpler model)
    lam = max(l for l, v in mse.items() if v <= best_mse * (1 + 1e-12))
    by_ds = {g.dataset: g for g in graphs}
    obj: dict[float, float] = {}
    for m in cfg.margins:
        vals = []
        for ds, pred in oof[lam].items():
            g = by_ds[ds]
            measured = [(w, z) for w, z in sorted(pred.items()) if w in g.y]
            ch = choose_from_log_ratios(measured, policy_max_step(g.profile, algorithm), m)
            vals.append(0.0 if ch.is_radix else g.y[ch.width])
        obj[m] = float(np.mean(vals)) if vals else math.inf
    best_obj = min(obj.values())
    margin = max(m for m, v in obj.items() if v <= best_obj + 1e-12)  # ties -> more conservative
    return Hyperparameters(lam, margin, mse, obj, len(topos))


# ----------------------------------------------------------------------------------
# Full training
# ----------------------------------------------------------------------------------


@dataclass
class FittedPolicy:
    policy: QueuePolicy
    hyper: Hyperparameters
    skipped_widths: dict[int, int]
    n_graphs: int
    n_topologies: int
    datasets: list[str]
    feature_subset: tuple[str, ...]
    algorithm: str
    per_width_graphs: dict[int, int] = field(default_factory=dict)


def train_policy(graphs: list[TrainingGraph], algorithm: str, cfg: TrainConfig) -> FittedPolicy:
    if len(graphs) < cfg.min_graphs:
        raise TrainingRefused(
            f"refusing to train on {len(graphs)} graph(s): at least {cfg.min_graphs} are "
            f"needed for the inner cross-validation to mean anything")
    hyper = select_hyperparameters(graphs, algorithm, cfg)
    cands, skipped = fit_candidates(graphs, cfg, hyper.lam, cfg.min_graphs)
    if not cands:
        raise TrainingRefused(
            f"no candidate width is measured on >= {cfg.min_graphs} graphs "
            f"(graphs per width: { {w: sum(w in g.y for g in graphs) for w in sorted({w for g in graphs for w in g.y})} })")
    pol = QueuePolicy(margin=hyper.margin, trained_on="",
                      candidates=[cands[w] for w in sorted(cands)])
    return FittedPolicy(
        policy=pol, hyper=hyper, skipped_widths=skipped, n_graphs=len(graphs),
        n_topologies=len(_topologies(graphs)), datasets=sorted(g.dataset for g in graphs),
        feature_subset=cfg.feature_subset, algorithm=algorithm,
        per_width_graphs={w: sum(w in g.y for g in graphs) for w in sorted(cands)},
    )


# ----------------------------------------------------------------------------------
# CLI
# ----------------------------------------------------------------------------------


def _expand_paths(patterns: list[str]) -> list[str]:
    out: list[str] = []
    for pat in patterns:
        if any(ch in pat for ch in "*?[") and not Path(pat).exists():
            hits = sorted(glob.glob(pat))
            if not hits:
                raise SystemExit(f"error: no files match {pat!r}")
            out.extend(hits)
        else:
            out.append(pat)
    return out


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="python -m lpq_tools.policy.train",
        description="Train, nested-evaluate and export the queue-selection policy.")
    ap.add_argument("--results", nargs="+", required=True,
                    help="lpq_benchmark result JSON files (globs allowed)")
    ap.add_argument("--profiles-dir", required=True, help="directory of <dataset>.lpqprofile files")
    ap.add_argument("--metadata-dir", required=True, help="directory of <dataset>.json metadata")
    ap.add_argument("--out", required=True, help="policy file to write (refuses to overwrite)")
    ap.add_argument("--report", default=None,
                    help="report JSON path (default: <out without suffix>.report.json)")
    ap.add_argument("--seed", type=int, required=True, help="seed for bootstrap resampling")
    ap.add_argument("--algorithm", default="dijkstra",
                    help="algorithm family to train on (default dijkstra); A* is separate")
    ap.add_argument("--features", default=",".join(FEATURE_NAMES),
                    help="comma-separated feature subset (default: all 7)")
    ap.add_argument("--margins", default=",".join(str(m) for m in DEFAULT_MARGINS))
    ap.add_argument("--bootstrap", type=int, default=1000, help="bootstrap replicates")
    ap.add_argument("--min-graphs", type=int, default=4)
    ap.add_argument("--assume-dataset", action="append", default=[], metavar="FILE=ID",
                    help="dataset id for a result file whose records have an empty 'dataset'")
    return ap


def main(argv: list[str] | None = None) -> int:
    # Imported here: evaluate/export import this module.
    from lpq_tools.policy import evaluate as ev
    from lpq_tools.policy import export as ex
    from lpq_tools.policy.dataset import ResultFileError, build_table, load_graphs
    from lpq_tools.policy.profile import PolicyFormatError

    args = build_parser().parse_args(argv)
    out = Path(args.out)
    report = Path(args.report) if args.report else out.with_suffix(".report.json")
    try:
        ex.check_writable(out, report)
        assume = {}
        for item in args.assume_dataset:
            if "=" not in item:
                raise SystemExit(f"error: --assume-dataset needs FILE=ID, got {item!r}")
            k, v = item.split("=", 1)
            assume[k] = v
        cfg = TrainConfig(
            margins=tuple(float(m) for m in args.margins.split(",")),
            feature_subset=tuple(f for f in args.features.split(",") if f),
            min_graphs=args.min_graphs)
        profiles_dir = Path(args.profiles_dir)
        known = [p.stem for p in profiles_dir.glob("*.lpqprofile")]
        table = build_table(_expand_paths(args.results), seed=args.seed, bootstrap=args.bootstrap,
                            assume_dataset=assume, known_datasets=known)
        if args.algorithm not in table.algorithms():
            raise SystemExit(f"error: algorithm {args.algorithm!r} not in the results "
                             f"(found: {', '.join(table.algorithms())})")
        infos = load_graphs(table.datasets(args.algorithm), profiles_dir, args.metadata_dir,
                            table.shapes)
        graphs = make_training_graphs(table, infos, args.algorithm)
        evaluation = ev.nested_leave_one_topology_out(graphs, args.algorithm, cfg, seed=args.seed)
        fitted = train_policy(graphs, args.algorithm, cfg)
        ex.export(out, report, fitted=fitted, evaluation=evaluation, table=table, infos=infos,
                  cfg=cfg, seed=args.seed)
    except (TrainingRefused, ResultFileError, PolicyFormatError, FileExistsError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(f"wrote {out} and {report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
