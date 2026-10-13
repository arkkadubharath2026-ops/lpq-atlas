"""Focused checks on the launcher helpers added for a first-time local install (not on the engine).

They run the real shell scripts in a throw-away copy of the ``scripts/`` tree, so they cannot touch the
repository's own ``.env``, pid file or processes. POSIX shells only; skipped where ``bash`` is missing.
"""

from __future__ import annotations

import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.skipif(shutil.which("bash") is None or os.name == "nt", reason="needs bash on a POSIX system")


@pytest.fixture
def tree(tmp_path: Path) -> Path:
    for rel in ("scripts/setup/_common.sh", "scripts/setup/stop.sh", "scripts/reproduce/common.sh"):
        dest = tmp_path / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(REPO / rel, dest)
    return tmp_path


def _clean_env() -> dict[str, str]:
    return {k: v for k, v in os.environ.items() if not k.startswith(("ATLAS_", "LPQ_"))}


def _ports(tree: Path, env: dict[str, str]) -> list[str]:
    done = subprocess.run(
        ["bash", "-c", f'source "{tree}/scripts/setup/_common.sh"; echo "$API_PORT $WEB_PORT ${{ATLAS_MAX_JOBS:-unset}} ${{OTHER_KEY:-unset}}"'],
        capture_output=True, text=True, env=env, check=False,
    )
    assert done.returncode == 0, done.stderr
    return done.stdout.split()


def test_without_an_env_file_the_defaults_apply(tree: Path) -> None:
    assert _ports(tree, _clean_env()) == ["8000", "3000", "unset", "unset"]


def test_an_env_file_sets_only_atlas_and_lpq_keys_and_the_real_environment_wins(tree: Path) -> None:
    (tree / ".env").write_text(
        "# comment\nATLAS_API_PORT=8011\r\nATLAS_WEB_PORT=\"3011\"\nATLAS_MAX_JOBS=3\nOTHER_KEY=leaked\nATLAS_BAD KEY=1\nnot a setting\n"
    )
    assert _ports(tree, _clean_env()) == ["8011", "3011", "3", "unset"]
    env = {**_clean_env(), "ATLAS_API_PORT": "9000"}
    assert _ports(tree, env)[:2] == ["9000", "3011"]


def test_stop_refuses_a_pid_that_is_not_the_launcher_and_leaves_it_running(tree: Path) -> None:
    victim = subprocess.Popen(["sleep", "30"])
    try:
        run_dir = tree / "results" / "atlas" / "launcher"
        run_dir.mkdir(parents=True)
        (run_dir / "run.pid").write_text(f"{victim.pid}\n")
        done = subprocess.run(["bash", str(tree / "scripts/setup/stop.sh")], capture_output=True, text=True,
                              env=_clean_env(), check=False)
        assert done.returncode != 0
        assert "NOT stopped" in done.stderr + done.stdout
        assert victim.poll() is None, "an unrelated process was killed"
    finally:
        victim.send_signal(signal.SIGKILL)
        victim.wait()


def test_stop_with_nothing_running_is_a_no_op(tree: Path) -> None:
    done = subprocess.run(["bash", str(tree / "scripts/setup/stop.sh")], capture_output=True, text=True,
                          env=_clean_env(), check=False)
    assert done.returncode == 0
    assert "no launcher is recorded" in done.stdout


def test_smoke_reports_an_unreachable_ui_as_a_failure_not_a_traceback() -> None:
    start = time.time()
    done = subprocess.run([sys.executable, str(REPO / "scripts/setup/smoke.py"), "--web", "http://127.0.0.1:9"],
                          capture_output=True, text=True, check=False, timeout=60)
    assert done.returncode == 1
    assert "[FAIL]" in done.stderr and "Traceback" not in done.stderr
    assert time.time() - start < 30
