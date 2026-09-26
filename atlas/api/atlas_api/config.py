"""Runtime configuration for the LPQ Atlas orchestration API.

Every path is derived from the repository root, never from a request, so no
client-supplied string ever reaches the filesystem (spec section 36).
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]

from lpq_tools.utilities.paths import RepoPaths


@dataclass(frozen=True)
class Settings:
    root: Path
    work_dir: Path
    max_concurrent_jobs: int
    run_timeout_s: int
    benchmark_timeout_s: int
    max_trace_events: int
    max_trace_bytes: int
    max_viewport_edges: int

    @property
    def paths(self) -> RepoPaths:
        return RepoPaths(self.root)

    @property
    def runs_dir(self) -> Path:
        return self.work_dir / "runs"

    @property
    def comparisons_dir(self) -> Path:
        return self.work_dir / "comparisons"

    @property
    def benchmarks_dir(self) -> Path:
        return self.work_dir / "benchmarks"

    @property
    def cache_dir(self) -> Path:
        return self.work_dir / "cache"

    def tool(self, name: str) -> Path:
        return self.paths.tool(name)


def _int_env(name: str, default: int) -> int:
    raw = os.environ.get(name)
    return int(raw) if raw else default


@lru_cache(maxsize=1)
def get_settings() -> Settings:
    root = Path(os.environ.get("ATLAS_REPO_ROOT", str(REPO_ROOT))).resolve()
    work = Path(os.environ.get("ATLAS_WORK_DIR", str(root / "results" / "atlas"))).resolve()
    s = Settings(
        root=root,
        work_dir=work,
        max_concurrent_jobs=_int_env("ATLAS_MAX_JOBS", 2),
        run_timeout_s=_int_env("ATLAS_RUN_TIMEOUT_S", 180),
        benchmark_timeout_s=_int_env("ATLAS_BENCHMARK_TIMEOUT_S", 1800),
        max_trace_events=_int_env("ATLAS_MAX_TRACE_EVENTS", 400_000),
        max_trace_bytes=_int_env("ATLAS_MAX_TRACE_BYTES", 200 * 1024 * 1024),
        max_viewport_edges=_int_env("ATLAS_MAX_VIEWPORT_EDGES", 60_000),
    )
    for d in (s.runs_dir, s.comparisons_dir, s.benchmarks_dir, s.cache_dir):
        d.mkdir(parents=True, exist_ok=True)
    return s
