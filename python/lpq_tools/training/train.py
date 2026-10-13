"""Fitting the static node scorer (ESD.md section 39 milestone 15).

Trains a logistic regression that answers one question: given a node's static
properties, how often is it the first entry extracted from the bucket it lands
in? That is precisely what ``StaticNodeScorer`` evaluates once per node before
any timed region, so the model fitted here is the model that ships.

-----------------------------------------------------------------------------
Why the aggregate rows are expanded into weights, not into rows
-----------------------------------------------------------------------------

``lpq_collect`` writes one row per node with ``positives`` and ``occurrences``.
The obvious move is to expand that into one row per observation, which on
``dimacs_ny`` means 26 million rows to express 264,346 distinct inputs. Instead
each node contributes two weighted rows -- one positive with weight
``positives`` and one negative with weight ``occurrences - positives``. The
likelihood is identical, to the last bit, because the expanded rows are equal in
x and differ only in y.

-----------------------------------------------------------------------------
Why the split is by node and not by observation
-----------------------------------------------------------------------------

The model sees only static per-node features, so every observation of a given
node has identical inputs. Splitting observations at random would put some of a
node's rows in train and the rest in test, and since the inputs are the same on
both sides the test set would be measuring memorisation of nodes the model had
already seen. The split is therefore over NODES: a node is wholly in train or
wholly in test, and the reported score is what the model achieves on nodes it
has never been fitted to.

-----------------------------------------------------------------------------
Why accuracy is not reported as the headline
-----------------------------------------------------------------------------

The labels are severely unbalanced: roughly one node per bucket per query is
positive. A model that always answers "no" therefore scores very high accuracy
while being useless, so accuracy alone would make a worthless model look
excellent. The report below leads with the majority-class baseline and states
every metric against it, and it reports ROC AUC and average precision, which the
imbalance does not flatter.
"""

from __future__ import annotations

import dataclasses
import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

import numpy as np
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import average_precision_score, roc_auc_score

from lpq_tools.data.queries import read_manifest
from lpq_tools.training.dataset import (
    STATIC_FEATURE_NAMES,
    TrainingSet,
    read_training_set_file,
)
from lpq_tools.training.model_io import (
    ModelKind,
    expand_static_weights,
    write_linear_model,
)
from lpq_tools.utilities.hashing import sha256_file


@dataclass(frozen=True)
class TrainingReport:
    """Everything needed to judge whether the fitted model is worth shipping."""

    dataset_id: str
    graph_checksum: int
    bucket_width: int
    query_count: int
    query_seed: int

    nodes: int
    observations: int
    positives: int
    positive_rate: float

    #: Accuracy of always answering the majority class. Every other number here
    #: has to be read against this one.
    majority_baseline_accuracy: float

    #: Fraction of nodes held out. Zero only when explicitly disabled.
    test_fraction: float

    #: In-sample scores, kept because the gap between them and the held-out
    #: scores is the overfitting this split exists to expose. Reporting only the
    #: held-out numbers would hide whether the model generalises or merely
    #: happens to score well twice.
    train_accuracy: float
    train_roc_auc: float
    train_average_precision: float

    accuracy: float
    roc_auc: float
    average_precision: float

    #: Average precision of a random ranker, which equals the positive rate.
    #: Reported so the AP above is interpretable without arithmetic.
    average_precision_baseline: float

    l2_inverse_strength: float
    seed: int

    weights: dict[str, float]
    bias: float

    #: Which query manifest produced the training set: name, SHA-256, selection rule, seed and query
    #: count (see :func:`describe_training_manifest`). None when the caller did not say; reports
    #: written before this field existed lack the key altogether.
    training_manifest: dict[str, Any] | None = None

    @property
    def precision_lift(self) -> float:
        """How many times better than a random ranker the model is.

        The honest headline for an unbalanced ranking task. Average precision
        alone is not comparable between datasets, because chance itself differs
        with the positive rate -- 0.118 is close to worthless where chance is
        0.105 and would be excellent where chance is 0.016.
        """
        base = self.average_precision_baseline
        return (self.average_precision / base) if base > 0 else 0.0

    def beats_chance(self) -> bool:
        """Whether the model ranks better than chance at all.

        A ranking criterion rather than an accuracy one, deliberately: the
        scorer ORDERS a bucket and never makes a yes/no decision, so a model
        that cannot rank is useless here however many labels it gets right.
        """
        return self.roc_auc > 0.5 and self.average_precision > self.average_precision_baseline

    def is_useful(self) -> bool:
        """Whether the margin over chance is large enough to be worth shipping.

        Passing `beats_chance` is a very low bar -- an AUC of 0.5001 clears it --
        and reporting that as a verdict would let a model that carries almost no
        information read as a success. The thresholds here are a judgement, not
        a measurement, and are stated so they can be argued with: an AUC at
        least 0.05 above chance AND at least 1.5x the precision of a random
        ranker.

        Measured, this separates the two datasets the project has trained on.
        On `snap_facebook` the static features give AUC 0.79 and 22x lift; on
        `dimacs_ny` they give AUC 0.53 and 1.12x, which is a road network
        telling us its near-uniform degree carries almost no signal.
        """
        return (self.roc_auc - 0.5) >= 0.05 and self.precision_lift >= 1.5


