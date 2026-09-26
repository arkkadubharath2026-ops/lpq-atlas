"""Background jobs with file-backed status (spec sections 24, 25 and 37).

A single-user research deployment does not need a database or a broker: each
job is a directory holding ``status.json`` plus its artifacts, and a bounded
thread pool runs the engine subprocesses. Status survives a restart; a job
that was in flight when the process died is reported FAILED on reload rather
than left looking alive.
"""

from __future__ import annotations

import json
import logging
import subprocess
import threading
import time
import uuid
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

log = logging.getLogger("atlas.jobs")

QUEUED = "QUEUED"
PREPARING = "PREPARING"
RUNNING = "RUNNING"
FINALIZING = "FINALIZING"
COMPLETED = "COMPLETED"
FAILED = "FAILED"
CANCELLED = "CANCELLED"
TERMINAL = {COMPLETED, FAILED, CANCELLED}


class JobCancelled(Exception):
    pass


class JobFailure(Exception):
    """A failure with a category and a message that is safe to show a user."""

    def __init__(self, category: str, message: str) -> None:
        super().__init__(message)
        self.category = category
        self.message = message


@dataclass
class Job:
    id: str
    kind: str
    dir: Path
    state: str = QUEUED
    created_at: float = field(default_factory=time.time)
    updated_at: float = field(default_factory=time.time)
    error: dict[str, str] | None = None
    progress: str | None = None
    summary: dict[str, Any] = field(default_factory=dict)
    cancel_event: threading.Event = field(default_factory=threading.Event)
    process: subprocess.Popen[str] | None = None

    def public(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "kind": self.kind,
            "state": self.state,
            "created_at": self.created_at,
            "updated_at": self.updated_at,
            "error": self.error,
            "progress": self.progress,
            "summary": self.summary,
        }


class JobManager:
    def __init__(self, base: Path, kind: str, workers: int) -> None:
        self.base = base
        self.kind = kind
        self._pool = ThreadPoolExecutor(max_workers=workers, thread_name_prefix=f"atlas-{kind}")
        self._jobs: dict[str, Job] = {}
        self._lock = threading.Lock()
        self._reload()

    # -- persistence ---------------------------------------------------------

    def _reload(self) -> None:
        for status in self.base.glob("*/status.json"):
            try:
                data = json.loads(status.read_text())
            except (OSError, ValueError):
                continue
            job = Job(
                id=data["id"],
                kind=data.get("kind", self.kind),
                dir=status.parent,
                state=data.get("state", FAILED),
                created_at=data.get("created_at", 0.0),
                updated_at=data.get("updated_at", 0.0),
                error=data.get("error"),
                progress=data.get("progress"),
                summary=data.get("summary", {}),
            )
            if job.state not in TERMINAL:
                job.state = FAILED
                job.error = {"category": "interrupted", "message": "the server stopped mid-job"}
                self._save(job)
            self._jobs[job.id] = job

    def _save(self, job: Job) -> None:
        job.updated_at = time.time()
        tmp = job.dir / "status.json.tmp"
        tmp.write_text(json.dumps(job.public(), indent=1))
        tmp.replace(job.dir / "status.json")

    # -- API -----------------------------------------------------------------

    def submit(
        self,
        request: dict[str, Any],
        work: Callable[[Job, JobManager], dict[str, Any]],
        summary: dict[str, Any] | None = None,
    ) -> Job:
        job_id = f"{self.kind[:3]}_{uuid.uuid4().hex[:16]}"
        d = self.base / job_id
        d.mkdir(parents=True)
        (d / "request.json").write_text(json.dumps(request, indent=1))
        job = Job(id=job_id, kind=self.kind, dir=d, summary=summary or {})
        with self._lock:
            self._jobs[job_id] = job
        self._save(job)
        log.info("job queued", extra={"job_id": job_id, "kind": self.kind})
        self._pool.submit(self._run, job, work)
        return job

    def get(self, job_id: str) -> Job:
        with self._lock:
            return self._jobs[job_id]

    def list(self, limit: int = 50) -> list[Job]:
        with self._lock:
            jobs = sorted(self._jobs.values(), key=lambda j: j.created_at, reverse=True)
        return jobs[:limit]

    def cancel(self, job_id: str) -> Job:
        job = self.get(job_id)
        if job.state in TERMINAL:
            return job
        job.cancel_event.set()
        proc = job.process
        if proc is not None and proc.poll() is None:
            proc.kill()
        if job.state == QUEUED:
            self.set_state(job, CANCELLED)
        return job

    def set_state(self, job: Job, state: str, progress: str | None = None) -> None:
        job.state = state
        if progress is not None:
            job.progress = progress
        self._save(job)
        log.info("job state", extra={"job_id": job.id, "state": state, "progress": progress})

    def check_cancel(self, job: Job) -> None:
        if job.cancel_event.is_set():
            raise JobCancelled

    def _run(self, job: Job, work: Callable[[Job, JobManager], dict[str, Any]]) -> None:
        if job.cancel_event.is_set():
            self.set_state(job, CANCELLED)
            return
        started = time.monotonic()
        try:
            self.set_state(job, PREPARING)
            job.summary.update(work(job, self) or {})
            self.set_state(job, COMPLETED)
        except JobCancelled:
            self.set_state(job, CANCELLED)
        except JobFailure as e:
            job.error = {"category": e.category, "message": e.message}
            self.set_state(job, FAILED)
        except Exception:  # an unexpected failure must still end the job
            log.exception("job crashed", extra={"job_id": job.id})
            job.error = {"category": "internal", "message": "unexpected server error"}
            self.set_state(job, FAILED)
        finally:
            job.process = None
            log.info(
                "job finished",
                extra={"job_id": job.id, "state": job.state, "seconds": time.monotonic() - started},
            )


def run_engine(
    job: Job,
    manager: JobManager,
    argv: list[str],
    cwd: Path,
    timeout_s: int,
    log_name: str = "engine.log",
) -> int:
    """Runs one engine process for ``job``: argv list (never a shell), timeout, cancel."""
    manager.check_cancel(job)
    log_path = job.dir / log_name
    with log_path.open("w", encoding="utf-8") as logf:
        logf.write("argv: " + json.dumps([Path(argv[0]).name, *argv[1:]]) + "\n")
        logf.flush()
        proc = subprocess.Popen(  # noqa: S603 -- argv built from validated enums
            argv, cwd=str(cwd), stdout=logf, stderr=subprocess.STDOUT, text=True
        )
        job.process = proc
        try:
            code = proc.wait(timeout=timeout_s)
        except subprocess.TimeoutExpired as exc:
            proc.kill()
            proc.wait()
            raise JobFailure("timeout", f"the engine did not finish within {timeout_s}s") from exc
        finally:
            job.process = None
    if job.cancel_event.is_set():
        raise JobCancelled
    return code
