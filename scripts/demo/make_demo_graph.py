#!/usr/bin/env python3
"""Generates the LPQ Atlas demonstration road graph (dataset id ``demo_road``).

A small, SYNTHETIC, road-like network that is fully determined by the constants
below. It exists so that a fresh clone can build, test, run and benchmark LPQ
Atlas with no network access and no external data. It is not a real city and
nothing measured on it should be described as a result on real road data; the
real networks (DIMACS NY, OSM Monaco, ...) are acquired by
``scripts/data/prepare_datasets.py``.

    python scripts/demo/make_demo_graph.py            # write datasets/demo/*.gz
    python scripts/demo/make_demo_graph.py --check    # verify the committed files
    python scripts/demo/make_demo_graph.py --stats    # describe the graph

Output is DIMACS ``.gr`` (arcs, integer travel time in deciseconds) and ``.co``
(longitude/latitude in micro-degrees), gzip-compressed, which is exactly what
``lpq_convert --format dimacs --coordinates`` reads for the real DIMACS graphs.

Construction (all integer or IEEE-basic-operation arithmetic, so the bytes are
identical on every platform and Python version; only the standard library is
used, there is no dependency on numpy's generator streams):

  * a W x H lattice of intersections, ~100 m apart, each position jittered;
  * a random spanning tree of the lattice (so the graph is always connected)
    plus each remaining street with a class-dependent probability, giving the
    sparse degree (~2.7 arcs per node) of a real road network;
  * three road classes -- local 30 km/h, arterial (every 10th row/column)
    60 km/h, highway (every 35th row/column, with long interchange-to-
    interchange links) 110 km/h;
  * weight = ceil(length_m * 36 / km/h) * congestion, in deciseconds, where the
    congestion factor has a heavy right tail. Short local blocks give many small
    weights, highways and congestion give the long tail: a road-like skew;
  * every street is two-way, so the graph is strongly connected and every
    source-target pair is reachable;
  * node ids are shuffled, so the in-memory order does not mirror the geometry.

Admissibility of the geometric A* heuristic: the engine derives the largest safe
scale from the graph itself (``euclidean_admissible_scale`` /
``haversine_admissible_scale`` take min over arcs of weight / geometric length).
Every weight here is at least length * 36 / 110 deciseconds-per-metre by
construction (110 km/h is the fastest class and the congestion factor is >= 1),
so the derived scale is close to that bound and the heuristic is informative
rather than degenerate. The generator does not choose the scale; it only
guarantees that none of the arcs forces it to be useless.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import math
import sys
from pathlib import Path

DATASET_ID = "demo_road"

#: Fixed. Changing any constant below changes the graph and invalidates the
#: sha256 digests pinned in configs/datasets/demo_road.yaml.
SEED = 20260930
COLS = 140
ROWS = 140  # 19,600 nodes: inside the 20k "teaching" scale mode of Atlas

#: Centre of the synthetic city, micro-degrees. Open ocean near the equator,
#: deliberately not a real place: no basemap tile will pretend to match it.
ORIGIN_LON_UDEG = -30_000_000
ORIGIN_LAT_UDEG = 0

#: Lattice pitch ~100 m. At the equator one micro-degree is ~0.1112 m.
PITCH_UDEG = 900
JITTER_UDEG = 280  # +/- per axis, < pitch / 2 so the lattice never folds

#: Metres per micro-degree on a sphere of the engine's Earth radius
#: (6371008.8 m): pi / 180 * 6371008.8 / 1e6.
METRES_PER_UDEG = 0.11119492664455873

ARTERIAL_EVERY = 10
HIGHWAY_EVERY = 35
P_LOCAL = 0.34  # chance of keeping a non-tree local street
P_DIAGONAL = 0.04
KMH = {"local": 30, "arterial": 60, "highway": 110}

_MASK = (1 << 64) - 1


class SplitMix64:
    """Tiny integer PRNG: exact, portable, and independent of any library."""

    def __init__(self, seed: int) -> None:
        self.state = seed & _MASK

    def next(self) -> int:
        self.state = (self.state + 0x9E3779B97F4A7C15) & _MASK
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & _MASK
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & _MASK
        return z ^ (z >> 31)

    def below(self, n: int) -> int:
        return self.next() % n

    def unit(self) -> float:
        return (self.next() >> 11) / float(1 << 53)

    def shuffle(self, items: list) -> None:
        for i in range(len(items) - 1, 0, -1):
            j = self.below(i + 1)
            items[i], items[j] = items[j], items[i]


def _road_class(a_rc: tuple[int, int], b_rc: tuple[int, int]) -> str:
    (r1, c1), (r2, c2) = a_rc, b_rc
    if r1 == r2 and r1 % HIGHWAY_EVERY == 0:
        return "highway"
    if c1 == c2 and c1 % HIGHWAY_EVERY == 0:
        return "highway"
    if r1 == r2 and r1 % ARTERIAL_EVERY == 0:
        return "arterial"
    if c1 == c2 and c1 % ARTERIAL_EVERY == 0:
        return "arterial"
    return "local"


class _UnionFind:
    def __init__(self, n: int) -> None:
        self.parent = list(range(n))

    def find(self, x: int) -> int:
        while self.parent[x] != x:
            self.parent[x] = self.parent[self.parent[x]]
            x = self.parent[x]
        return x

    def union(self, a: int, b: int) -> bool:
        ra, rb = self.find(a), self.find(b)
        if ra == rb:
            return False
        self.parent[ra] = rb
        return True


def generate() -> tuple[list[tuple[int, int, int]], list[tuple[int, int]]]:
    """Returns (arcs, coords): arcs are 1-based (u, v, weight); coords are
    (lon_udeg, lat_udeg) indexed by 0-based node id."""
    rng = SplitMix64(SEED)
    n = COLS * ROWS

    def idx(r: int, c: int) -> int:
        return r * COLS + c

    # Positions: lattice + jitter. Integer arithmetic only.
    pos: list[tuple[int, int]] = []
    for r in range(ROWS):
        for c in range(COLS):
            jx = rng.below(2 * JITTER_UDEG + 1) - JITTER_UDEG
            jy = rng.below(2 * JITTER_UDEG + 1) - JITTER_UDEG
            pos.append((ORIGIN_LON_UDEG + c * PITCH_UDEG + jx, ORIGIN_LAT_UDEG + r * PITCH_UDEG + jy))

    # Candidate streets: lattice neighbours, a few diagonals, and highway links
    # that skip 5 nodes (an interchange-to-interchange segment).
    cand: list[tuple[int, int, str]] = []
    for r in range(ROWS):
        for c in range(COLS):
            if c + 1 < COLS:
                cand.append((idx(r, c), idx(r, c + 1), _road_class((r, c), (r, c + 1))))
            if r + 1 < ROWS:
                cand.append((idx(r, c), idx(r + 1, c), _road_class((r, c), (r + 1, c))))
            if r + 1 < ROWS and c + 1 < COLS and rng.unit() < P_DIAGONAL:
                cand.append((idx(r, c), idx(r + 1, c + 1), "local"))
    skip = 5
    for r in range(0, ROWS, HIGHWAY_EVERY):
        for c in range(0, COLS - skip, skip):
            cand.append((idx(r, c), idx(r, c + skip), "highway"))
    for c in range(0, COLS, HIGHWAY_EVERY):
        for r in range(0, ROWS - skip, skip):
            cand.append((idx(r, c), idx(r + skip, c), "highway"))

    # Spanning tree by randomised Kruskal => connectivity is guaranteed.
    order = list(range(len(cand)))
    rng.shuffle(order)
    uf = _UnionFind(n)
    keep = [False] * len(cand)
    for i in order:
        if uf.union(cand[i][0], cand[i][1]):
            keep[i] = True
    # Non-tree streets: arterials/highways always kept (they carry the speed
    # contrast); locals with probability P_LOCAL.
    for i in range(len(cand)):
        if not keep[i]:
            keep[i] = cand[i][2] != "local" or rng.unit() < P_LOCAL

    # Shuffle node ids so memory order does not equal geometry order.
    perm = list(range(n))
    rng.shuffle(perm)  # perm[old] = new (0-based)

    arcs: list[tuple[int, int, int]] = []
    for i, (a, b, cls) in enumerate(cand):
        if not keep[i]:
            continue
        dx = (pos[b][0] - pos[a][0]) * METRES_PER_UDEG
        dy = (pos[b][1] - pos[a][1]) * METRES_PER_UDEG
        length_m = math.sqrt(dx * dx + dy * dy)
        # Heavy-tailed congestion: cube of a uniform draw, up to +150 %.
        u = rng.unit()
        congestion = 1.0 + 1.5 * u * u * u
        w = max(1, math.ceil(length_m * 36.0 / KMH[cls] * congestion))
        pa, pb = perm[a] + 1, perm[b] + 1
        arcs.append((pa, pb, w))
        arcs.append((pb, pa, w))
    arcs.sort()

    coords = [(0, 0)] * n
    for old, p in enumerate(pos):
        coords[perm[old]] = p
    return arcs, coords


def render(arcs: list[tuple[int, int, int]], coords: list[tuple[int, int]]) -> tuple[bytes, bytes]:
    """The exact bytes of the .gr and .co files (LF newlines on every OS)."""
    gr = [
        f"c LPQ Atlas synthetic demonstration road graph '{DATASET_ID}' (seed {SEED})",
        "c generated by scripts/demo/make_demo_graph.py; weights are travel time in deciseconds",
        f"p sp {len(coords)} {len(arcs)}",
    ]
    gr += [f"a {u} {v} {w}" for u, v, w in arcs]
    co = [
        f"c LPQ Atlas synthetic demonstration road graph '{DATASET_ID}': lon lat in micro-degrees",
        f"p aux sp co {len(coords)}",
    ]
    co += [f"v {i + 1} {x} {y}" for i, (x, y) in enumerate(coords)]
    return ("\n".join(gr) + "\n").encode("ascii"), ("\n".join(co) + "\n").encode("ascii")


def gzip_bytes(payload: bytes) -> bytes:
    # mtime=0 and no embedded filename: the archive bytes carry no timestamp.
    return gzip.compress(payload, compresslevel=9, mtime=0)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def stats(arcs: list[tuple[int, int, int]], coords: list[tuple[int, int]]) -> str:
    ws = sorted(w for _, _, w in arcs)
    n, m = len(coords), len(arcs)
    q = lambda p: ws[min(len(ws) - 1, int(p * len(ws)))]  # noqa: E731
    return (
        f"nodes {n:,}  arcs {m:,}  mean out-degree {m / n:.2f}\n"
        f"weight (deciseconds): min {ws[0]}  p50 {q(0.5)}  p90 {q(0.9)}  "
        f"p99 {q(0.99)}  max {ws[-1]}  mean {sum(ws) / len(ws):.1f}"
    )


def main(argv: list[str] | None = None) -> int:
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out-dir", type=Path, default=root / "datasets" / "demo")
    parser.add_argument("--check", action="store_true",
                        help="regenerate in memory and compare with the files in --out-dir")
    parser.add_argument("--stats", action="store_true", help="print graph statistics")
    args = parser.parse_args(argv)

    arcs, coords = generate()
    gr, co = render(arcs, coords)
    if args.stats:
        print(stats(arcs, coords))

    targets = {f"{DATASET_ID}.gr.gz": gr, f"{DATASET_ID}.co.gz": co}
    if args.check:
        bad = 0
        for name, payload in targets.items():
            path = args.out_dir / name
            if not path.exists():
                print(f"MISSING  {path}")
                bad += 1
                continue
            # Compare the decompressed text: gzip bytes can differ between zlib
            # builds without the graph differing at all.
            same = gzip.decompress(path.read_bytes()) == payload
            print(f"{'ok      ' if same else 'DIFFERS '} {path}")
            bad += 0 if same else 1
        return 1 if bad else 0

    args.out_dir.mkdir(parents=True, exist_ok=True)
    for name, payload in targets.items():
        path = args.out_dir / name
        data = gzip_bytes(payload)
        path.write_bytes(data)
        print(f"wrote {path}  sha256 {sha256(data)}  ({len(data):,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
