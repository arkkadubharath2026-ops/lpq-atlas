"""Errors shown to a client name what failed, not where the server lives (spec sections 35 and 36).

Found in M11: a failed benchmark's message carried the tail of the engine log, whose first line is the
argv with absolute host paths; and a failed download in a preparation job was reported as an
"unexpected server error" because its exception type was not one the job caught.
"""

from __future__ import annotations

import os
import re
import time
from pathlib import Path
from typing import Any

import pytest

from .conftest import FIXTURE_ID

_ABS = re.compile(r"(?<![\w:/.>\-])/(?:[\w.@+\-]+/){2,}")


def _wait(client: Any, path: str) -> dict[str, Any]:
    deadline = time.time() + 60
    while time.time() < deadline:
        d = client.get(path).json()
        if d["state"] in ("COMPLETED", "FAILED", "CANCELLED"):
            return d
        time.sleep(0.1)
    raise AssertionError(f"{path} did not finish")


def test_scrub_replaces_known_roots_then_any_other_absolute_path() -> None:
    from atlas_api.jobs import scrub_host_paths

    roots = [(Path("/srv/repo"), "<repo>"), (Path("/srv/repo/results/atlas"), "<work>")]
    text = "failed: /srv/repo/results/atlas/runs/x/engine.log and /srv/repo/data/a.bin and /etc/ssl/certs/ca.pem"
    out = scrub_host_paths(text, roots)
    assert out == "failed: <work>/runs/x/engine.log and <repo>/data/a.bin and <path>"


def test_scrub_leaves_urls_relative_paths_and_plain_text_alone() -> None:
    from atlas_api.jobs import scrub_host_paths

    text = "https://download.example.org/europe/monaco.osm.pbf: [Errno 104] Connection reset; see data/raw/x.pbf"
    assert scrub_host_paths(text, []) == text


def test_a_failed_benchmark_does_not_reveal_host_paths(
    client: Any, fixture_root: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    fake = tmp_path / "lpq_benchmark"
    fake.write_text('#!/bin/sh\necho "error: simulated engine failure" >&2\nexit 3\n')
    fake.chmod(0o755)
    monkeypatch.setenv("LPQ_BENCHMARK", str(fake))

    r = client.post(
        "/api/benchmarks",
        json={"dataset_id": FIXTURE_ID, "queues": ["binary_heap"], "query_count": 4,
              "repetitions": 3, "warmup": 0},
    )
    assert r.status_code == 202, r.text
    job = _wait(client, f"/api/benchmarks/{r.json()['id']}")
    assert job["state"] == "FAILED"
    assert job["error"]["category"] == "engine"
    message = job["error"]["message"]
    assert "simulated engine failure" in message  # still says what went wrong
    assert str(fixture_root) not in message
    assert os.environ["ATLAS_WORK_DIR"] not in message
    assert not _ABS.search(message), message
    # The log in the job directory keeps the argv for the operator.
    log = Path(os.environ["ATLAS_WORK_DIR"]) / "benchmarks" / job["id"] / "engine.log"
    assert log.read_text().startswith("argv: ")


def test_a_failed_download_is_a_preparation_failure_not_an_internal_error(
    client: Any, fixture_root: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    import atlas_api.app as app_module
    from lpq_tools.data.acquire import AcquisitionError

    cfg = fixture_root / "configs" / "datasets" / "fx_download.yaml"
    cfg.parent.mkdir(parents=True, exist_ok=True)
    cfg.write_text("placeholder: true\n")
    monkeypatch.setattr(app_module, "load_dataset_config", lambda _path: object())

    def refuse(*_a: Any, **_k: Any) -> None:
        raise AcquisitionError("https://download.example.org/x.osm.pbf: [Errno 104] Connection reset by peer")

    monkeypatch.setattr(app_module, "prepare_dataset", refuse)

    r = client.post("/api/datasets/fx_download/prepare")
    assert r.status_code == 202, r.text
    job = _wait(client, f"/api/preparations/{r.json()['id']}")
    assert job["state"] == "FAILED"
    assert job["error"]["category"] == "preparation"
    assert "Connection reset by peer" in job["error"]["message"]
    assert "download.example.org" in job["error"]["message"]  # a public URL is not a host path
