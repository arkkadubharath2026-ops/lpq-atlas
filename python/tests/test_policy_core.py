"""Policy tooling, part 1: the C++ mirror (profile/features/ring/choose) and the table."""

from __future__ import annotations

import json
import math
import re
from pathlib import Path

import numpy as np
import pytest
from lpq_tools.policy import profile as P
from lpq_tools.policy.dataset import (
    ResultFileError,
    build_table,
    declared_base,
    load_graphs,
)
from lpq_tools.policy.profile import PolicyFormatError
from test_policy_support import (
    make_profile,
    record,
    synth_records,
    write_graph_files,
    write_results,
)

REPO = Path(__file__).resolve().parents[2]
HEADER = REPO / "cpp" / "include" / "lpq" / "lpq" / "queue_policy.hpp"


# --- profile / features ----------------------------------------------------------


def test_feature_names_match_the_cpp_header() -> None:
    if not HEADER.is_file():
        pytest.skip("queue_policy.hpp not present")
    text = HEADER.read_text()
    m = re.search(r"kPolicyFeatureNames\s*=\s*\{(.*?)\};", text, re.S)
    assert m, "kPolicyFeatureNames not found in the header"
    cpp_names = tuple(re.findall(r'"([^"]+)"', m.group(1)))
    assert cpp_names == P.FEATURE_NAMES
    count = re.search(r"kPolicyFeatureCount\s*=\s*(\d+)", text)
    assert count and int(count.group(1)) == len(P.FEATURE_NAMES)


def test_max_policy_ring_matches_the_cpp_header() -> None:
    if not HEADER.is_file():
        pytest.skip("queue_policy.hpp not present")
    m = re.search(r"kMaxPolicyRing\s*=\s*std::size_t\{1\}\s*<<\s*(\d+)", HEADER.read_text())
    assert m and (1 << int(m.group(1))) == P.MAX_POLICY_RING


def test_features_follow_the_cpp_formulas() -> None:
    p = make_profile(nodes=264346, arcs=733846, max_w=36946, mean_w=1293.3, p50_ratio=951 / 36946,
                     p99_ratio=5462 / 36946, cv=1130.0 / 1293.3)
    x = P.features(p)
    assert x[0] == math.log2(36946 + 1.0)
    assert x[1] == math.log2(1293.3 + 1.0)
    assert x[2] == p.p50 / 36946
    assert x[3] == p.p99 / 36946
    assert x[4] == p.stddev_weight / 1293.3
    assert x[5] == math.log2(264346)
    assert x[6] == p.mean_degree
    assert len(x) == len(P.FEATURE_NAMES)


def test_features_clamp_degenerate_profiles_like_cpp() -> None:
    z = P.WeightProfile(nodes=0, arcs=0, min_weight=0, max_weight=0, mean_weight=0,
                        stddev_weight=0, p50=0, p90=0, p99=0, mean_degree=0)
    x = P.features(z)
    assert x[0] == math.log2(2.0)  # max clamps to 1
    assert x[1] == math.log2(1e-9 + 1.0)  # mean clamps to 1e-9
    assert x[4] == 0.0
    assert x[5] == 0.0  # nodes clamp to 1


def test_profile_text_round_trip_and_strictness() -> None:
    p = make_profile(1000, 3000, 999.0, 12.345678901234567, 0.25, checksum=15161404696693572585)
    assert P.parse_profile_text(P.format_profile(p)) == p
    with pytest.raises(PolicyFormatError, match="unknown profile key"):
        P.parse_profile_text(P.format_profile(p) + "bogus 1\n")
    with pytest.raises(PolicyFormatError, match="version"):
        P.parse_profile_text(P.format_profile(p).replace("lpqprofile 1", "lpqprofile 2"))


def test_real_profile_parses_if_present() -> None:
    f = REPO / "datasets" / "processed" / "dimacs_ny.lpqprofile"
    if not f.is_file():
        pytest.skip("dimacs_ny.lpqprofile not present")
    p = P.load_profile(f)
    assert (p.nodes, p.arcs, p.max_weight) == (264346, 733846, 36946.0)


def test_missing_profile_error_names_the_file(tmp_path: Path) -> None:
    with pytest.raises(PolicyFormatError, match="nope.lpqprofile"):
        P.load_profile(tmp_path / "nope.lpqprofile")


# --- ring and choose -------------------------------------------------------------


