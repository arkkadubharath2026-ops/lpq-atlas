"""Writing model files the C++ engine can load.

The layout is defined by ``cpp/include/lpq/models/serialization.hpp`` and
``linear_predictor.hpp``; this is the second implementation of it. A second
implementation of a binary format is only safe if something checks the two
agree, so the tests here write a model with this module and load it with the
real C++ predictor, rather than round-tripping within Python.

The header carries three guards and all three matter. The magic stops a file
that is not a model being interpreted as one. The format version stops an old
container being misread. The **feature layout version** stops a model trained
against one column order being evaluated against another -- the dangerous case,
because it fails silently: every feature is read from the wrong column and the
outputs are still the right shape, in the right range, and confidently wrong.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

#: "LPQMODEL" little-endian, matching kModelMagic.
MAGIC = 0x4C45444F4D51504C

#: Matches kModelFormatVersion.
FORMAT_VERSION = 1

#: Matches kFeatureLayoutVersion.
FEATURE_LAYOUT_VERSION = 1

#: Matches kFeatureCount -- the length of FeatureVector, not the number of
#: features a given model actually uses.
FEATURE_COUNT = 28


class ModelKind:
    """Mirrors the C++ ModelKind enum."""

    HEURISTIC = 1
    LINEAR = 2
    LOGISTIC = 3
    MLP = 4
    TREE_ENSEMBLE = 5


@dataclass(frozen=True)
class OnlineConfig:
    """The online-learning parameters stored alongside the weights.

    These are not used to make a prediction. They are the configuration the
    *adaptive* path would continue training under, and they are written because
    the C++ loader reads and validates them -- omitting them would produce a
    file that is truncated rather than one that merely lacks a feature.

    The defaults mirror the C++ side. ``positive_weight`` is above one because
    the label distribution is severely unbalanced: roughly one node per bucket
    per query is positive, so a model minimising unweighted loss does well by
    answering "no" always.
    """

    learning_rate: float = 0.01
    l2: float = 0.0001
    positive_weight: float = 1.0
    max_weight_delta: float = 1.0


def write_linear_model(
    path: str | Path,
    weights: list[float],
    bias: float,
    *,
    kind: int = ModelKind.LOGISTIC,
    temperature: float = 1.0,
    config: OnlineConfig | None = None,
    updates: int = 0,
) -> None:
    """Writes a linear or logistic model in the engine's format.

    Args:
        weights: one weight per feature. Must be exactly FEATURE_COUNT long --
            a short list is rejected rather than zero-padded, because padding
            would quietly produce a model whose columns are offset from the
            layout it claims.
        bias: the intercept.
        kind: ModelKind.LINEAR or ModelKind.LOGISTIC. The two share a body and
            differ only in how the score is read, so the tag is what tells the
            loader which to build.
        temperature: calibration temperature. Must be positive; the C++ loader
            validates it, because a non-positive value inverts every confidence.
        updates: how many online updates produced these weights. Zero for a
            model fitted offline, which is what this module does.

    Raises:
        ValueError: on a wrong-length weight vector, a non-positive temperature,
            or an unsupported kind.
    """
    if len(weights) != FEATURE_COUNT:
        raise ValueError(
            f"expected {FEATURE_COUNT} weights (the full FeatureVector length), "
            f"got {len(weights)}. Padding a short vector here would offset every "
            "column from the layout the file claims to use"
        )
    if not temperature > 0.0:
        raise ValueError(
            f"temperature must be positive, got {temperature}: a non-positive "
            "temperature inverts every confidence the model reports"
        )
    if kind not in (ModelKind.LINEAR, ModelKind.LOGISTIC):
        raise ValueError(
            f"kind {kind} does not share the linear parameter block; only "
            "LINEAR and LOGISTIC can be written here"
        )

    cfg = config or OnlineConfig()

    with open(path, "wb") as handle:
        handle.write(struct.pack("<Q", MAGIC))
        handle.write(
            struct.pack("<III", FORMAT_VERSION, FEATURE_LAYOUT_VERSION, FEATURE_COUNT)
        )
        handle.write(struct.pack("<I", kind))

        handle.write(struct.pack(f"<{FEATURE_COUNT}f", *weights))
        handle.write(struct.pack("<f", bias))
        handle.write(struct.pack("<f", temperature))
        handle.write(struct.pack("<f", cfg.learning_rate))
        handle.write(struct.pack("<f", cfg.l2))
        handle.write(struct.pack("<f", cfg.positive_weight))
        handle.write(struct.pack("<f", cfg.max_weight_delta))
        handle.write(struct.pack("<Q", updates))


def expand_static_weights(static_weights: list[float]) -> list[float]:
    """Places static-feature weights into a full-length FeatureVector.

    A model fitted on the six static columns has six coefficients, but the file
    format carries one per feature. The remaining columns are written as zero,
    which is exactly right: the collector leaves those features zero, so any
    weight on them would multiply nothing.
    """
    if len(static_weights) > FEATURE_COUNT:
        raise ValueError(
            f"{len(static_weights)} static weights exceed the {FEATURE_COUNT}-column layout"
        )
    full = [0.0] * FEATURE_COUNT
    for index, value in enumerate(static_weights):
        full[index] = value
    return full
