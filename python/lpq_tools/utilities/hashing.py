"""Content hashes.

Two different hashes are used, for two different jobs.

``sha256_file`` covers DOWNLOADS. The threat there includes a truncated
transfer, a mirror serving a different revision of a dataset, and -- in
principle -- a tampered file, so a cryptographic hash is the right tool and its
cost is irrelevant next to the network transfer it verifies.

``fnv1a64`` mirrors the checksum the C++ side computes over a converted graph's
arcs. It is here only so Python can read a checksum back and compare it; the
authoritative computation lives in ``graph_io.hpp`` and is never duplicated
over a real graph's payload.
"""

from __future__ import annotations

import hashlib
from pathlib import Path

#: Large enough that the hash is bound by disk throughput rather than by Python
#: call overhead, small enough not to matter for memory on a multi-gigabyte file.
_CHUNK_BYTES = 1 << 20

_FNV_OFFSET = 14695981039346656037
_FNV_PRIME = 1099511628211
_MASK64 = (1 << 64) - 1


def sha256_file(path: Path, chunk_bytes: int = _CHUNK_BYTES) -> str:
    """Returns the lowercase hexadecimal SHA-256 of a file's contents."""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(chunk_bytes)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def fnv1a64(data: bytes) -> int:
    """FNV-1a over *data*, matching ``lpq::detail::hash_bytes``.

    Intended for short byte strings only. Hashing a whole graph payload here
    would be both slow and a second implementation of a format that has
    deliberately got one.
    """
    h = _FNV_OFFSET
    for byte in data:
        h = ((h ^ byte) * _FNV_PRIME) & _MASK64
    return h