@pytest.mark.parametrize(
    ("step", "width", "ring"),
    [(36946, 1, 65536), (36946, 8, 8192), (36946, 37, 1024), (3, 1, 8), (0, 1, 4),
     (1, 1, 4), (2, 2, 4), (36946, 36946, 4), ((1 << 18) - 4, 1, 1 << 18), ((1 << 18) - 3, 1, 1 << 18), ((1 << 18) - 2, 1, 1 << 19)],
)
def test_pooled_ring_for_is_bit_ceil_of_need(step: int, width: int, ring: int) -> None:
    # need = step / width + 3 (integer division), ring = std::bit_ceil(need)
    assert P.pooled_ring_for(step, width) == ring


def test_max_step_doubles_only_for_the_exact_name_astar() -> None:
    p = make_profile(10, 30, 100.0, 5.0, 0.2)
    assert P.policy_max_step(p, "dijkstra") == 100
    assert P.policy_max_step(p, "astar") == 200
    assert P.policy_max_step(p, "astar_haversine") == 100  # exactly what the C++ does


def _cand(width: int, z: float) -> P.Candidate:
    return P.Candidate(width, z, (0.0,) * 7)


def test_choose_margin_is_strict_and_radix_is_the_default() -> None:
    prof = make_profile(1000, 3000, 100.0, 5.0, 0.2)
    pol = P.QueuePolicy(margin=0.1, candidates=[_cand(1, math.log(0.5))])
    assert pol.choose(prof).width == 1
    pol = P.QueuePolicy(margin=0.1, candidates=[_cand(1, math.log(0.95))])
    assert pol.choose(prof).is_radix  # 0.95 is not below 1 - 0.1
    # exactly on the boundary (ratio 1.0, margin 0): strict '<' keeps radix
    assert P.choose_from_log_ratios([(1, 0.0)], 100, 0.0).is_radix
    assert not P.choose_from_log_ratios([(1, -1e-9)], 100, 0.0).is_radix
    assert P.QueuePolicy().choose(prof).is_radix  # no candidates: radix


def test_choose_picks_lowest_ratio_first_wins_ties_and_skips_big_rings() -> None:
    prof = make_profile(1000, 3000, 100.0, 5.0, 0.2)
    pol = P.QueuePolicy(margin=0.0, candidates=[_cand(1, math.log(0.7)), _cand(2, math.log(0.4)),
                                                _cand(4, math.log(0.4))])
    ch = pol.choose(prof)
    assert (ch.width, ch.ring) == (2, P.pooled_ring_for(100, 2))
    assert ch.predicted_ratio == pytest.approx(0.4)
    # width 1 would need a ring of 2^19 when the step is huge: skipped despite the best ratio
    big = make_profile(1000, 3000, float(1 << 18), 5.0, 0.2)
    pol = P.QueuePolicy(margin=0.0, candidates=[_cand(1, math.log(0.1)), _cand(8, math.log(0.8))])
    assert P.pooled_ring_for(1 << 18, 1) > P.MAX_POLICY_RING
    assert pol.choose(big).width == 8
    # A* doubles the step and can therefore make a width infeasible
    p2 = make_profile(1000, 3000, float((1 << 17) + 10), 5.0, 0.2)
    pol = P.QueuePolicy(margin=0.0, candidates=[_cand(1, math.log(0.1)), _cand(8, math.log(0.8))])
    assert pol.choose(p2, "dijkstra").width == 1
    assert pol.choose(p2, "astar").width == 8


def test_choose_overflowing_exp_is_infinite_not_an_error() -> None:
    prof = make_profile(1000, 3000, 100.0, 5.0, 0.2)
    assert P.QueuePolicy(margin=0.0, candidates=[_cand(1, 1e4)]).choose(prof).is_radix


def test_policy_text_parse_is_as_strict_as_the_cpp_loader() -> None:
    good = ("lpqpolicy 1\nmargin 0.03\ntrained_on a b c\nfeatures " + " ".join(P.FEATURE_NAMES)
            + "\ncandidate pooled w=2 intercept -0.1 coef 1 2 3 4 5 6 7\n")
    pol = P.parse_policy_text(good)
    assert pol.margin == 0.03 and pol.candidates[0].width == 2
    assert pol.candidates[0].coef == (1, 2, 3, 4, 5, 6, 7)
    with pytest.raises(PolicyFormatError, match="feature list"):
        P.parse_policy_text(good.replace("cv_w", "cvw"))
    with pytest.raises(PolicyFormatError, match="unknown queue policy key"):
        P.parse_policy_text(good + "extra 1\n")
    with pytest.raises(PolicyFormatError, match="too few"):
        P.parse_policy_text(good.replace("coef 1 2 3 4 5 6 7", "coef 1 2 3"))
    with pytest.raises(PolicyFormatError, match="missing"):
        P.parse_policy_text("lpqpolicy 1\nmargin 0.1\n")


