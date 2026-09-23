"""Download and verification -- the first two steps of ESD.md section 26.

    download -> validate -> normalize -> convert -> metadata -> manifest -> checksum
    ^^^^^^^^^^^^^^^^^^^^^

ESD.md section 26's standing rule is **never modify raw datasets in place**, and
this module is where that rule is actually enforced rather than merely
respected:

  * a download lands in a temporary file and is renamed into ``datasets/raw``
    only after its digest has been checked, so an interrupted transfer cannot
    leave a short file that later looks like a complete one;
  * the file is then made read-only, so a later step that tries to decompress,
    rewrite or "fix" a raw file fails loudly instead of quietly editing the
    record of what was published;
  * decompression writes to ``datasets/processed/staging``, never back into
    ``raw``.

Re-running is cheap: a file already present with the right digest is not
downloaded again.
"""

from __future__ import annotations

import gzip
import os
import shutil
import ssl
import stat
import tempfile
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from lpq_tools.data.sources import DatasetConfig, RemoteFile
from lpq_tools.utilities.hashing import sha256_file
from lpq_tools.utilities.jsonio import read_json, write_json

#: Sent because several dataset hosts reject the default urllib agent outright.
_USER_AGENT = "lpq-dataset-pipeline/1.0 (+research; contact via repository)"

_READ_ONLY = stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH


def _ssl_context() -> ssl.SSLContext:
    """Builds a verifying TLS context, preferring certifi's CA bundle.

    Python on Windows does not read the system certificate store, so several
    university-hosted dataset mirrors -- including the DIMACS challenge host --
    fail with CERTIFICATE_VERIFY_FAILED even though their certificates are
    perfectly valid. certifi carries the same roots a browser would use and
    resolves it.

    Verification is never disabled. The obvious "fix" of passing an unverified
    context would make every download silently accept any server, which for a
    pipeline whose whole purpose is knowing exactly which bytes it got is worse
    than not downloading at all. If certifi is unavailable the system default is
    used, and a genuine verification failure is reported rather than bypassed.
    """
    try:
        import certifi

        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        return ssl.create_default_context()


class AcquisitionError(RuntimeError):
    """Raised when a file cannot be fetched or fails verification."""


@dataclass(frozen=True)
class AcquiredFile:
    """The outcome of acquiring one file."""

    remote: RemoteFile
    path: Path
    sha256: str

    #: True when the file was already present and verified, so nothing was
    #: transferred.
    cached: bool

    #: True when the config declared no expected digest, so the observed one was
    #: recorded rather than checked. Surfaced all the way up to the report,
    #: because "verified" and "accepted on trust" are different claims.
    trusted_on_first_use: bool


def _make_writable(path: Path) -> None:
    """Restores write permission, so a raw file can be replaced deliberately."""
    if path.exists():
        os.chmod(path, stat.S_IWUSR | stat.S_IRUSR)


def _make_read_only(path: Path) -> None:
    try:
        os.chmod(path, _READ_ONLY)
    except OSError:
        # A filesystem that refuses the mode change (some network mounts) is not
        # a reason to fail the download; the guarantee weakens from enforced to
        # conventional, and nothing downstream depends on it being enforced.
        pass


def download_file(
    url: str,
    destination: Path,
    *,
    progress: Callable[[int, int | None], None] | None = None,
    timeout: float = 60.0,
) -> None:
    """Fetches *url* into *destination*, atomically.

    The transfer goes to a temporary file in the destination's directory and is
    renamed on success, so a failure never leaves a partial file where a
    complete one is expected.
    """
    destination.parent.mkdir(parents=True, exist_ok=True)
    request = urllib.request.Request(url, headers={"User-Agent": _USER_AGENT})

    tmp_path: Path | None = None
    try:
        with urllib.request.urlopen(request, timeout=timeout, context=_ssl_context()) as response:
            declared = response.headers.get("Content-Length")
            total = int(declared) if declared and declared.isdigit() else None

            handle = tempfile.NamedTemporaryFile(
                "wb", dir=str(destination.parent), delete=False, suffix=".part"
            )
            tmp_path = Path(handle.name)
            transferred = 0
            try:
                while True:
                    chunk = response.read(1 << 20)
                    if not chunk:
                        break
                    handle.write(chunk)
                    transferred += len(chunk)
                    if progress is not None:
                        progress(transferred, total)
                handle.flush()
                os.fsync(handle.fileno())
            finally:
                handle.close()

        if total is not None and transferred != total:
            raise AcquisitionError(
                f"{url}: transfer ended after {transferred} bytes but the server "
                f"declared {total}"
            )

        _make_writable(destination)
        os.replace(tmp_path, destination)
        tmp_path = None
    except urllib.error.HTTPError as exc:
        raise AcquisitionError(f"{url}: HTTP {exc.code} {exc.reason}") from exc
    except urllib.error.URLError as exc:
        raise AcquisitionError(f"{url}: {exc.reason}") from exc
    finally:
        if tmp_path is not None and tmp_path.exists():
            tmp_path.unlink(missing_ok=True)


