"""Cross-language verification of the training-set format.

``lpq_tools.training.dataset`` is a second implementation of the layout defined
in ``cpp/include/lpq/models/training_data.hpp``. As with the model files, the
only way to establish the two agree is to have one side produce a file and the
other consume it -- a round trip inside Python would pass just as happily if
both ends were wrong in the same way.

So the central test here runs the real ``lpq_collect`` over a real converted
graph and reads the result with the Python reader. It skips, with the command
needed to fix it, when the tool or the dataset is absent, because a developer
who has not yet built or downloaded those should get an instruction rather than
a failure.
"""

from __future__ import annotations

import io
import struct
import subprocess

import pytest

from lpq_tools.training.dataset import (
    FEATURE_LAYOUT_VERSION,
    MAGIC,
    STATIC_FEATURE_NAMES,
    TrainingSetError,
    read_training_set,
    read_training_set_file,
)
from lpq_tools.utilities.paths import RepoPaths

#: The smallest converted graph, so the test costs seconds rather than minutes.
SMALL_DATASET = "snap_facebook"


@pytest.fixture(scope="module")
def collected_set(tmp_path_factory):
    """Runs the real collector and returns the parsed training set."""
    paths = RepoPaths.discover()

    try:
        tool = paths.tool("lpq_collect")
    except FileNotFoundError as error:
        pytest.skip(str(error))

    graph = paths.processed / f"{SMALL_DATASET}.lpqgraph"
    queries = paths.manifests / f"{SMALL_DATASET}_queries.yaml"
    if not graph.exists() or not queries.exists():
        pytest.skip(
            f"{SMALL_DATASET} is not prepared; run "
            f"python scripts/data/prepare_datasets.py {SMALL_DATASET}"
        )

    out = tmp_path_factory.mktemp("training") / "set.lpqtrain"
    result = subprocess.run(
        [
            str(tool),
            "--dataset",
            str(graph),
            "--queries-file",
            str(queries),
            "--output",
            str(out),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, f"lpq_collect failed:\n{result.stderr}"
    return read_training_set_file(out)


# -----------------------------------------------------------------------------
# What the collector actually produced
# -----------------------------------------------------------------------------


def test_python_reads_what_the_collector_wrote(collected_set):
    assert collected_set.dataset_id == SMALL_DATASET
    assert len(collected_set) > 0
    assert collected_set.graph_checksum != 0
    assert collected_set.bucket_width > 0
    assert collected_set.feature_layout_version == FEATURE_LAYOUT_VERSION


def test_the_set_records_the_geometry_its_labels_depend_on(collected_set):
    # "First out of its bucket" is a different question at every bucket width,
    # so a set that did not carry the width would be uninterpretable.
    assert collected_set.bucket_width > 0
    assert collected_set.query_count > 0


def test_no_node_wins_more_often_than_it_appears(collected_set):
    # Impossible by construction. If it happened, the collector mislabelled and
    # the set would still train perfectly happily.
    for index, (wins, seen) in enumerate(
        zip(collected_set.positives, collected_set.occurrences)
    ):
        assert wins <= seen, f"node {index}: {wins} wins from {seen} appearances"


def test_both_classes_are_present(collected_set):
    # A set with only one class fits a constant dressed up as a classifier.
    assert collected_set.total_positives > 0
    assert collected_set.total_occurrences > collected_set.total_positives


def test_the_positive_rate_is_the_imbalance_a_trainer_must_report(collected_set):
    rate = collected_set.positive_rate
    assert 0.0 < rate < 1.0
    # One node per bucket per query is positive, so positives are the minority
    # by a wide margin. This is the number any accuracy has to be read against.
    assert rate < 0.5


def test_static_features_are_the_only_ones_filled(collected_set):
    # The collector fills the static group and leaves the rest zero, which is a
    # statement that this model does not see them rather than an approximation
    # of their values. A non-zero dynamic column would mean the collector had
    # started writing something it cannot know per node.
    static_count = len(STATIC_FEATURE_NAMES)
    for row in collected_set.features[:200]:
        assert set(row[static_count:]) == {0.0}


def test_static_features_are_not_all_zero(collected_set):
    # The mirror of the test above: leaving every column zero would also satisfy
    # it, and would mean the collector wrote no features at all.
    assert any(any(row[: len(STATIC_FEATURE_NAMES)]) for row in collected_set.features)


# -----------------------------------------------------------------------------
# Refusals
# -----------------------------------------------------------------------------


def test_refuses_a_file_that_is_not_a_training_set():
    with pytest.raises(TrainingSetError, match="magic"):
        read_training_set(io.BytesIO(b"this is not a training set at all, truly"))


def test_refuses_a_mismatched_feature_layout():
    # The dangerous case: every feature read from the wrong column, producing a
    # confident and plausible wrong model that nothing downstream can detect.
    header = struct.pack("<QIII", MAGIC, 1, FEATURE_LAYOUT_VERSION + 1, 28)
    with pytest.raises(TrainingSetError, match="wrong column"):
        read_training_set(io.BytesIO(header))


def test_refuses_a_truncated_file():
    with pytest.raises(TrainingSetError, match="truncated"):
        read_training_set(io.BytesIO(struct.pack("<Q", MAGIC)))


def test_refuses_a_row_claiming_more_wins_than_appearances():
    payload = struct.pack("<QIII", MAGIC, 1, FEATURE_LAYOUT_VERSION, 28)
    payload += struct.pack("<I", 0)  # empty dataset id
    payload += struct.pack("<QQQQQ", 0, 1, 1, 0, 1)  # one row declared
    payload += struct.pack("<28f", *([0.0] * 28))
    payload += struct.pack("<II", 5, 2)  # 5 wins from 2 appearances

    with pytest.raises(TrainingSetError, match="positives"):
        read_training_set(io.BytesIO(payload))