# --- dataset table ---------------------------------------------------------------


def _tiny(tmp_path: Path, name: str = "a.json", ds: str = "g1", exp: str = "e") -> Path:
    recs = []
    # query 0: radix 100, w1 50 (ratio .5), w8 200 (ratio 2);  query 1: radix 100, w1 200, w8 50
    for q, (t1, t8) in enumerate([(50.0, 200.0), (200.0, 50.0)]):
        recs += [
            record(ds, "dijkstra", "radix_heap", "radix_heap", 0, q, 9, 100.0),
            record(ds, "dijkstra", "pooled_w1", "pooled_bucket", 1, q, 9, t1, ring=64),
            record(ds, "dijkstra", "pooled_w8", "pooled_bucket", 8, q, 9, t8, ring=16),
            record(ds, "dijkstra", "binary_heap", "binary_heap", 0, q, 9, 400.0),
        ]
    return write_results(tmp_path / name, recs, exp)


def test_paired_ratio_label_and_counts(tmp_path: Path) -> None:
    t = build_table([_tiny(tmp_path)], seed=1, bootstrap=200)
    rows = {r.width: r for r in t.rows}
    # geometric mean of {0.5, 2.0} is exactly 1 -> y == 0 for both widths
    assert rows[1].y == pytest.approx(0.0, abs=1e-12)
    assert rows[8].y == pytest.approx(0.0, abs=1e-12)
    assert rows[1].n_queries == 2 and rows[1].n_pairs == 2
    assert rows[1].ring_sizes == (64,) and rows[8].ring_sizes == (16,)
    assert t.ignored[("g1", "dijkstra", "binary_heap")] == 2  # visible, not silent
    c = t.counts[("g1", "dijkstra", 1)]
    assert (c.candidate_records, c.paired) == (2, 2)
    assert rows[1].se is not None and rows[1].se > 0  # ratios differ, so the SE is positive


def test_label_is_a_geometric_not_arithmetic_mean(tmp_path: Path) -> None:
    recs = []
    for q, tc in enumerate([50.0, 400.0]):  # ratios 0.5 and 4.0 -> geo mean 2^(1/2)... = sqrt(2)
        recs += [record("g", "dijkstra", "radix_heap", "radix_heap", 0, q, 1, 100.0),
                 record("g", "dijkstra", "pooled_w1", "pooled_bucket", 1, q, 1, tc)]
    t = build_table([write_results(tmp_path / "r.json", recs)], seed=1)
    assert t.rows[0].ratio == pytest.approx(math.sqrt(0.5 * 4.0))


def test_inexact_file_is_refused(tmp_path: Path) -> None:
    recs = [record("g", "dijkstra", "radix_heap", "radix_heap", 0, 0, 1, 100.0),
            record("g", "dijkstra", "pooled_w1", "pooled_bucket", 1, 0, 1, 50.0, exact=False)]
    with pytest.raises(ResultFileError, match="exact_match"):
        build_table([write_results(tmp_path / "r.json", recs)], seed=1)
    recs[1] = record("g", "dijkstra", "pooled_w1", "pooled_bucket", 1, 0, 1, 50.0, exact=None)
    with pytest.raises(ResultFileError, match="exact_match"):
        build_table([write_results(tmp_path / "r2.json", recs)], seed=1)


def test_unsupported_and_missing_radix_are_counted_not_dropped(tmp_path: Path) -> None:
    recs = [
        # q0: fine
        record("g", "dijkstra", "radix_heap", "radix_heap", 0, 0, 1, 100.0),
        record("g", "dijkstra", "pooled_w1", "pooled_bucket", 1, 0, 1, 50.0),
        # q1: candidate unsupported
        record("g", "dijkstra", "radix_heap", "radix_heap", 0, 1, 1, 100.0),
        record("g", "dijkstra", "pooled_w1", "pooled_bucket", 1, 1, 1, 50.0, supported=False),
        # q2: radix unsupported
        record("g", "dijkstra", "radix_heap", "radix_heap", 0, 2, 1, 100.0, supported=False),
        record("g", "dijkstra", "pooled_w1", "pooled_bucket", 1, 2, 1, 50.0),
        # q3: no radix record at all
        record("g", "dijkstra", "pooled_w1", "pooled_bucket", 1, 3, 1, 50.0),
    ]
    t = build_table([write_results(tmp_path / "r.json", recs)], seed=1)
    c = t.counts[("g", "dijkstra", 1)]
    assert c.as_dict() == {"candidate_records": 4, "paired": 1, "unsupported_candidate": 1,
                           "unsupported_radix": 1, "no_radix": 1}
    assert t.rows[0].n_queries == 1 and t.rows[0].se is None  # one query: no SE claimed


