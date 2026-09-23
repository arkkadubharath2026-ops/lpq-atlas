"""JSON that reads back byte-identically.

Every JSON file this project writes is an artifact that may be committed,
diffed, or checksummed. Dictionary iteration order and float formatting are both
sources of spurious differences, so keys are sorted and the file always ends in
exactly one newline.
"""

from __future__ import annotations

import json
import os
import tempfile
from pathlib import Path
from typing import Any


def read_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def write_json(path: Path, payload: Any) -> None:
    """Writes *payload* atomically.

    Written to a temporary file in the same directory and then renamed, so an
    interrupted run leaves either the old file or the new one -- never a
    half-written record that a later step would read as truth.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(payload, indent=2, sort_keys=True, ensure_ascii=False) + "\n"

    handle = tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=str(path.parent), delete=False, newline="\n"
    )
    try:
        handle.write(text)
        handle.flush()
        os.fsync(handle.fileno())
    finally:
        handle.close()
    os.replace(handle.name, path)
