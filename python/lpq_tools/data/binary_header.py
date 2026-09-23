"""Reads the header of a converted graph file.

Only the header. The arcs are deliberately not parsed here, and neither is the
checksum recomputed: ``graph_io.hpp`` owns the format, and a second full reader
in Python would drift from it. What Python needs is the ability to confirm that
the file on disk is the one the metadata record describes -- node count, arc
count, weight type, directedness and the stored checksum -- and that is entirely
in the first fifty bytes.

The header layout (format version 2), all little-endian:

    u64  magic       "LPQGRAPH"
    u32  version
    u32  weight type tag
    u32  undirected flag
    u64  node count
    u64  arc count
    u32  has node-id mapping
    u32  has coordinates
    u64  checksum (FNV-1a over the arc payload)
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

#: "LPQGRAPH" read as a little-endian 64-bit value, matching ``kGraphMagic``.
GRAPH_MAGIC = 0x485041524751504C

#: Must track ``kGraphFormatVersion``. A mismatch is reported rather than
#: tolerated: a header whose fields have moved would be read as plausible
#: nonsense.
GRAPH_FORMAT_VERSION = 2

_WEIGHT_TYPES = {1: "uint32", 2: "uint64", 3: "double"}

_HEADER = struct.Struct("<QIIIQQIIQ")


class BinaryGraphError(ValueError):
    """Raised when a converted graph file cannot be interpreted."""


@dataclass(frozen=True)
class BinaryGraphHeader:
    path: Path
    version: int
    weight_type: str
    directed: bool
    node_count: int
    arc_count: int
    has_mapping: bool
    has_coordinates: bool
    checksum: int

    @property
    def edge_count(self) -> int:
        """Semantic edges: halved for an undirected graph, matching ``num_edges``."""
        return self.arc_count if self.directed else self.arc_count // 2


def read_binary_graph_header(path: Path) -> BinaryGraphHeader:
    with path.open("rb") as handle:
        blob = handle.read(_HEADER.size)
    if len(blob) < _HEADER.size:
        raise BinaryGraphError(f"{path}: too short to hold a graph header")

    (
        magic,
        version,
        weight_tag,
        undirected,
        node_count,
        arc_count,
        has_mapping,
        has_coordinates,
        checksum,
    ) = _HEADER.unpack(blob)

    if magic != GRAPH_MAGIC:
        raise BinaryGraphError(f"{path}: not an LPQ graph file (bad magic number)")
    if version != GRAPH_FORMAT_VERSION:
        raise BinaryGraphError(
            f"{path}: graph format version {version}, but this tooling reads "
            f"version {GRAPH_FORMAT_VERSION}"
        )
    if weight_tag not in _WEIGHT_TYPES:
        raise BinaryGraphError(f"{path}: unknown weight type tag {weight_tag}")

    return BinaryGraphHeader(
        path=path,
        version=version,
        weight_type=_WEIGHT_TYPES[weight_tag],
        directed=undirected == 0,
        node_count=node_count,
        arc_count=arc_count,
        has_mapping=has_mapping != 0,
        has_coordinates=has_coordinates != 0,
        checksum=checksum,
    )