def test_dijkstra_and_astar_stay_separate(tmp_path: Path) -> None:
    recs = []
    for alg, tc in (("dijkstra", 50.0), ("astar", 150.0)):
        recs += [record("g", alg, "radix_heap", "radix_heap", 0, 0, 1, 100.0),
                 record("g", alg, "pooled_w1", "pooled_bucket", 1, 0, 1, tc)]
    t = build_table([write_results(tmp_path / "r.json", recs)], seed=1)
    assert t.algorithms() == ["astar", "dijkstra"]
    by = {r.algorithm: r.ratio for r in t.rows}
    assert by["dijkstra"] == pytest.approx(0.5) and by["astar"] == pytest.approx(1.5)


def test_bootstrap_se_is_seeded(tmp_path: Path) -> None:
    rng = np.random.default_rng(3)
    f = write_results(tmp_path / "r.json", synth_records("g", {1: -0.3}, 25, rng, noise=0.3))
    a = build_table([f], seed=11, bootstrap=300).rows[0].se
    b = build_table([f], seed=11, bootstrap=300).rows[0].se
    c = build_table([f], seed=12, bootstrap=300).rows[0].se
    assert a == b and a != c


def test_replicates_across_files_are_averaged_per_query(tmp_path: Path) -> None:
    f1 = _tiny(tmp_path, "a.json")
    recs = json.loads(f1.read_text())["records"]
    for r in recs:  # a second run of the same queries, 2x slower candidate only
        if r["pq"] == "pooled_w1":
            r["timing"]["median_ns"] *= 2
    f2 = write_results(tmp_path / "b.json", recs, "e2")
    t = build_table([f1, f2], seed=1, bootstrap=50)
    r1 = next(r for r in t.rows if r.width == 1)
    assert r1.n_queries == 2 and r1.n_pairs == 4  # replicates counted, not treated as new queries
    assert r1.y == pytest.approx(0.5 * math.log(2.0))


def test_identical_file_twice_is_refused(tmp_path: Path) -> None:
    f = _tiny(tmp_path)
    g = tmp_path / "copy.json"
    g.write_bytes(f.read_bytes())
    with pytest.raises(ResultFileError, match="byte-identical"):
        build_table([f, g], seed=1)


def test_empty_dataset_field_needs_a_mapping_or_unique_inference(tmp_path: Path) -> None:
    f = _tiny(tmp_path, ds="", exp="policy-campaign-snap_facebook-dijkstra")
    with pytest.raises(ResultFileError, match="--assume-dataset"):
        build_table([f], seed=1)
    t = build_table([f], seed=1, known_datasets=["snap_facebook", "dimacs_ny"])
    assert {r.dataset for r in t.rows} == {"snap_facebook"}
    assert "snap_facebook" in t.inferred_datasets
    t2 = build_table([f], seed=1, assume_dataset={"a.json": "dimacs_ny"})
    assert {r.dataset for r in t2.rows} == {"dimacs_ny"}
    # ambiguous inference is refused rather than guessed
    with pytest.raises(ResultFileError):
        build_table([_tiny(tmp_path, "c.json", ds="", exp="x-snap_facebook-dimacs_ny")], seed=1,
                    known_datasets=["snap_facebook", "dimacs_ny"])


def test_real_result_files_build_if_present() -> None:
    f = REPO / "results" / "raw" / "v2" / "ny_pooled1_interleaved.json"
    if not f.is_file():
        pytest.skip("v2 result file not present")
    t = build_table([f], seed=5, bootstrap=100, assume_dataset={f.name: "dimacs_ny"})
    assert {r.width for r in t.rows} == {1, 2, 4, 8, 16, 37}
    assert all(r.n_queries == 100 for r in t.rows)
    assert ("dimacs_ny", "dijkstra", "replay_floor") in t.ignored


# --- topology / graph catalog ------------------------------------------------------


