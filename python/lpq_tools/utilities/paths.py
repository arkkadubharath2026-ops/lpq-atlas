"""Repository-relative paths.

Every path in the dataset pipeline is derived from the repository root rather
than from the current working directory. A pipeline that resolves
``datasets/raw`` relative to wherever it happened to be invoked will silently
create a second, empty dataset tree the first time someone runs it from a
subdirectory -- and then re-download several gigabytes into it.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

#: Files that together identify the repository root. All must be present, so a
#: directory that merely happens to contain a ``PRD.md`` is not mistaken for it.
_ROOT_MARKERS = ("PRD.md", "ESD.md", "CMakePresets.json")


def find_repo_root(start: Path | None = None) -> Path:
    """Walks upwards from *start* until it finds the repository root.

    Raises:
        FileNotFoundError: if no ancestor carries the marker files. Failing is
            the right outcome: guessing a root would put datasets somewhere
            other than where the rest of the project looks for them.
    """
    here = (start or Path(__file__)).resolve()
    for candidate in (here, *here.parents):
        if candidate.is_dir() and all((candidate / m).exists() for m in _ROOT_MARKERS):
            return candidate
    raise FileNotFoundError(
        f"no repository root above {here}: expected a directory containing "
        + ", ".join(_ROOT_MARKERS)
    )


@dataclass(frozen=True)
class RepoPaths:
    """The directories the dataset pipeline reads and writes.

    The layout is ESD.md section 3's. Holding it in one object means a change to
    the layout is a change in one place rather than a search for string
    literals.
    """

    root: Path

    @classmethod
    def discover(cls, start: Path | None = None) -> "RepoPaths":
        return cls(find_repo_root(start))

    # -- datasets -------------------------------------------------------------

    @property
    def datasets(self) -> Path:
        return self.root / "datasets"

    @property
    def raw(self) -> Path:
        """Downloaded files, exactly as published. Never modified in place."""
        return self.datasets / "raw"

    @property
    def processed(self) -> Path:
        """Converted graphs in the project's binary CSR format."""
        return self.datasets / "processed"

    @property
    def staging(self) -> Path:
        """Decompressed intermediates.

        Separate from ``processed`` because these are reproducible from ``raw``
        and can be deleted at any time, whereas a converted graph is referenced
        by checksum from experiment records.
        """
        return self.processed / "staging"

    @property
    def metadata(self) -> Path:
        return self.datasets / "metadata"

    # -- configuration and experiments ---------------------------------------

    @property
    def dataset_configs(self) -> Path:
        return self.root / "configs" / "datasets"

    @property
    def manifests(self) -> Path:
        return self.root / "experiments" / "manifests"

    # -- build output ---------------------------------------------------------

    def converter(self) -> Path:
        """Locates the ``lpq_convert`` executable.

        Searched rather than configured because the build directory differs
        between the presets and between platforms, and an absolute path in a
        config file would be wrong on every machine but one.
        """
        override = os.environ.get("LPQ_CONVERT")
        if override:
            path = Path(override)
            if not path.exists():
                raise FileNotFoundError(f"LPQ_CONVERT points at a missing file: {path}")
            return path

        names = ("lpq_convert", "lpq_convert.exe")
        roots = (
            self.root / "build" / "release" / "bin",
            self.root / "build" / "relwithdebinfo" / "bin",
            self.root / "build" / "debug" / "bin",
        )
        for directory in roots:
            for name in names:
                candidate = directory / name
                if candidate.exists():
                    return candidate
        raise FileNotFoundError(
            "lpq_convert was not found in any build directory. Build it with\n"
            "  cmake --build build/release --target lpq_convert\n"
            "or set LPQ_CONVERT to its path."
        )

    def ensure_directories(self) -> None:
        for path in (self.raw, self.processed, self.staging, self.metadata, self.manifests):
            path.mkdir(parents=True, exist_ok=True)
