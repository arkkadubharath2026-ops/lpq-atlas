"""Reading the training sets that ``lpq_collect`` writes.

The binary layout is defined by ``cpp/include/lpq/models/training_data.hpp``,
which is the authority. This module is the second implementation of that layout,
and a second implementation of a binary format is a liability unless something
checks the two agree -- so ``tests/test_dataset.py`` reads a file produced by the
real C++ tool rather than one this module wrote, and the round trip is asserted
rather than assumed.

Why the rows are aggregated, and what the label means, are documented in the C++
header and not repeated here. The short version needed to use this module: one
row per node, ``positives`` counts how often that node was the first entry
extracted from its bucket, and ``occurrences`` counts how often it was settled at
all. Both are needed -- a node that never won is not the same observation as a
node that never appeared.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

#: "LPQTRAIN" little-endian, matching kTrainingSetMagic.
MAGIC = 0x4E4941525451504C

#: Matches kTrainingSetFormatVersion. A mismatch is an error, never a best effort.
FORMAT_VERSION = 1

#: Matches kFeatureLayoutVersion. See the note in read_training_set on why this
#: is checked rather than trusted.
FEATURE_LAYOUT_VERSION = 1

#: The static features lpq_collect fills, by their index in the FeatureVector.
#: The rest of the vector is zero, which is a statement that this model does not
#: see those features rather than an approximation of them.
STATIC_FEATURE_NAMES = (
    "out_degree",
    "min_adjacent_weight",
    "mean_adjacent_weight",
    "max_adjacent_weight",
    "adjacent_weight_spread",
    "degree_relative_to_mean",
)
STATIC_FEATURE_INDICES = tuple(range(len(STATIC_FEATURE_NAMES)))


class TrainingSetError(Exception):
    """Raised when a training set is malformed or built against another layout."""


@dataclass(frozen=True)
class TrainingSet:
    """A training set plus the provenance needed to trust it."""

    dataset_id: str
    graph_checksum: int
    bucket_width: int
    query_count: int
    query_seed: int
    feature_layout_version: int

    #: One list of floats per node, length kFeatureCount.
    features: list[list[float]]
    positives: list[int]
    occurrences: list[int]

    def __len__(self) -> int:
        return len(self.features)

    @property
    def total_positives(self) -> int:
        return sum(self.positives)

    @property
    def total_occurrences(self) -> int:
        return sum(self.occurrences)

    @property
    def positive_rate(self) -> float:
        """Share of observations that were positive.

        The number any reported accuracy has to be read against: a model that
        always answers "no" scores ``1 - positive_rate``, and on this task that
        is a high number.
        """
        total = self.total_occurrences
        return (self.total_positives / total) if total else 0.0

    def static_features(self) -> list[list[float]]:
        """Just the columns lpq_collect actually fills.

        Training on the full vector would hand a model 22 columns that are
        identically zero. Nothing breaks, but a regulariser then spends its
        budget on them and a reported coefficient count becomes misleading.
        """
        return [[row[i] for i in STATIC_FEATURE_INDICES] for row in self.features]


def _read_exactly(stream, count: int) -> bytes:
    data = stream.read(count)
    if len(data) != count:
        raise TrainingSetError(
            f"training set truncated: wanted {count} bytes, got {len(data)}"
        )
    return data


def read_training_set(stream) -> TrainingSet:
    """Reads a training set from a binary stream.

    Raises:
        TrainingSetError: on a wrong magic, an unreadable version, a feature
            layout that does not match this build, a truncated file, or a row
            claiming more wins than appearances.
    """
    (magic,) = struct.unpack("<Q", _read_exactly(stream, 8))
    if magic != MAGIC:
        raise TrainingSetError(
            "not a training set: wrong magic. A file that is not a training set "
            "must be rejected as such rather than interpreted as one"
        )

    (format_version,) = struct.unpack("<I", _read_exactly(stream, 4))
    if format_version != FORMAT_VERSION:
        raise TrainingSetError(
            f"training set format version {format_version} cannot be read here "
            f"(expected {FORMAT_VERSION})"
        )

    (layout_version,) = struct.unpack("<I", _read_exactly(stream, 4))
    (feature_count,) = struct.unpack("<I", _read_exactly(stream, 4))

    # The check that matters most. A set built against one feature layout and
    # trained against another puts every value in the wrong column and yields a
    # confident, plausible, wrong model -- and nothing downstream can detect it,
    # because the outputs are the right shape and in the right range.
    if layout_version != FEATURE_LAYOUT_VERSION:
        raise TrainingSetError(
            f"training set was built against feature layout {layout_version} but "
            f"this module expects {FEATURE_LAYOUT_VERSION}; every feature would "
            "be read from the wrong column"
        )

    (name_length,) = struct.unpack("<I", _read_exactly(stream, 4))
    if name_length > 4096:
        raise TrainingSetError(f"implausible dataset id length: {name_length}")
    dataset_id = _read_exactly(stream, name_length).decode("utf-8")

    graph_checksum, bucket_width, query_count, query_seed, row_count = struct.unpack(
        "<QQQQQ", _read_exactly(stream, 40)
    )

    row_struct = struct.Struct(f"<{feature_count}fII")
    features: list[list[float]] = []
    positives: list[int] = []
    occurrences: list[int] = []

    payload = _read_exactly(stream, row_struct.size * row_count)
    for index, values in enumerate(row_struct.iter_unpack(payload)):
        *row_features, row_positives, row_occurrences = values
        # A node cannot win more buckets than it was settled in. This catches a
        # corrupt file and, more usefully, a labelling bug upstream, which would
        # otherwise produce a set that trains without complaint.
        if row_positives > row_occurrences:
            raise TrainingSetError(
                f"row {index} claims {row_positives} positives from "
                f"{row_occurrences} occurrences"
            )
        features.append(list(row_features))
        positives.append(row_positives)
        occurrences.append(row_occurrences)

    return TrainingSet(
        dataset_id=dataset_id,
        graph_checksum=graph_checksum,
        bucket_width=bucket_width,
        query_count=query_count,
        query_seed=query_seed,
        feature_layout_version=layout_version,
        features=features,
        positives=positives,
        occurrences=occurrences,
    )


def read_training_set_file(path: str | Path) -> TrainingSet:
    with open(path, "rb") as handle:
        return read_training_set(handle)