def lock_file_path(metadata_dir: Path, dataset_id: str) -> Path:
    """Where observed digests for an unpinned dataset are recorded."""
    return metadata_dir / f"{dataset_id}.checksums.json"


def acquire_file(
    remote: RemoteFile,
    raw_dir: Path,
    *,
    metadata_dir: Path,
    dataset_id: str,
    force: bool = False,
    progress: Callable[[int, int | None], None] | None = None,
) -> AcquiredFile:
    """Ensures *remote* is present in *raw_dir* and verified."""
    destination = raw_dir / remote.filename
    lock_path = lock_file_path(metadata_dir, dataset_id)
    recorded: dict[str, str] = {}
    if lock_path.exists():
        recorded = dict(read_json(lock_path).get("files", {}))

    # The expectation is the config's pin if there is one; failing that, a
    # digest this pipeline recorded on an earlier run. The second is weaker --
    # it only proves the file has not changed since we first saw it -- so the
    # two are never conflated in the report.
    expected = remote.sha256
    remembered = recorded.get(remote.filename)

    cached = False
    if destination.exists() and not force:
        observed = sha256_file(destination)
        if expected is not None and observed != expected:
            raise AcquisitionError(
                f"{destination} does not match the digest declared in the dataset "
                f"config.\n  expected {expected}\n  observed {observed}\n"
                "The raw file is never repaired in place; delete it and re-run to "
                "download again."
            )
        if expected is None and remembered is not None and observed != remembered:
            raise AcquisitionError(
                f"{destination} has changed since it was first acquired.\n"
                f"  recorded {remembered}\n  observed {observed}\n"
                "Raw datasets are immutable; investigate before continuing."
            )
        cached = True
    else:
        download_file(remote.url, destination, progress=progress)
        observed = sha256_file(destination)
        if expected is not None and observed != expected:
            # The bad file is removed: leaving it would let a later run with
            # `force=False` find it, re-check it, and fail identically forever,
            # while looking like a cached file.
            _make_writable(destination)
            destination.unlink(missing_ok=True)
            raise AcquisitionError(
                f"{remote.url} downloaded but did not match the declared digest.\n"
                f"  expected {expected}\n  observed {observed}"
            )

    _make_read_only(destination)

    if expected is None:
        recorded[remote.filename] = observed
        write_json(
            lock_path,
            {
                "dataset_id": dataset_id,
                "note": (
                    "Digests observed on first acquisition. These files were NOT "
                    "verified against a published checksum -- they were accepted on "
                    "trust and recorded here so that a later change is detected. "
                    "Copy a digest into the dataset config's 'sha256' to pin it."
                ),
                "files": recorded,
            },
        )

    return AcquiredFile(
        remote=remote,
        path=destination,
        sha256=observed,
        cached=cached,
        trusted_on_first_use=expected is None,
    )


def acquire_dataset(
    config: DatasetConfig,
    raw_dir: Path,
    *,
    metadata_dir: Path,
    force: bool = False,
    progress: Callable[[int, int | None], None] | None = None,
) -> list[AcquiredFile]:
    """Acquires every file a dataset declares."""
    return [
        acquire_file(
            remote,
            raw_dir,
            metadata_dir=metadata_dir,
            dataset_id=config.dataset_id,
            force=force,
            progress=progress,
        )
        for remote in config.files
    ]


def decompress(source: Path, staging_dir: Path, *, force: bool = False) -> Path:
    """Expands a gzip-compressed raw file into *staging_dir*.

    Returns the path of the usable file. A file that is not compressed is
    returned unchanged rather than copied: there is nothing to do, and copying a
    multi-gigabyte road network to rename it would be pure cost.
    """
    if source.suffix != ".gz":
        return source

    staging_dir.mkdir(parents=True, exist_ok=True)
    destination = staging_dir / source.name[: -len(".gz")]
    if destination.exists() and not force:
        return destination

    tmp_handle = tempfile.NamedTemporaryFile(
        "wb", dir=str(staging_dir), delete=False, suffix=".part"
    )
    tmp_path = Path(tmp_handle.name)
    try:
        with gzip.open(source, "rb") as compressed:
            shutil.copyfileobj(compressed, tmp_handle, length=1 << 20)
        tmp_handle.flush()
        os.fsync(tmp_handle.fileno())
        tmp_handle.close()
        os.replace(tmp_path, destination)
        tmp_path = None
    except (OSError, EOFError, gzip.BadGzipFile) as exc:
        raise AcquisitionError(f"{source}: cannot decompress ({exc})") from exc
    finally:
        if not tmp_handle.closed:
            tmp_handle.close()
        if tmp_path is not None and tmp_path.exists():
            tmp_path.unlink(missing_ok=True)

    return destination