def test_declared_base_chains_and_source_path_fallback(tmp_path: Path) -> None:
    meta = tmp_path / "meta"
    meta.mkdir()
    (meta / "real.json").write_text(json.dumps({"dataset_id": "real", "source_path": "x/y.gr"}))
    (meta / "d1.json").write_text(json.dumps({"derived_from": "real"}))
    (meta / "d2.json").write_text(json.dumps({"derived_from": "d1"}))
    (meta / "synth.json").write_text(json.dumps({"source_path": "datasets/processed/real.lpqgraph"}))
    (meta / "w.json").write_text(json.dumps({"weights": {"derived_from": "real"}}))
    for ds, base in [("real", "real"), ("d1", "real"), ("d2", "real"), ("synth", "real"), ("w", "real")]:
        assert declared_base(ds, meta) == base
    (meta / "c1.json").write_text(json.dumps({"derived_from": "c2"}))
    (meta / "c2.json").write_text(json.dumps({"derived_from": "c1"}))
    with pytest.raises(PolicyFormatError, match="cycle"):
        declared_base("c1", meta)
    with pytest.raises(PolicyFormatError, match="metadata not found"):
        declared_base("unknown", meta)  # fail closed, never guess a topology


def test_load_graphs_groups_topologies(tmp_path: Path) -> None:
    pr = make_profile(1000, 3000, 100.0, 5.0, 0.2, checksum=1)
    write_graph_files(tmp_path, "dimacs_x", pr)
    write_graph_files(tmp_path, "derived_x_u16", make_profile(1000, 3000, 16.0, 8.0, 0.5, checksum=2),
                      derived_from="dimacs_x")
    write_graph_files(tmp_path, "derived_x_u64", make_profile(1000, 3000, 64.0, 8.0, 0.5, checksum=3),
                      derived_from="dimacs_x")
    write_graph_files(tmp_path, "other", make_profile(2000, 6000, 100.0, 5.0, 0.2, checksum=4))
    # no derived_from, but identical node/arc counts: merged conservatively
    write_graph_files(tmp_path, "lookalike", make_profile(2000, 6000, 77.0, 5.0, 0.2, checksum=5))
    infos = load_graphs(["derived_x_u16", "derived_x_u64", "dimacs_x", "other", "lookalike"],
                        tmp_path / "profiles", tmp_path / "meta")
    assert infos["dimacs_x"].topology == infos["derived_x_u16"].topology == infos["derived_x_u64"].topology
    assert infos["dimacs_x"].topology == "dimacs_x"  # a real graph names its group
    assert not infos["dimacs_x"].declared_derived and infos["derived_x_u16"].declared_derived
    assert infos["other"].topology == infos["lookalike"].topology != infos["dimacs_x"].topology


def test_stale_or_foreign_profiles_are_refused(tmp_path: Path) -> None:
    write_graph_files(tmp_path, "g", make_profile(1000, 3000, 100.0, 5.0, 0.2, checksum=9))
    with pytest.raises(PolicyFormatError, match="nodes/arcs"):
        load_graphs(["g"], tmp_path / "profiles", tmp_path / "meta", {"g": (999, 3000)})
    meta = tmp_path / "meta" / "g.json"
    d = json.loads(meta.read_text())
    d["graph_checksum_fnv1a64"] = 10
    meta.write_text(json.dumps(d))
    with pytest.raises(PolicyFormatError, match="checksum"):
        load_graphs(["g"], tmp_path / "profiles", tmp_path / "meta")
    with pytest.raises(PolicyFormatError, match="missing.lpqprofile"):
        load_graphs(["missing"], tmp_path / "profiles", tmp_path / "meta")


def test_record_shape_accepts_edge_count_of_an_undirected_graph(tmp_path: Path) -> None:
    # the engine writes graph_edges = edge count (half the arcs of an undirected graph)
    write_graph_files(tmp_path, "u", make_profile(1000, 3000, 100.0, 5.0, 0.2, checksum=9),
                      extra_meta={"edge_count": 1500})
    assert "u" in load_graphs(["u"], tmp_path / "profiles", tmp_path / "meta", {"u": (1000, 1500)})
    assert "u" in load_graphs(["u"], tmp_path / "profiles", tmp_path / "meta", {"u": (1000, 3000)})
    with pytest.raises(PolicyFormatError, match="nodes/arcs"):
        load_graphs(["u"], tmp_path / "profiles", tmp_path / "meta", {"u": (1000, 1499)})
    write_graph_files(tmp_path, "v", make_profile(1000, 3000, 100.0, 5.0, 0.2, checksum=10),
                      extra_meta={"arc_count": 2999})
    with pytest.raises(PolicyFormatError, match="arcs=3000"):
        load_graphs(["v"], tmp_path / "profiles", tmp_path / "meta")
