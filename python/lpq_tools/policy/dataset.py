"""Builds the policy training table from ``lpq_benchmark`` result files.

One table row is one ``(dataset, algorithm, candidate)``: the *graph-level* label
``y = log(geometric mean over queries of t_candidate / t_radix)``.  The ratios are
PAIRED -- ``lpq_benchmark --interleave --compare`` measures every configuration of
a query back to back, so a query's candidate and radix timings share the same
machine state; the ratio is only ever formed inside one result file and one query.

Rules (CLAUDE.md "never fabricate"; nothing is silently dropped):

* A file with any record whose ``correctness.exact_match`` is not true is refused.
* A query contributes to a candidate's label only if the candidate AND the radix
  heap are both ``supported`` for it.  Every other case is counted per
  ``(dataset, algorithm, width)`` in :class:`KeyCounts`; the counts always add up
  to the number of candidate records seen.
* Dijkstra and A* are separate: the algorithm string is part of the key.
* Labels come from the measured rows only.
"""

from __future__ import annotations

import hashlib
import json
import math
import re
import zlib
from collections import Counter, defaultdict
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Iterable

import numpy as np

from lpq_tools.policy.profile import PolicyFormatError, WeightProfile, load_profile


class ResultFileError(ValueError):
    """A result file cannot be used as training data (and says why)."""