def _node_rows(data: TrainingSet, indices: np.ndarray):
    """Turns aggregate rows for the given nodes into weighted positive/negative pairs.

    Nodes that were never settled contribute nothing: they carry no observation,
    and a zero-weight row would only add noise while inflating the row count a
    report shows.
    """
    features = np.asarray(data.static_features(), dtype=np.float64)[indices]
    positives = np.asarray(data.positives, dtype=np.float64)[indices]
    occurrences = np.asarray(data.occurrences, dtype=np.float64)[indices]
    negatives = occurrences - positives

    observed = occurrences > 0
    features = features[observed]
    positives = positives[observed]
    negatives = negatives[observed]

    x = np.vstack([features, features])
    y = np.concatenate([np.ones(len(features)), np.zeros(len(features))])
    weights = np.concatenate([positives, negatives])

    keep = weights > 0
    return x[keep], y[keep], weights[keep]


def _score(model, x, y, weights):
    """Accuracy, ROC AUC and average precision under observation weights."""
    scores = model.decision_function(x)
    predicted = (scores > 0).astype(float)
    return (
        float(np.average(predicted == y, weights=weights)),
        float(roc_auc_score(y, scores, sample_weight=weights)),
        float(average_precision_score(y, scores, sample_weight=weights)),
    )


def train_static_scorer(
    data: TrainingSet,
    *,
    seed: int,
    l2_inverse_strength: float = 1.0,
    max_iterations: int = 1000,
    test_fraction: float = 0.25,
) -> tuple[list[float], float, TrainingReport]:
    """Fits the model on a node split and reports held-out performance.

    Args:
        data: a training set from ``lpq_collect``.
        seed: required, not defaulted. Every RNG seed in this project is
            explicit and preserved so a fit can be replayed exactly -- including
            the split, which is drawn from this seed and is therefore part of
            what a rerun reproduces.
        l2_inverse_strength: scikit-learn's ``C``. Smaller means more
            regularisation.
        test_fraction: share of NODES held out. See the module docstring for why
            the split is over nodes rather than observations.

    Returns:
        (static weights, bias, report). The returned weights are fitted on the
        training split only, so the reported held-out scores describe the model
        that is actually returned.

    Raises:
        ValueError: if either split ends up with a single class, where a fitted
            model is a constant dressed up as a classifier, or if the fraction
            is not a proper fraction.
    """
    if not 0.0 <= test_fraction < 1.0:
        raise ValueError(f"test_fraction must be in [0, 1), got {test_fraction}")

    node_count = len(data)
    generator = np.random.default_rng(seed)
    order = generator.permutation(node_count)
    held_out = int(round(node_count * test_fraction))
    test_nodes = order[:held_out]
    train_nodes = order[held_out:]

    x_train, y_train, w_train = _node_rows(data, train_nodes)

    positive_mass = float(w_train[y_train == 1].sum())
    negative_mass = float(w_train[y_train == 0].sum())
    if positive_mass == 0 or negative_mass == 0:
        raise ValueError(
            "the training split has only one class "
            f"({positive_mass} positive, {negative_mass} negative observations); "
            "any model fitted to it is a constant, not a classifier"
        )

    model = LogisticRegression(
        C=l2_inverse_strength,
        max_iter=max_iterations,
        random_state=seed,
        solver="lbfgs",
    )
    model.fit(x_train, y_train, sample_weight=w_train)

    train_accuracy, train_auc, train_ap = _score(model, x_train, y_train, w_train)

    if held_out > 0:
        x_test, y_test, w_test = _node_rows(data, test_nodes)
        if float(w_test[y_test == 1].sum()) == 0:
            raise ValueError(
                "the held-out split contains no positive observations, so no "
                "ranking metric can be computed on it. Use a larger "
                "test_fraction or a training set covering more queries"
            )
        accuracy, auc, ap = _score(model, x_test, y_test, w_test)
    else:
        # Explicitly disabled. The held-out fields then repeat the in-sample
        # ones rather than being left at zero, which would read as a model that
        # scored nothing.
        accuracy, auc, ap = train_accuracy, train_auc, train_ap

    rate = data.positive_rate
    report = TrainingReport(
        dataset_id=data.dataset_id,
        graph_checksum=data.graph_checksum,
        bucket_width=data.bucket_width,
        query_count=data.query_count,
        query_seed=data.query_seed,
        nodes=node_count,
        observations=data.total_occurrences,
        positives=data.total_positives,
        positive_rate=rate,
        majority_baseline_accuracy=max(rate, 1.0 - rate),
        test_fraction=test_fraction,
        train_accuracy=train_accuracy,
        train_roc_auc=train_auc,
        train_average_precision=train_ap,
        accuracy=accuracy,
        roc_auc=auc,
        average_precision=ap,
        average_precision_baseline=rate,
        l2_inverse_strength=l2_inverse_strength,
        seed=seed,
        weights=dict(zip(STATIC_FEATURE_NAMES, map(float, model.coef_[0]))),
        bias=float(model.intercept_[0]),
    )

    return [float(w) for w in model.coef_[0]], float(model.intercept_[0]), report


