"""Cross-language verification of the model file format.

``lpq_tools.training.model_io`` is a second implementation of the layout defined
in ``cpp/include/lpq/models/serialization.hpp``. Two implementations of a binary
format that are never checked against each other will drift, and the drift is
silent: a model whose columns are offset by one still loads, still predicts, and
still produces numbers of the right shape in the right range.

So these tests do not round-trip within Python. They write a file with the
Python writer and read it back with the REAL C++ loader, through
``lpq_model_info``, and assert the values survive.

The C++ tool is located the same way every other tool in this package is, so a
developer who has not built it gets a skip with the build command rather than a
confusing failure.
"""

from __future__ import annotations

import subprocess

import pytest

from lpq_tools.training.model_io import (
    FEATURE_COUNT,
    ModelKind,
    OnlineConfig,
    expand_static_weights,
    write_linear_model,
)
from lpq_tools.utilities.paths import RepoPaths


def _model_info_tool():
    paths = RepoPaths.discover()
    try:
        return paths.tool("lpq_model_info")
    except FileNotFoundError as error:
        pytest.skip(str(error))


def _read_back(model_path) -> dict[str, str]:
    """Loads a model with the C++ engine and returns its printed fields."""
    result = subprocess.run(
        [str(_model_info_tool()), "--model", str(model_path)],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, (
        f"the C++ loader rejected a file this package wrote:\n{result.stderr}"
    )
    fields = {}
    for line in result.stdout.splitlines():
        if ":" in line:
            key, _, value = line.partition(":")
            fields[key.strip()] = value.strip()
    return fields


# -----------------------------------------------------------------------------
# The engine can load what this package writes
# -----------------------------------------------------------------------------


def test_cpp_loads_a_python_written_logistic_model(tmp_path):
    weights = expand_static_weights([0.5, -0.25, 0.125, 2.0, -1.5, 0.0625])
    path = tmp_path / "logistic.lpqmodel"
    write_linear_model(path, weights, bias=-0.75, kind=ModelKind.LOGISTIC)

    fields = _read_back(path)

    assert fields["kind"] == "logistic"
    assert int(fields["feature_count"]) == FEATURE_COUNT


def test_cpp_reads_back_the_exact_weights(tmp_path):
    # The values are chosen to be exactly representable in float32, so a
    # mismatch here is a layout error and never a rounding artefact.
    static = [0.5, -0.25, 0.125, 2.0, -1.5, 0.0625]
    path = tmp_path / "logistic.lpqmodel"
    write_linear_model(path, expand_static_weights(static), bias=-0.75)

    fields = _read_back(path)

    assert float(fields["weight.out_degree"]) == 0.5
    assert float(fields["weight.min_adjacent_weight"]) == -0.25
    assert float(fields["weight.mean_adjacent_weight"]) == 0.125
    assert float(fields["weight.max_adjacent_weight"]) == 2.0
    assert float(fields["weight.adjacent_weight_spread"]) == -1.5
    assert float(fields["weight.degree_relative_to_mean"]) == 0.0625
    assert float(fields["bias"]) == -0.75


def test_columns_are_not_offset(tmp_path):
    """The failure this whole file exists to catch.

    A layout that is off by one loads without complaint and predicts plausible
    nonsense. Giving every static column a distinct value and checking each by
    name is what turns that into a test failure.
    """
    static = [1.0, 2.0, 4.0, 8.0, 16.0, 32.0]
    path = tmp_path / "offset.lpqmodel"
    write_linear_model(path, expand_static_weights(static), bias=0.0)

    fields = _read_back(path)

    assert float(fields["weight.out_degree"]) == 1.0
    assert float(fields["weight.degree_relative_to_mean"]) == 32.0
    # And the columns the collector never fills must be exactly zero, not
    # whatever happened to follow the six in memory.
    assert float(fields["weight.tentative_distance"]) == 0.0
    assert float(fields["weight.queue_size"]) == 0.0


def test_a_linear_model_is_tagged_as_linear(tmp_path):
    # Linear and logistic share a parameter block and differ only in how the
    # score is read, so the tag is the only thing that tells them apart.
    path = tmp_path / "linear.lpqmodel"
    write_linear_model(
        path, expand_static_weights([1.0]), bias=0.0, kind=ModelKind.LINEAR
    )

    assert _read_back(path)["kind"] == "linear"


def test_the_engine_accepts_a_non_default_online_config(tmp_path):
    # These parameters are validated on load, not merely stored, so a writer
    # that emitted them in the wrong order would be caught here rather than
    # producing a model that fails much later during adaptation.
    path = tmp_path / "configured.lpqmodel"
    write_linear_model(
        path,
        expand_static_weights([0.5]),
        bias=0.0,
        config=OnlineConfig(
            learning_rate=0.05, l2=0.001, positive_weight=8.0, max_weight_delta=0.5
        ),
    )

    assert _read_back(path)["kind"] == "logistic"


# -----------------------------------------------------------------------------
# Refusals in the writer
# -----------------------------------------------------------------------------


def test_refuses_a_short_weight_vector(tmp_path):
    # Zero-padding a short vector would silently produce a model whose columns
    # are offset from the layout the file claims.
    with pytest.raises(ValueError, match="weights"):
        write_linear_model(tmp_path / "bad.lpqmodel", [0.1, 0.2], bias=0.0)


def test_refuses_a_non_positive_temperature(tmp_path):
    # A non-positive temperature inverts every confidence the model reports.
    with pytest.raises(ValueError, match="temperature"):
        write_linear_model(
            tmp_path / "bad.lpqmodel",
            expand_static_weights([1.0]),
            bias=0.0,
            temperature=0.0,
        )


def test_refuses_a_kind_that_does_not_share_the_linear_block(tmp_path):
    with pytest.raises(ValueError, match="linear parameter block"):
        write_linear_model(
            tmp_path / "bad.lpqmodel",
            expand_static_weights([1.0]),
            bias=0.0,
            kind=ModelKind.TREE_ENSEMBLE,
        )


def test_expanding_static_weights_leaves_the_rest_zero():
    full = expand_static_weights([1.0, 2.0])

    assert len(full) == FEATURE_COUNT
    assert full[:2] == [1.0, 2.0]
    assert set(full[2:]) == {0.0}


def test_expanding_refuses_more_weights_than_columns():
    with pytest.raises(ValueError, match="exceed"):
        expand_static_weights([0.0] * (FEATURE_COUNT + 1))
