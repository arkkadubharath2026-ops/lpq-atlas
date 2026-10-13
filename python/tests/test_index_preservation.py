"""Preparing one dataset must not erase the others from ``datasets/metadata/index.json``.

``index.json`` is committed. ``scripts/data/prepare_datasets.py`` used to write it from only
the datasets it had just prepared, so ``prepare_datasets.py osm_monaco`` replaced the whole
file with a single entry and a ``git diff`` showed four datasets deleted. The documentation
called that diff "expected". These tests pin the corrected behaviour at two levels:

* ``pipeline.update_index`` merges a summary into an existing index;
* the command line, end to end with the slow parts stubbed, preserves what it did not touch.

The tests that call ``pipeline.update_index`` look it up at call time, so each one fails on
its own when the function is absent instead of the whole module failing to import.
"""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace

import pytest

from lpq_tools.data import pipeline
from lpq_tools.data.pipeline import PipelineError
from lpq_tools.utilities.jsonio import write_json

REPO = Path(__file__).resolve().parents[2]


def entry(dataset_id: str, nodes: int = 10, checksum: int = 111) -> dict:
    """A shape-faithful index entry (the keys ``summarise`` writes)."""
    return {
        "arc_count": nodes * 2,
        "dataset_id": dataset_id,
        "directed": True,
        "edge_count": nodes * 2,
        "files_accepted_on_trust": [],
        "graph_checksum_fnv1a64": checksum,
        "graph_path": f"datasets/processed/{dataset_id}.lpqgraph",
        "has_coordinates": True,
        "manifest_path": f"experiments/manifests/{dataset_id}_queries.yaml",
        "metadata_path": f"datasets/metadata/{dataset_id}.json",
        "node_count": nodes,
        "weight_type": "uint32",
    }


def write_index(path: Path, entries: list[dict], **extra: object) -> str:
    write_json(path, {"datasets": entries, **extra})
    return path.read_text(encoding="utf-8")


def block(text: str, dataset_id: str) -> tuple[int, int]:
    """Line range [start, end) of the object holding *dataset_id* in canonical output."""
    lines = text.splitlines()
    marker = next(i for i, ln in enumerate(lines) if f'"dataset_id": "{dataset_id}"' in ln)
    start = marker
    while lines[start].strip() != "{":
        start -= 1
    end = marker
    while not lines[end].startswith("    }"):
        end += 1
    return start, end + 1


# -----------------------------------------------------------------------------
# pipeline.update_index
# -----------------------------------------------------------------------------


def test_replacing_one_entry_leaves_every_other_line_of_the_file_untouched(tmp_path):
    index = tmp_path / "index.json"
    before = write_index(index, [entry("a"), entry("b"), entry("c"), entry("d")])

    pipeline.update_index(index, {"datasets": [entry("c", nodes=999, checksum=222)]})

    after = index.read_text(encoding="utf-8")
    old_lo, old_hi = block(before, "c")
    new_lo, new_hi = block(after, "c")
    # Everything before and after the replaced entry is the same text, byte for byte.
    assert after.splitlines()[:new_lo] == before.splitlines()[:old_lo]
    assert after.splitlines()[new_hi:] == before.splitlines()[old_hi:]
    updated = json.loads(after)["datasets"]
    assert [e["dataset_id"] for e in updated] == ["a", "b", "c", "d"]
    assert updated[2]["node_count"] == 999 and updated[2]["graph_checksum_fnv1a64"] == 222


def test_a_new_dataset_is_inserted_in_sorted_order_and_the_rest_is_preserved(tmp_path):
    index = tmp_path / "index.json"
    write_index(index, [entry("a"), entry("c")])
    originals = {e["dataset_id"]: e for e in json.loads(index.read_text())["datasets"]}

    pipeline.update_index(index, {"datasets": [entry("b", nodes=5)]})

    merged = json.loads(index.read_text())["datasets"]
    assert [e["dataset_id"] for e in merged] == ["a", "b", "c"]
    assert merged[0] == originals["a"] and merged[2] == originals["c"]


def test_a_missing_index_is_created_from_the_summary(tmp_path):
    index = tmp_path / "metadata" / "index.json"

    pipeline.update_index(index, {"datasets": [entry("only")]})

    assert json.loads(index.read_text())["datasets"] == [entry("only")]


def test_updating_twice_with_the_same_summary_changes_nothing(tmp_path):
    index = tmp_path / "index.json"
    write_index(index, [entry("a"), entry("b")])
    summary = {"datasets": [entry("b", nodes=77)]}

    pipeline.update_index(index, summary)
    once = index.read_bytes()
    pipeline.update_index(index, summary)

    assert index.read_bytes() == once


