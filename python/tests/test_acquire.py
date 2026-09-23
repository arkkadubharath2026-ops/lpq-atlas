"""Tests for acquisition and the immutability of raw datasets.

ESD.md section 26's rule is "never modify raw datasets in place". These tests
exercise the mechanisms that enforce it rather than trusting that no caller will
try, because the failure they prevent -- a raw file that has quietly diverged
from what was published -- is undetectable afterwards.

No test here touches the network: downloads are redirected to local files.
"""

from __future__ import annotations

import gzip
import os
import stat

import pytest

from lpq_tools.data import acquire
from lpq_tools.data.acquire import AcquisitionError, acquire_file, decompress
from lpq_tools.data.sources import RemoteFile
from lpq_tools.utilities.hashing import fnv1a64, sha256_file
from lpq_tools.utilities.jsonio import read_json


@pytest.fixture
def fake_server(monkeypatch):
    """Replaces the downloader with one that serves from a dictionary."""
    served: dict[str, bytes] = {}
    calls: list[str] = []

    def fake_download(url, destination, *, progress=None, timeout=60.0):
        calls.append(url)
        if url not in served:
            raise AcquisitionError(f"{url}: HTTP 404 Not Found")
        destination.parent.mkdir(parents=True, exist_ok=True)
        acquire._make_writable(destination)
        destination.write_bytes(served[url])

    monkeypatch.setattr(acquire, "download_file", fake_download)
    return served, calls


def make_dirs(tmp_path):
    raw = tmp_path / "raw"
    metadata = tmp_path / "metadata"
    raw.mkdir()
    metadata.mkdir()
    return raw, metadata


# -----------------------------------------------------------------------------
# Verification
# -----------------------------------------------------------------------------


def test_a_matching_digest_is_accepted(tmp_path, fake_server):
    served, _ = fake_server
    payload = b"1 2\n2 3\n"
    served["https://x.invalid/a.txt"] = payload

    import hashlib

    digest = hashlib.sha256(payload).hexdigest()
    raw, metadata = make_dirs(tmp_path)

    result = acquire_file(
        RemoteFile(url="https://x.invalid/a.txt", filename="a.txt", sha256=digest),
        raw,
        metadata_dir=metadata,
        dataset_id="toy",
    )
    assert result.sha256 == digest
    assert not result.trusted_on_first_use
    assert not result.cached


def test_a_mismatched_digest_is_rejected_and_the_bad_file_removed(tmp_path, fake_server):
    served, _ = fake_server
    served["https://x.invalid/a.txt"] = b"not what was expected"
    raw, metadata = make_dirs(tmp_path)

    with pytest.raises(AcquisitionError, match="did not match"):
        acquire_file(
            RemoteFile(url="https://x.invalid/a.txt", filename="a.txt", sha256="ab" * 32),
            raw,
            metadata_dir=metadata,
            dataset_id="toy",
        )
    # Left in place, a later run would find it, re-check it, and fail forever
    # while looking like a cached file.
    assert not (raw / "a.txt").exists()


def test_an_unpinned_file_is_recorded_rather_than_verified(tmp_path, fake_server):
    served, _ = fake_server
    served["https://x.invalid/a.txt"] = b"1 2\n"
    raw, metadata = make_dirs(tmp_path)

    result = acquire_file(
        RemoteFile(url="https://x.invalid/a.txt", filename="a.txt"),
        raw,
        metadata_dir=metadata,
        dataset_id="toy",
    )
    assert result.trusted_on_first_use

    lock = read_json(metadata / "toy.checksums.json")
    assert lock["files"]["a.txt"] == result.sha256
    # The distinction must be stated in the artifact, not just in the return
    # value, because the lock file outlives the run that wrote it.
    assert "trust" in lock["note"].lower()


def test_a_changed_unpinned_file_is_detected_on_the_next_run(tmp_path, fake_server):
    served, _ = fake_server
    served["https://x.invalid/a.txt"] = b"original\n"
    raw, metadata = make_dirs(tmp_path)
    remote = RemoteFile(url="https://x.invalid/a.txt", filename="a.txt")

    acquire_file(remote, raw, metadata_dir=metadata, dataset_id="toy")

    # Someone edits the raw file afterwards.
    os.chmod(raw / "a.txt", stat.S_IWUSR | stat.S_IRUSR)
    (raw / "a.txt").write_bytes(b"tampered\n")

    with pytest.raises(AcquisitionError, match="changed since it was first acquired"):
        acquire_file(remote, raw, metadata_dir=metadata, dataset_id="toy")


def test_a_present_file_with_the_wrong_pinned_digest_is_not_repaired(tmp_path, fake_server):
    served, _ = fake_server
    raw, metadata = make_dirs(tmp_path)
    (raw / "a.txt").write_bytes(b"wrong contents\n")

    with pytest.raises(AcquisitionError, match="never repaired in place"):
        acquire_file(
            RemoteFile(url="https://x.invalid/a.txt", filename="a.txt", sha256="cd" * 32),
            raw,
            metadata_dir=metadata,
            dataset_id="toy",
        )


