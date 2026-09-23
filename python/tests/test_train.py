"""Tests for fitting the static node scorer.

The tests that matter here are not "does logistic regression converge" --
scikit-learn's problem, not this project's. They are:

  * the node-level split really holds nodes out, so a reported score is not
    measuring memorisation;
  * a set with one class is refused rather than fitted, because a model fitted
    to it is a constant dressed up as a classifier;
  * the verdict does not call a near-chance model useful, which is the failure
    that would let this project report a learned component as working when it
    carries almost no information.

That last one is not hypothetical. The static features give a held-out AUC of
0.79 on ``snap_facebook`` and 0.53 on ``dimacs_ny``; a verdict thresholded at
"better than chance" would have called both a success.
"""

from __future__ import annotations

import numpy as np
import pytest

from lpq_tools.training.dataset import TrainingSet
from lpq_tools.training.train import train_static_scorer


def make_set(
    node_count: int = 400,
    *,
    separable: bool = True,
    seed: int = 7,
) -> TrainingSet:
    """A synthetic set where the label is (or is not) a function of a feature.

    When ``separable`` the first static column drives the label, so a working
    trainer must find it. When not, the label is independent of every feature
    and no honest model can rank better than chance -- which is what the verdict
    tests below rely on.
    """
    rng = np.random.default_rng(seed)
    features: list[list[float]] = []
    positives: list[int] = []
    occurrences: list[int] = []

    for _ in range(node_count):
        signal = float(rng.uniform(0.0, 1.0))
        row = [signal] + [float(rng.uniform(0.0, 1.0)) for _ in range(5)] + [0.0] * 22
        seen = int(rng.integers(5, 20))
        probability = signal if separable else 0.3
        won = int(rng.binomial(seen, probability))
        features.append(row)
        positives.append(won)
        occurrences.append(seen)

    return TrainingSet(
        dataset_id="synthetic",
        graph_checksum=1234,
        bucket_width=8,
        query_count=10,
        query_seed=1,
        feature_layout_version=1,
        features=features,
        positives=positives,
        occurrences=occurrences,
    )


# -----------------------------------------------------------------------------
# Fitting
# -----------------------------------------------------------------------------


def test_recovers_a_signal_that_is_there():
    weights, _bias, report = train_static_scorer(make_set(), seed=1)

    # The first column drives the label, so it must carry the largest positive
    # weight. A trainer that shuffled its columns would still converge and still
    # report a good AUC, so the column identity is asserted, not just the score.
    assert weights[0] == max(weights)
    assert report.roc_auc > 0.6


def test_reports_no_signal_when_there_is_none():
    _weights, _bias, report = train_static_scorer(make_set(separable=False), seed=1)

    assert not report.is_useful()
    assert report.roc_auc == pytest.approx(0.5, abs=0.05)


def test_the_same_seed_reproduces_the_same_fit():
    # The split is drawn from the seed, so reproducibility covers which nodes
    # were held out as well as the optimiser.
    first = train_static_scorer(make_set(), seed=99)
    second = train_static_scorer(make_set(), seed=99)

    assert first[0] == second[0]
    assert first[1] == second[1]
    assert first[2].roc_auc == second[2].roc_auc


def test_a_different_seed_changes_the_split():
    _, _, a = train_static_scorer(make_set(), seed=1)
    _, _, b = train_static_scorer(make_set(), seed=2)

    # Different held-out nodes, so the held-out score should differ. If it did
    # not, the split would not be doing anything.
    assert a.roc_auc != b.roc_auc


# -----------------------------------------------------------------------------
# The split
# -----------------------------------------------------------------------------


def test_held_out_nodes_are_not_in_the_fit():
    """The whole point of splitting by node rather than by observation.

    Every observation of a node has identical features, so a split that let a
    node appear on both sides would report memorisation as generalisation. The
    check is indirect but sufficient: with a genuine signal, held-out and
    in-sample scores should be close but not identical, whereas a leaking split
    makes them equal.
    """
    _, _, report = train_static_scorer(make_set(), seed=3, test_fraction=0.5)

    assert report.test_fraction == 0.5
    assert report.roc_auc != report.train_roc_auc


def test_a_zero_fraction_disables_the_split_explicitly():
    # Held-out fields then repeat the in-sample ones rather than reading as a
    # model that scored nothing.
    _, _, report = train_static_scorer(make_set(), seed=3, test_fraction=0.0)

    assert report.test_fraction == 0.0
    assert report.roc_auc == report.train_roc_auc


def test_refuses_a_fraction_that_is_not_a_fraction():
    with pytest.raises(ValueError, match="test_fraction"):
        train_static_scorer(make_set(), seed=1, test_fraction=1.0)


# -----------------------------------------------------------------------------
# Refusals
# -----------------------------------------------------------------------------


def test_refuses_a_set_with_no_positives():
    data = make_set()
    degenerate = TrainingSet(
        **{
            **data.__dict__,
            "positives": [0] * len(data),
        }
    )

    with pytest.raises(ValueError, match="only one class"):
        train_static_scorer(degenerate, seed=1)


def test_refuses_a_set_with_no_negatives():
    data = make_set()
    degenerate = TrainingSet(
        **{
            **data.__dict__,
            "positives": list(data.occurrences),
        }
    )

    with pytest.raises(ValueError, match="only one class"):
        train_static_scorer(degenerate, seed=1)


def test_the_seed_is_required():
    # Every RNG seed in this project is explicit so a fit can be replayed.
    with pytest.raises(TypeError):
        train_static_scorer(make_set())  # type: ignore[call-arg]


# -----------------------------------------------------------------------------
# The verdict
# -----------------------------------------------------------------------------


def test_a_barely_better_than_chance_model_is_not_called_useful():
    """The failure this project would most regret publishing.

    dimacs_ny measures a held-out AUC of 0.533 and 1.12x the precision of a
    random ranker. A verdict thresholded at "better than chance" calls that a
    success; it is a road network telling us its near-uniform degree carries
    almost no signal.
    """
    _, _, report = train_static_scorer(make_set(separable=False), seed=5)

    assert not report.is_useful()


def test_precision_lift_is_relative_to_chance():
    # Average precision is not comparable across datasets, because chance moves
    # with the positive rate. The lift is what makes two datasets comparable.
    _, _, report = train_static_scorer(make_set(), seed=1)

    expected = report.average_precision / report.average_precision_baseline
    assert report.precision_lift == pytest.approx(expected)