def describe_training_manifest(path: str | Path, data: TrainingSet) -> dict[str, Any]:
    """The provenance record for the manifest a training set was collected from.

    The record is checked against the training set before it is trusted: a manifest for another
    dataset or graph, or with another seed or query count, cannot be what produced this set, and
    recording it would be worse than recording nothing. (Seed and count do not tell protocol B from
    protocol C apart, which is why the SHA-256 is recorded.)
    """
    path = Path(path)
    manifest = read_manifest(path)
    if manifest.dataset_id != data.dataset_id:
        raise ValueError(
            f"{path.name} is for dataset {manifest.dataset_id!r}, the training set for {data.dataset_id!r}"
        )
    if manifest.graph_checksum is not None and manifest.graph_checksum != data.graph_checksum:
        raise ValueError(f"{path.name} was generated for a different graph than the training set")
    if manifest.seed != data.query_seed:
        raise ValueError(
            f"{path.name} has seed {manifest.seed}, the training set was collected at seed {data.query_seed}"
        )
    if len(manifest.pairs) != data.query_count:
        raise ValueError(
            f"{path.name} has {len(manifest.pairs)} queries, the training set was collected from {data.query_count}"
        )
    return {
        "name": path.name,
        "sha256": sha256_file(path),
        "selection": manifest.selection,
        "seed": manifest.seed,
        "query_count": len(manifest.pairs),
    }


def train_from_file(
    training_set: str | Path,
    model_out: str | Path,
    report_out: str | Path | None = None,
    *,
    seed: int,
    l2_inverse_strength: float = 1.0,
    test_fraction: float = 0.25,
    training_manifest: str | Path | None = None,
) -> TrainingReport:
    """Reads a training set, fits, writes a loadable model and its report.

    *training_manifest* is the query manifest the training set was collected from. When given,
    its name and SHA-256 are recorded in the report so that the model can later be tied to the
    exact queries it was trained on.
    """
    data = read_training_set_file(training_set)
    provenance = (
        describe_training_manifest(training_manifest, data) if training_manifest is not None else None
    )
    static_weights, bias, report = train_static_scorer(
        data,
        seed=seed,
        l2_inverse_strength=l2_inverse_strength,
        test_fraction=test_fraction,
    )
    report = dataclasses.replace(report, training_manifest=provenance)

    model_path = Path(model_out)
    model_path.parent.mkdir(parents=True, exist_ok=True)
    write_linear_model(
        model_path,
        expand_static_weights(static_weights),
        bias,
        kind=ModelKind.LOGISTIC,
    )

    if report_out is not None:
        report_path = Path(report_out)
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(asdict(report), indent=2), encoding="utf-8")

    return report


def format_report(report: TrainingReport) -> str:
    """A console summary that cannot be read as better than it is."""
    if report.is_useful():
        verdict = "carries usable signal"
    elif report.beats_chance():
        verdict = (
            "better than chance, but only barely -- NOT worth shipping. "
            "The features carry almost no information about this graph"
        )
    else:
        verdict = "no better than chance -- this model is worthless here"
    return "\n".join(
        [
            f"dataset          : {report.dataset_id}",
            f"bucket width     : {report.bucket_width}",
            f"queries          : {report.query_count}  seed {report.query_seed}",
            f"nodes            : {report.nodes}",
            f"observations     : {report.observations}",
            f"positives        : {report.positives}  (rate {report.positive_rate:.6f})",
            "",
            f"held out         : {report.test_fraction:.0%} of nodes "
            "(a node is wholly in train or wholly in test)",
            "",
            "                     held-out      in-sample",
            f"accuracy         : {report.accuracy:11.6f}  {report.train_accuracy:12.6f}",
            f"  always-no gives: {report.majority_baseline_accuracy:11.6f}"
            "   <-- accuracy alone says almost nothing here",
            f"ROC AUC          : {report.roc_auc:11.6f}  {report.train_roc_auc:12.6f}"
            "   (0.5 is chance)",
            f"average precision: {report.average_precision:11.6f}"
            f"  {report.train_average_precision:12.6f}"
            f"   (chance is {report.average_precision_baseline:.6f})",
            f"precision lift   : {report.precision_lift:11.2f}x over a random ranker",
            "",
            f"verdict          : {verdict}",
        ]
    )