# -----------------------------------------------------------------------------
# Caching and immutability
# -----------------------------------------------------------------------------


def test_a_verified_file_is_not_downloaded_again(tmp_path, fake_server):
    served, calls = fake_server
    served["https://x.invalid/a.txt"] = b"1 2\n"
    raw, metadata = make_dirs(tmp_path)
    remote = RemoteFile(url="https://x.invalid/a.txt", filename="a.txt")

    first = acquire_file(remote, raw, metadata_dir=metadata, dataset_id="toy")
    second = acquire_file(remote, raw, metadata_dir=metadata, dataset_id="toy")

    assert not first.cached
    assert second.cached
    assert len(calls) == 1


def test_force_downloads_again(tmp_path, fake_server):
    served, calls = fake_server
    served["https://x.invalid/a.txt"] = b"1 2\n"
    raw, metadata = make_dirs(tmp_path)
    remote = RemoteFile(url="https://x.invalid/a.txt", filename="a.txt")

    acquire_file(remote, raw, metadata_dir=metadata, dataset_id="toy")
    acquire_file(remote, raw, metadata_dir=metadata, dataset_id="toy", force=True)
    assert len(calls) == 2


@pytest.mark.skipif(os.name == "nt", reason="Windows ignores the read-only bit for the owner")
def test_an_acquired_file_is_made_read_only(tmp_path, fake_server):
    served, _ = fake_server
    served["https://x.invalid/a.txt"] = b"1 2\n"
    raw, metadata = make_dirs(tmp_path)

    result = acquire_file(
        RemoteFile(url="https://x.invalid/a.txt", filename="a.txt"),
        raw,
        metadata_dir=metadata,
        dataset_id="toy",
    )
    mode = stat.S_IMODE(result.path.stat().st_mode)
    assert not mode & stat.S_IWUSR


def test_a_missing_url_reports_rather_than_creating_an_empty_file(tmp_path, fake_server):
    _, _ = fake_server
    raw, metadata = make_dirs(tmp_path)
    with pytest.raises(AcquisitionError, match="404"):
        acquire_file(
            RemoteFile(url="https://x.invalid/missing.txt", filename="missing.txt"),
            raw,
            metadata_dir=metadata,
            dataset_id="toy",
        )
    assert not (raw / "missing.txt").exists()


# -----------------------------------------------------------------------------
# Decompression
# -----------------------------------------------------------------------------


def test_decompression_writes_to_staging_and_leaves_raw_untouched(tmp_path):
    raw = tmp_path / "raw"
    staging = tmp_path / "staging"
    raw.mkdir()

    source = raw / "a.txt.gz"
    with gzip.open(source, "wb") as handle:
        handle.write(b"1 2\n2 3\n")
    before = sha256_file(source)

    expanded = decompress(source, staging)
    assert expanded == staging / "a.txt"
    assert expanded.read_bytes() == b"1 2\n2 3\n"
    # The raw file is the record of what was published.
    assert sha256_file(source) == before
    assert source.exists()


def test_decompression_is_cached(tmp_path):
    raw = tmp_path / "raw"
    staging = tmp_path / "staging"
    raw.mkdir()
    source = raw / "a.txt.gz"
    with gzip.open(source, "wb") as handle:
        handle.write(b"payload\n")

    first = decompress(source, staging)
    first.write_bytes(b"marker\n")
    second = decompress(source, staging)
    assert second.read_bytes() == b"marker\n"

    forced = decompress(source, staging, force=True)
    assert forced.read_bytes() == b"payload\n"


def test_an_uncompressed_file_is_returned_unchanged(tmp_path):
    raw = tmp_path / "raw"
    staging = tmp_path / "staging"
    raw.mkdir()
    source = raw / "a.txt"
    source.write_bytes(b"1 2\n")
    # Not copied: copying a multi-gigabyte road network to rename it is cost
    # with no benefit.
    assert decompress(source, staging) == source


def test_a_corrupt_archive_is_reported(tmp_path):
    raw = tmp_path / "raw"
    staging = tmp_path / "staging"
    raw.mkdir()
    source = raw / "a.txt.gz"
    source.write_bytes(b"this is not gzip data at all")

    with pytest.raises(AcquisitionError, match="cannot decompress"):
        decompress(source, staging)
    assert not (staging / "a.txt").exists()


# -----------------------------------------------------------------------------
# Hashing helpers
# -----------------------------------------------------------------------------


def test_fnv1a64_matches_the_cpp_offset_basis():
    # The empty input must hash to the offset basis, which is what pins this
    # implementation to lpq::detail::kFnvOffset.
    assert fnv1a64(b"") == 14695981039346656037


def test_fnv1a64_is_order_sensitive():
    assert fnv1a64(b"ab") != fnv1a64(b"ba")


def test_sha256_file_matches_hashlib(tmp_path):
    import hashlib

    path = tmp_path / "blob"
    payload = os.urandom(3 * (1 << 20) + 17)  # spans several read chunks
    path.write_bytes(payload)
    assert sha256_file(path) == hashlib.sha256(payload).hexdigest()