def test_an_empty_summary_does_not_touch_the_file(tmp_path):
    index = tmp_path / "index.json"
    write_index(index, [entry("a"), entry("b")])
    before = index.read_bytes()

    pipeline.update_index(index, {"datasets": []})

    assert index.read_bytes() == before


def test_unknown_top_level_keys_survive(tmp_path):
    index = tmp_path / "index.json"
    write_index(index, [entry("a")], schema_note="kept")

    pipeline.update_index(index, {"datasets": [entry("a", nodes=3)]})

    assert json.loads(index.read_text())["schema_note"] == "kept"


@pytest.mark.parametrize(
    "bad_text",
    [
        "{ this is not json",
        "[]",
        '{"datasets": "nope"}',
        '{"datasets": [{"node_count": 1}]}',
        '{"datasets": [{"dataset_id": "a"}, {"dataset_id": "a"}]}',
    ],
    ids=["not-json", "not-an-object", "datasets-not-a-list", "entry-without-id", "duplicate-id"],
)
def test_an_unreadable_index_is_refused_and_never_overwritten(tmp_path, bad_text):
    index = tmp_path / "index.json"
    index.write_text(bad_text, encoding="utf-8")

    with pytest.raises(PipelineError):
        pipeline.update_index(index, {"datasets": [entry("a")]})

    assert index.read_text(encoding="utf-8") == bad_text


# -----------------------------------------------------------------------------
# The command line, with the slow parts stubbed
# -----------------------------------------------------------------------------


def load_cli():
    spec = importlib.util.spec_from_file_location(
        "prepare_datasets_under_test", REPO / "scripts" / "data" / "prepare_datasets.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def stub_cli(monkeypatch, tmp_path, declared, prepared, failures=()):
    """Wires the CLI to a throwaway repository and a pipeline that 'prepares' *prepared*."""
    cli = load_cli()
    paths = SimpleNamespace(
        root=tmp_path, dataset_configs=tmp_path / "configs", metadata=tmp_path / "metadata"
    )
    paths.metadata.mkdir(exist_ok=True)

    class FakePaths:
        @staticmethod
        def discover():
            return paths

    class FakeResult:
        def __init__(self, dataset_id):
            self.dataset_id = dataset_id

        def summary(self):
            return f"{self.dataset_id}: prepared (stub)"

    def fake_prepare_all(selected, paths_, **kwargs):
        wanted = {c.dataset_id for c in selected}
        return [FakeResult(i) for i in prepared if i in wanted], list(failures)

    monkeypatch.setattr(cli, "RepoPaths", FakePaths)
    monkeypatch.setattr(
        cli,
        "load_all_dataset_configs",
        lambda _dir: [
            SimpleNamespace(dataset_id=i, coordinate_file=None, source_format="stub", weights=None)
            for i in declared
        ],
    )
    monkeypatch.setattr(cli, "prepare_all", fake_prepare_all)
    monkeypatch.setattr(
        cli,
        "summarise",
        lambda results, root: {"datasets": [entry(r.dataset_id, nodes=4242) for r in results]},
    )
    return cli, paths.metadata / "index.json"


def test_preparing_one_dataset_from_the_command_line_keeps_the_other_entries(
    monkeypatch, tmp_path, capsys
):
    cli, index = stub_cli(monkeypatch, tmp_path, ["a", "b", "c"], prepared=["b"])
    before = write_index(index, [entry("a"), entry("b"), entry("c")])

    code = cli.main(["b"])

    assert code == 0
    merged = {e["dataset_id"]: e for e in json.loads(index.read_text())["datasets"]}
    assert set(merged) == {"a", "b", "c"}, "the untouched datasets must still be in the index"
    assert merged["b"]["node_count"] == 4242
    old = {e["dataset_id"]: e for e in json.loads(before)["datasets"]}
    assert merged["a"] == old["a"] and merged["c"] == old["c"]
    capsys.readouterr()


def test_a_partly_failed_run_still_preserves_the_index_and_reports_failure(
    monkeypatch, tmp_path, capsys
):
    cli, index = stub_cli(
        monkeypatch, tmp_path, ["a", "b", "c"], prepared=["a", "b"], failures=[("c", "no network")]
    )
    write_index(index, [entry("a"), entry("b"), entry("c", nodes=31)])

    code = cli.main([])

    assert code == 1, "a partial run is a failure"
    merged = {e["dataset_id"]: e for e in json.loads(index.read_text())["datasets"]}
    assert set(merged) == {"a", "b", "c"}
    assert merged["c"]["node_count"] == 31, "the entry for the dataset that failed is kept as it was"
    assert merged["a"]["node_count"] == 4242
    capsys.readouterr()