def sha256_file(path: str | Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


@dataclass(frozen=True)
class InputFile:
    path: str
    sha256: str
    experiment_id: str
    records: int


@dataclass
class KeyCounts:
    """Accounting for one ``(dataset, algorithm, width)``.

    ``candidate_records == paired + unsupported_candidate + unsupported_radix +
    no_radix`` always holds.
    """

    candidate_records: int = 0
    paired: int = 0
    unsupported_candidate: int = 0
    unsupported_radix: int = 0
    no_radix: int = 0

    def as_dict(self) -> dict[str, int]:
        return dict(self.__dict__)


@dataclass(frozen=True)
class CandidateRow:
    dataset: str
    algorithm: str
    width: int
    labels: tuple[str, ...]
    ring_sizes: tuple[int, ...]
    y: float  # log of the geometric mean paired ratio
    se: float | None  # seeded bootstrap standard error of y (over queries)
    n_queries: int  # distinct queries contributing
    n_pairs: int  # paired observations (replicates across files counted)
    files: tuple[str, ...]

    @property
    def ratio(self) -> float:
        return math.exp(self.y)


@dataclass
class PolicyTable:
    rows: list[CandidateRow]
    inputs: list[InputFile]
    counts: dict[tuple[str, str, int], KeyCounts]
    ignored: Counter  # (dataset, algorithm, label) of non-candidate, non-radix records
    shapes: dict[str, tuple[int, int]]  # dataset -> (graph_nodes, graph_edges) as recorded
    inferred_datasets: dict[str, str]  # dataset -> how its id was obtained (if not in the record)
    bootstrap_replicates: int
    seed: int

    def datasets(self, algorithm: str) -> list[str]:
        return sorted({r.dataset for r in self.rows if r.algorithm == algorithm})

    def algorithms(self) -> list[str]:
        return sorted({r.algorithm for r in self.rows})

    def by_dataset(self, algorithm: str) -> dict[str, dict[int, CandidateRow]]:
        out: dict[str, dict[int, CandidateRow]] = defaultdict(dict)
        for r in self.rows:
            if r.algorithm == algorithm:
                out[r.dataset][r.width] = r
        return dict(out)

    def count_report(self) -> list[dict[str, object]]:
        rep = []
        for (ds, alg, w), c in sorted(self.counts.items()):
            rep.append({"dataset": ds, "algorithm": alg, "width": w, **c.as_dict()})
        return rep


def _bootstrap_se(per_query: np.ndarray, seed: int, key: tuple[str, str, int], b: int) -> float | None:
    n = per_query.size
    if n < 2 or b < 2:
        return None
    ss = np.random.SeedSequence([seed & 0xFFFFFFFF, seed >> 32, zlib.crc32(key[0].encode()),
                                 zlib.crc32(key[1].encode()), key[2]])
    rng = np.random.default_rng(ss)
    idx = rng.integers(0, n, size=(b, n))
    means = per_query[idx].mean(axis=1)
    return float(means.std(ddof=1))


def _queue_kind(rec: dict) -> str:
    qc = rec.get("queue_config")
    if isinstance(qc, dict) and qc.get("queue"):
        return str(qc["queue"])
    return str(rec.get("pq", ""))


def _infer_dataset_from_experiment(experiment_id: str, known: Iterable[str]) -> str | None:
    """Matches ``-<dataset>-`` (or a trailing ``-<dataset>``) in the experiment id.

    Only a unique match among datasets that have a profile is accepted.
    """
    hits = []
    for ds in known:
        if re.search(rf"(^|[-_/]){re.escape(ds)}($|[-_/])", experiment_id):
            hits.append(ds)
    # a dataset id that is a prefix of a longer id also matches only at a boundary
    # above, but two overlapping ids (a, a_b) can both match: keep the longest.
    hits = [h for h in hits if not any(o != h and h in o for o in hits)]
    return hits[0] if len(hits) == 1 else None


def build_table(
    result_paths: Iterable[str | Path],
    *,
    seed: int,
    bootstrap: int = 1000,
    assume_dataset: dict[str, str] | None = None,
    known_datasets: Iterable[str] = (),
) -> PolicyTable:
    """Reads result files and builds the paired-ratio table.

    ``assume_dataset`` maps a result file (path or basename) to a dataset id for
    files whose records carry an empty ``dataset`` field (the engine currently
    writes ``""``).  Without a mapping the id is inferred from ``experiment_id``
    only if exactly one known dataset id appears in it; otherwise the file is
    refused.  Both routes are recorded in :attr:`PolicyTable.inferred_datasets`.
    """
    assume_dataset = assume_dataset or {}
    known = list(known_datasets)
    inputs: list[InputFile] = []
    counts: dict[tuple[str, str, int], KeyCounts] = defaultdict(KeyCounts)
    ignored: Counter = Counter()
    shapes: dict[str, tuple[int, int]] = {}
    inferred: dict[str, str] = {}
    # (dataset, algorithm, width) -> {query: [log ratios]} ; labels ; rings ; files
    acc: dict[tuple[str, str, int], dict[tuple[int, int], list[float]]] = defaultdict(
        lambda: defaultdict(list))
    labels: dict[tuple[str, str, int], set[str]] = defaultdict(set)
    rings: dict[tuple[str, str, int], set[int]] = defaultdict(set)
    kfiles: dict[tuple[str, str, int], set[str]] = defaultdict(set)

    paths = [Path(p) for p in result_paths]
    if not paths:
        raise ResultFileError("no result files given")
    seen_hashes: dict[str, str] = {}

    for path in paths:
        if not path.is_file():
            raise ResultFileError(f"result file not found: {path}")
        digest = sha256_file(path)
        if digest in seen_hashes:
            raise ResultFileError(
                f"{path} is byte-identical to {seen_hashes[digest]}: the same measurements "
                f"would be counted twice")
        seen_hashes[digest] = str(path)
        try:
            doc = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            raise ResultFileError(f"{path}: not valid JSON ({exc})") from exc
        if not isinstance(doc, dict) or doc.get("schema_version") != 1:
            raise ResultFileError(f"{path}: unsupported schema_version (need 1)")
        records = doc.get("records")
        if not isinstance(records, list) or not records:
            raise ResultFileError(f"{path}: no records")
        exp_id = str(doc.get("experiment_id", ""))
        inputs.append(InputFile(str(path), digest, exp_id, len(records)))

        bad = [r for r in records
               if r.get("correctness", {}).get("exact_match") is False
               or (r.get("supported", True) and r.get("correctness", {}).get("exact_match") is not True)]
        if bad:
            ex = bad[0]
            raise ResultFileError(
                f"{path}: {len(bad)} record(s) without exact_match=true (first: pq="
                f"{ex.get('pq')!r}, source={ex.get('source')}, target={ex.get('target')}); "
                f"a file with an inexact queue is not trustworthy training data")

        # --- dataset id resolution ------------------------------------------------
        file_dataset: str | None = None
        for key in (str(path), path.name):
            if key in assume_dataset:
                file_dataset = assume_dataset[key]
                how = f"--assume-dataset {key}"
                break
        else:
            how = ""
        if file_dataset is None and any(not r.get("dataset") for r in records):
            file_dataset = _infer_dataset_from_experiment(exp_id, known)
            how = f"inferred from experiment_id {exp_id!r}"
            if file_dataset is None:
                raise ResultFileError(
                    f"{path}: records carry an empty 'dataset' and the id cannot be inferred "
                    f"from experiment_id {exp_id!r}; pass --assume-dataset {path.name}=<dataset_id>")

        groups: dict[tuple[str, str, int, int], dict[str, dict]] = defaultdict(dict)
        for r in records:
            ds = r.get("dataset") or file_dataset
            if not r.get("dataset"):
                inferred[ds] = f"{path.name}: {how}"
            alg = str(r.get("algorithm", ""))
            if not alg:
                raise ResultFileError(f"{path}: record without algorithm")
            shape = (int(r.get("graph_nodes", 0)), int(r.get("graph_edges", 0)))
            if shapes.setdefault(ds, shape) != shape:
                raise ResultFileError(
                    f"{path}: dataset {ds!r} recorded with shape {shape}, earlier {shapes[ds]}")
            qkey = (ds, alg, int(r["source"]), int(r["target"]))
            label = str(r.get("pq", ""))
            if label in groups[qkey]:
                raise ResultFileError(
                    f"{path}: duplicate record for {label!r} on {ds}/{alg} query {qkey[2]}->{qkey[3]}")
            groups[qkey][label] = r

        for (ds, alg, src, dst), by_label in groups.items():
            radix_recs = [r for r in by_label.values() if _queue_kind(r) == "radix_heap"]
            if len(radix_recs) > 1:
                raise ResultFileError(
                    f"{path}: {len(radix_recs)} radix_heap records for one query on {ds}/{alg}")
            radix = radix_recs[0] if radix_recs else None
            for label, r in by_label.items():
                kind = _queue_kind(r)
                if kind == "radix_heap":
                    continue
                if kind != "pooled_bucket":
                    ignored[(ds, alg, label)] += 1
                    continue
                qc = r.get("queue_config") or {}
                width = int(qc.get("bucket_width", 0))
                if width < 1:
                    raise ResultFileError(f"{path}: pooled_bucket record {label!r} without a width")
                key = (ds, alg, width)
                c = counts[key]
                c.candidate_records += 1
                labels[key].add(label)
                if qc.get("ring_size"):
                    rings[key].add(int(qc["ring_size"]))
                kfiles[key].add(str(path))
                if radix is None:
                    c.no_radix += 1
                    continue
                if not r.get("supported", True):
                    c.unsupported_candidate += 1
                    continue
                if not radix.get("supported", True):
                    c.unsupported_radix += 1
                    continue
                tc = float(r["timing"]["median_ns"])
                tr = float(radix["timing"]["median_ns"])
                if not (tc > 0 and tr > 0 and math.isfinite(tc) and math.isfinite(tr)):
                    raise ResultFileError(
                        f"{path}: non-positive median_ns for {label!r}/radix on {ds} query {src}->{dst}")
                c.paired += 1
                acc[key][(src, dst)].append(math.log(tc / tr))

    rows: list[CandidateRow] = []
    for key in sorted(counts):
        c = counts[key]
        assert c.candidate_records == c.paired + c.unsupported_candidate + c.unsupported_radix + c.no_radix
        per_q = acc.get(key)
        if not per_q:
            continue  # nothing paired: visible in `counts`, no row to train or evaluate on
        per_query = np.array([float(np.mean(v)) for v in per_q.values()])
        rows.append(CandidateRow(
            dataset=key[0], algorithm=key[1], width=key[2],
            labels=tuple(sorted(labels[key])), ring_sizes=tuple(sorted(rings[key])),
            y=float(per_query.mean()),
            se=_bootstrap_se(per_query, seed, key, bootstrap),
            n_queries=int(per_query.size), n_pairs=c.paired, files=tuple(sorted(kfiles[key])),
        ))
    return PolicyTable(rows=rows, inputs=inputs, counts=dict(counts), ignored=ignored,
                       shapes=shapes, inferred_datasets=inferred,
                       bootstrap_replicates=bootstrap, seed=seed)


# ----------------------------------------------------------------------------------
# Graph catalog: profiles, metadata and topology groups
# ----------------------------------------------------------------------------------


@dataclass(frozen=True)
class GraphInfo:
    dataset: str
    profile: WeightProfile
    profile_path: str
    metadata_path: str
    base: str  # declared base topology (itself for a real graph)
    declared_derived: bool
    topology: str = ""  # merged topology group (see load_graphs)


def _read_metadata(metadata_dir: Path, ds: str) -> tuple[dict, Path]:
    p = metadata_dir / f"{ds}.json"
    if not p.is_file():
        raise PolicyFormatError(
            f"dataset metadata not found: {p} -- needed to decide which graphs share a base "
            f"topology (leave-one-topology-out); refusing to guess")
    try:
        return json.loads(p.read_text(encoding="utf-8")), p
    except json.JSONDecodeError as exc:
        raise PolicyFormatError(f"{p}: not valid JSON ({exc})") from exc


def declared_base(ds: str, metadata_dir: Path) -> str:
    """Base topology a dataset was derived from, following ``derived_from`` chains.

    A real graph is its own base.  ``derived_from`` is authoritative; when it is
    absent but ``source_path`` is a ``.lpqgraph`` of another dataset (as in
    ``synth_ratio1_ny``) that dataset is the base.  The metadata of ``ds`` itself
    must exist (fail closed); a named base whose own metadata is absent is its
    own base.
    """
    seen = [ds]
    cur = ds
    while True:
        if cur != ds and not (metadata_dir / f"{cur}.json").is_file():
            return cur
        meta, _ = _read_metadata(metadata_dir, cur)
        nxt = meta.get("derived_from") or (meta.get("weights") or {}).get("derived_from")
        if not nxt:
            sp = str(meta.get("source_path", ""))
            if sp.endswith(".lpqgraph") and Path(sp).stem != cur:
                nxt = Path(sp).stem
        if not nxt:
            return cur
        if nxt in seen:
            raise PolicyFormatError(f"derived_from cycle through {nxt!r} starting at {ds!r}")
        seen.append(nxt)
        cur = nxt


def load_graphs(
    datasets: Iterable[str],
    profiles_dir: str | Path,
    metadata_dir: str | Path,
    shapes: dict[str, tuple[int, int]] | None = None,
) -> dict[str, GraphInfo]:
    """Loads profile + metadata for each dataset and assigns topology groups.

    Checks that guard against a stale or mismatched profile: node/arc counts must
    agree with the metadata and with the shape recorded in the result files (whose
    ``graph_edges`` is the edge count, so either the arc count or the metadata
    edge count is accepted), and the profile checksum must equal the metadata's
    ``graph_checksum_fnv1a64`` when the latter exists.

    Topology groups are the union of (a) the declared base and (b) datasets with
    identical node and arc counts (conservative: it can only remove training data).
    """
    pdir, mdir = Path(profiles_dir), Path(metadata_dir)
    infos: dict[str, GraphInfo] = {}
    for ds in sorted(set(datasets)):
        prof_path = pdir / f"{ds}.lpqprofile"
        prof = load_profile(prof_path)
        meta, meta_path = _read_metadata(mdir, ds)
        for field_, got in (("node_count", prof.nodes), ("arc_count", prof.arcs)):
            if field_ in meta and int(meta[field_]) != got:
                raise PolicyFormatError(
                    f"{prof_path}: profile {field_.split('_')[0]}s={got} but {meta_path} says "
                    f"{field_}={meta[field_]} (stale profile?)")
        if shapes and ds in shapes and shapes[ds] != (0, 0):
            rec_nodes, rec_edges = shapes[ds]
            # result records say graph_edges: the EDGE count (half the arcs of an
            # undirected graph), so accept either the arc count or the metadata edge count
            ok_edges = {prof.arcs} | ({int(meta["edge_count"])} if "edge_count" in meta else set())
            if rec_nodes != prof.nodes or rec_edges not in ok_edges:
                raise PolicyFormatError(
                    f"{prof_path}: profile has nodes/arcs {(prof.nodes, prof.arcs)} but the result "
                    f"records for {ds} say nodes/edges {shapes[ds]} (stale profile or wrong "
                    f"dataset id)")
        cs = meta.get("graph_checksum_fnv1a64")
        if cs is not None and prof.checksum and int(cs) != prof.checksum:
            raise PolicyFormatError(
                f"{prof_path}: profile checksum {prof.checksum} != metadata checksum {cs} "
                f"({meta_path}); the profile belongs to a different graph file")
        base = declared_base(ds, mdir)
        infos[ds] = GraphInfo(ds, prof, str(prof_path), str(meta_path), base, base != ds)

    parent: dict[str, str] = {}

    def find(a: str) -> str:
        parent.setdefault(a, a)
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    def rank(a: str) -> tuple[bool, str]:
        # a real graph names its topology group in preference to a derived variant
        return (a.startswith("derived_"), a)

    def union(a: str, b: str) -> None:
        ra, rb = find(a), find(b)
        if ra != rb:
            keep, drop = sorted((ra, rb), key=rank)
            parent[drop] = keep

    by_shape: dict[tuple[int, int], list[str]] = defaultdict(list)
    for ds, gi in infos.items():
        union(ds, gi.base)
        by_shape[(gi.profile.nodes, gi.profile.arcs)].append(ds)
    for group in by_shape.values():
        for other in group[1:]:
            union(group[0], other)
    return {ds: replace(gi, topology=find(ds)) for ds, gi in infos.items()}
