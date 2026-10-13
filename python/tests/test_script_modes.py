"""The shell scripts the documentation tells people to run directly must be executable.

``CLAUDE.md``, ``ESD.md`` section 36 and ``docs/reproducing.md`` give these as commands
(``scripts/reproduce/build_all.sh``, not ``bash scripts/reproduce/build_all.sh``). A file
tracked as mode 100644 fails that documented command with "Permission denied" on Linux
and macOS. The mode bit is invisible on Windows, which is how it went unnoticed.

The mode is read from the git index (``git ls-files -s``), not from the file system, so
the check means the same on every platform and says what a fresh clone will contain.

``scripts/reproduce/common.sh`` is deliberately absent: it is only ever sourced.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]

DIRECTLY_INVOKED = (
    "scripts/build/linux_checks.sh",
    "scripts/build/refresh_graph.sh",
    "scripts/profile/check_counters.sh",
    "scripts/reproduce/build_all.sh",
    "scripts/reproduce/generate_figures.sh",
    "scripts/reproduce/generate_tables.sh",
    "scripts/reproduce/run_main_experiments.sh",
)


def _index_mode(relative: str) -> str | None:
    """The mode git has recorded for *relative*, or None when it cannot be read."""
    if shutil.which("git") is None:
        return None
    done = subprocess.run(
        ["git", "ls-files", "-s", "--", relative],
        cwd=REPO,
        capture_output=True,
        text=True,
        check=False,
    )
    if done.returncode != 0 or not done.stdout.strip():
        return None
    return done.stdout.split()[0]


@pytest.mark.parametrize("script", DIRECTLY_INVOKED)
def test_directly_invoked_scripts_are_executable_in_git(script: str) -> None:
    mode = _index_mode(script)
    if mode is None:
        pytest.skip("not a git checkout, or git is unavailable")
    assert mode == "100755", (
        f"{script} is tracked as {mode}; the documentation runs it directly, which needs 100755"
    )


@pytest.mark.parametrize("script", DIRECTLY_INVOKED)
def test_directly_invoked_scripts_have_a_bash_shebang_and_lf_endings(script: str) -> None:
    raw = (REPO / script).read_bytes()
    assert raw.startswith(b"#!/usr/bin/env bash\n"), f"{script} must start with an env-bash shebang"
    assert b"\r" not in raw, f"{script} contains CR: a bad-interpreter error on Linux"
