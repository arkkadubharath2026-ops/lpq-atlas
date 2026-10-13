#!/usr/bin/env python3
"""M6 analysis: reads the raw result files and the profile logs and prints the tables used in the report.

Everything here is a function of the raw files; nothing is dropped. Randomised steps use explicit seeds.

    m6_analyse.py <m6-worktree> <evidence-dir>
"""
from __future__ import annotations

import json
import re
import statistics
import sys
from pathlib import Path

import numpy as np

W = Path(sys.argv[1])
E = Path(sys.argv[2])
sys.path.insert(0, str(W / "python"))
from lpq_tools.statistics import adjust_comparisons, compare_paired  # noqa: E402

RAW = W / "results" / "raw" / "m6"
SEED = 20260923
RESAMPLES = 10_000
ROWS = ["radix_heap", "binary_heap", "four_ary_heap", "bucket_classical", "lpq", "lpq_model",
        "lpq_direct_nonml", "lpq_optimized", "replay_floor"]


def load(path: Path) -> tuple[dict, dict[str, dict[tuple[int, int], dict]]]:
    d = json.loads(path.read_text())
    rows: dict[str, dict[tuple[int, int], dict]] = {}
    for r in d["records"]:
        rows.setdefault(r["pq"], {})[(r["source"], r["target"])] = r
    return d, rows


def med(x):
    return float(np.median(x))


def mad(x):
    x = np.asarray(x, float)
    return float(np.median(np.abs(x - np.median(x))))


def boot_ci(values: np.ndarray, rng: np.random.Generator, stat=np.median) -> tuple[float, float]:
    n = len(values)
    idx = rng.integers(0, n, size=(RESAMPLES, n))
    s = stat(values[idx], axis=1)
    return float(np.percentile(s, 2.5)), float(np.percentile(s, 97.5))


def per_query(rows, name, queries):
    return np.array([rows[name][q]["timing"]["median_ns"] for q in queries], float)


def fmt_ms(ns):
    return f"{ns / 1e6:.2f}"


out: list[str] = []


def p(s=""):
    out.append(s)
    print(s)


# ------------------------------------------------------------------------------------------------
# Blocks A and B: three invocations each
# ------------------------------------------------------------------------------------------------
def block(tag: str, files: list[Path], title: str):
    p(f"## {title}")
    data = [load(f) for f in files]
    queries = sorted(data[0][1]["radix_heap"].keys())
    n_inv = len(data)
    # exactness and record counts
    total = sum(len(d["records"]) for d, _ in data)
    exact = sum(1 for d, _ in data for r in d["records"] if r["correctness"].get("exact_match") is True)
    noisy = [sum(1 for r in d["records"] if r["timing"]["noisy"]) for d, _ in data]
    pinned = {r["timing"]["cpu_pinned"] for d, _ in data for r in d["records"]}
    commits = {(r["environment"]["git_commit"], r["environment"]["git_dirty"]) for d, _ in data for r in d["records"]}
    p(f"records {total}, exact_match true {exact}, cpu_pinned values {sorted(pinned)}, (commit, dirty) {sorted(commits)}")
    p(f"noisy-flagged records per invocation: {noisy} of {[len(d['records']) for d, _ in data]}")
    # anomaly rule
    anchors = [med(per_query(rows, "radix_heap", queries)) for _, rows in data]
    mid = med(anchors)
    dev = [abs(a - mid) / mid for a in anchors]
    anomalous_a = [x > 0.25 for x in dev]
    anomalous_b = [nz / len(d["records"]) > 0.5 for nz, (d, _) in zip(noisy, data)]
    p(f"anchor (radix_heap median of per-query medians) ms per invocation: {[fmt_ms(a) for a in anchors]}; "
      f"deviation from their median: {[f'{x:.1%}' for x in dev]}; rule (a) anomalous: {anomalous_a}; rule (b) anomalous: {anomalous_b}")
    p()
    # absolute table (host-specific)
    p("### Absolute per-row figures (host-specific; median / MAD across the 100 queries of each query's median, ms)")
    p("| row | " + " | ".join(f"inv{i + 1} median (MAD)" for i in range(n_inv)) + " | comparisons (inv1) | noisy records (all inv) |")
    p("|---|" + "---|" * (n_inv + 2))
    for name in ROWS:
        cells = []
        for _, rows in data:
            v = per_query(rows, name, queries)
            cells.append(f"{fmt_ms(med(v))} ({fmt_ms(mad(v))})")
        comps = sum(data[0][1][name][q]["pq_operations"].get("comparisons", 0) for q in queries)
        nz = sum(1 for _, rows in data for q in queries if rows[name][q]["timing"]["noisy"])
        p(f"| {name} | " + " | ".join(cells) + f" | {comps:,} | {nz} |")
    p()
    # paired ratios against radix and the registered pairs
    pairs = [("binary_heap", "radix_heap"), ("four_ary_heap", "radix_heap"), ("bucket_classical", "radix_heap"),
             ("lpq", "radix_heap"), ("lpq_model", "radix_heap"), ("lpq_direct_nonml", "radix_heap"),
             ("lpq_optimized", "radix_heap"), ("replay_floor", "radix_heap"),
             ("lpq_model", "lpq"), ("lpq_direct_nonml", "lpq")]
    p("### Paired ratios, row A / row B, per query (A slower when above 1)")
    p("Per invocation: median ratio [95% bootstrap CI over queries], fraction of queries where A is faster, Holm-adjusted "
      "permutation p within the invocation's family of 10 comparisons. Pooled: cluster bootstrap over queries across "
      "all invocations. 'established' needs the same sign in every invocation AND a pooled CI that excludes 1.")
    p("| A / B | " + " | ".join(f"inv{i + 1}" for i in range(n_inv)) + " | pooled median [95% CI] | A faster on (pooled) | verdict |")
    p("|---|" + "---|" * (n_inv + 3))
    rng = np.random.default_rng(SEED)
    comps_by_inv = []
    for i, (_, rows) in enumerate(data):
        cs = [compare_paired(per_query(rows, a, queries), per_query(rows, b, queries), seed=SEED + i, name_a=a, name_b=b)
              for a, b in pairs]
        comps_by_inv.append(adjust_comparisons(cs))
    verdicts = {}
    for j, (a, b) in enumerate(pairs):
        cells, signs, mats = [], [], []
        for i, (_, rows) in enumerate(data):
            r = per_query(rows, a, queries) / per_query(rows, b, queries)
            mats.append(r)
            lo, hi = boot_ci(r, rng)
            c = comps_by_inv[i][j]
            cells.append(f"{med(r):.3f} [{lo:.3f}, {hi:.3f}] f={float(np.mean(r < 1)):.2f} p_adj={c.adjusted_p_value:.3g}")
            signs.append(np.sign(med(r) - 1.0))
        M = np.vstack(mats)  # inv x query
        idx = rng.integers(0, M.shape[1], size=(RESAMPLES, M.shape[1]))
        pooled_stats = np.median(M[:, idx].reshape(M.shape[0], RESAMPLES, -1).transpose(1, 0, 2).reshape(RESAMPLES, -1), axis=1)
        plo, phi = float(np.percentile(pooled_stats, 2.5)), float(np.percentile(pooled_stats, 97.5))
        pm = med(M)
        same = len(set(signs)) == 1 and signs[0] != 0
        excl = plo > 1.0 or phi < 1.0
        v = ("established " + ("slower" if signs[0] > 0 else "faster")) if (same and excl) else "not distinguishable"
        verdicts[(a, b)] = (v, pm, plo, phi, signs)
        p(f"| {a} / {b} | " + " | ".join(cells) + f" | {pm:.3f} [{plo:.3f}, {phi:.3f}] | {float(np.mean(M < 1)):.2f} | {v} |")
    p()
    # lpq_optimized resolution
    p("### lpq_optimized resolution and one-time costs (per invocation)")
    for i, (_, rows) in enumerate(data):
        qc = rows["lpq_optimized"][queries[0]]["queue_config"]
        p(f"inv{i + 1}: resolved_queue={qc.get('resolved_queue')}, method={qc.get('resolution_method')}, profile_source={qc.get('profile_source')}, "
          f"profile_ns={qc.get('profile_ns')}, calibration_ns={qc.get('calibration_ns')}; reason: {qc.get('resolution_reason')}")
    p()
    best = min((r for r in ROWS if r in ("radix_heap", "binary_heap", "four_ary_heap", "bucket_classical", "lpq_optimized")),
               key=lambda r: med([med(per_query(rows, r, queries)) for _, rows in data]))
    p(f"fastest classical row by pooled median of per-query medians: {best}")
    p()
    return data, verdicts


a_files = [RAW / f"A_ny_inv{i}.json" for i in (1, 2, 3)]
b_files = [RAW / f"B_fb_inv{i}.json" for i in (1, 2, 3)]
a_data, a_verd = block("A", a_files, "Block A: dimacs_ny, Dijkstra, exact, 100 queries")
b_data, b_verd = block("B", b_files, "Block B: snap_facebook, Dijkstra, exact, 100 queries")

# ------------------------------------------------------------------------------------------------
# Block C: counters overhead
# ------------------------------------------------------------------------------------------------
p("## Block C: cost of the pq-operation counters (release vs release-noinstr), dimacs_ny")
rng = np.random.default_rng(SEED + 7)
rel = [load(RAW / f"C_rel_{k}.json") for k in (1, 2)]
ni = [load(RAW / f"C_ni_{k}.json") for k in (1, 2)]
queries_c = sorted(rel[0][1]["radix_heap"].keys())
ni_instr = {r["environment"].get("instrumentation_enabled") for d, _ in ni for r in d["records"]}
rel_instr = {r["environment"].get("instrumentation_enabled") for d, _ in rel for r in d["records"]}
p(f"instrumentation_enabled: release {sorted(rel_instr)}, release-noinstr {sorted(ni_instr)}")
p("| row | pair | counters-on / counters-off, median [95% CI] | queries where counters-on slower |")
p("|---|---|---|---|")
c_over = {}
for name in ("radix_heap", "binary_heap", "lpq"):
    allr = []
    for k in (0, 1):
        r = per_query(rel[k][1], name, queries_c) / per_query(ni[k][1], name, queries_c)
        lo, hi = boot_ci(r, rng)
        allr.append(r)
        p(f"| {name} | {k + 1} | {med(r):.3f} [{lo:.3f}, {hi:.3f}] | {float(np.mean(r > 1)):.2f} |")
    M = np.vstack(allr)
    idx = rng.integers(0, M.shape[1], size=(RESAMPLES, M.shape[1]))
    ps = np.median(M[:, idx].reshape(2, RESAMPLES, -1).transpose(1, 0, 2).reshape(RESAMPLES, -1), axis=1)
    c_over[name] = (med(M), float(np.percentile(ps, 2.5)), float(np.percentile(ps, 97.5)), [med(x) for x in allr])
    p(f"| {name} | pooled | {c_over[name][0]:.3f} [{c_over[name][1]:.3f}, {c_over[name][2]:.3f}] | {float(np.mean(M > 1)):.2f} |")
cnt_ni = sum(r["pq_operations"].get("comparisons", 0) for d, _ in ni for r in d["records"])
cnt_rel = sum(r["pq_operations"].get("comparisons", 0) for d, _ in rel for r in d["records"])
p(f"comparisons recorded: release {cnt_rel:,}, release-noinstr {cnt_ni:,} (counters off = 0 by construction)")
p()

# ------------------------------------------------------------------------------------------------
# Block D: phase profile
# ------------------------------------------------------------------------------------------------
p("## Block D: phase profile (dimacs_ny, 3 passes over 100 queries)")


def parse_profile(path: Path):
    t = path.read_text()
    d = {}
    for key, rx in (("wall", r"wall clock of region\s*:\s*([\d.]+) s"), ("probe", r"cost of one probe pair\s*:\s*([\d.]+) ticks")):
        m = re.search(rx, t)
        d[key] = float(m.group(1)) if m else None
    rows = {}
    for line in t.splitlines():
        m = re.match(r"\s*(push|pop \(total\)|fallback scan|bucket traversal|window maintenance|verification scan|remove from bucket|pop \(unattributed\))\s+(\d+)\s+(\d+|-)\s+([\d.]+%|-)\s+([\d.]+%|-)", line)
        if m:
            rows[m.group(1)] = (int(m.group(2)), m.group(4), m.group(5))
    d["rows"] = rows
    return d


p("| queue | round | uninstrumented wall (s) | with probes wall (s) | inflation | probe pair (ticks) |")
p("|---|---|---|---|---|---|")
infl = {}
prof = {}
for q in ("lpq", "binary_heap"):
    for rnd in (1, 2):
        a = parse_profile(E / f"D_lpq_profile_noinstr_{q}_r{rnd}.log")
        b = parse_profile(E / f"D_lpq_profile_{q}_r{rnd}.log")
        prof[(q, rnd)] = b
        infl.setdefault(q, []).append(b["wall"] / a["wall"])
        p(f"| {q} | {rnd} | {a['wall']:.3f} | {b['wall']:.3f} | {b['wall'] / a['wall']:.3f}x | {b['probe']} |")
p()
p("Phase shares, LPQ (% of pop ticks), both rounds:")
for rnd in (1, 2):
    r = prof[("lpq", rnd)]["rows"]
    pop = r["pop (total)"][0]
    share = lambda k: 100.0 * r[k][0] / pop
    wv = share("window maintenance") + share("verification scan") + share("fallback scan")
    p(f"round {rnd}: " + ", ".join(f"{k} {share(k):.2f}%" for k in ("bucket traversal", "window maintenance", "verification scan",
                                                                    "fallback scan", "remove from bucket", "pop (unattributed)"))
      + f"; window+verification+fallback = {wv:.2f}% of pop; push share of push+pop = {100 * r['push'][0] / (r['push'][0] + pop):.2f}%")
p()

# ------------------------------------------------------------------------------------------------
# Block E: online
# ------------------------------------------------------------------------------------------------
p("## Block E: lpq_online vs lpq_model on snap_facebook (100 queries, one invocation per mode)")
for mode in ("disabled", "periodic"):
    d, rows = load(RAW / f"E_fb_{mode}.json")
    qs = sorted(rows["radix_heap"].keys())
    on = f"online_{mode}"
    t_model = per_query(rows, "lpq_model", qs)
    t_on = per_query(rows, on, qs)
    r = t_on / t_model
    rng = np.random.default_rng(SEED + 11)
    lo, hi = boot_ci(r, rng)
    upd = np.array([rows[on][q]["ml"]["online_update_ns"] for q in qs], float)
    samples_ok = all(len(rows[on][q]["timing"]["samples_ns"]) == rows[on][q]["timing"]["repetitions"] == 7 for q in qs)
    lr = [rows[on][q]["learner"] for q in qs]
    changed = sum(1 for x in lr if x.get("weights_hash_before") != x.get("weights_hash_after"))
    ups = [x.get("updates_applied", 0) for x in lr]
    exact = all(rows[on][q]["correctness"]["exact_match"] for q in qs)
    p(f"mode {mode}: exact_match all {exact}; search time online / model per query: median {med(r):.3f} [{lo:.3f}, {hi:.3f}], "
      f"online faster on {float(np.mean(r < 1)):.2f} of queries; median T_search model {fmt_ms(med(t_model))} ms, online {fmt_ms(med(t_on))} ms")
    p(f"   online_update_ns per query: median {med(upd) / 1e3:.1f} us, MAD {mad(upd) / 1e3:.1f} us, max {upd.max() / 1e3:.1f} us, "
      f"sum {upd.sum() / 1e6:.2f} ms; queries whose weights hash changed: {changed}; total updates (last record): {max(ups) if ups else 0}; "
      f"timing.samples_ns has exactly 7 entries on every record (update cost not inside it): {samples_ok}")
    p(f"   labelled estimate, search + update per query (update counted once per query): {fmt_ms(med(t_on) + med(upd))} ms "
      f"(median T_search {fmt_ms(med(t_on))} ms + median update {med(upd) / 1e6:.4f} ms)")
p()

# ------------------------------------------------------------------------------------------------
# Scorer-build probe
# ------------------------------------------------------------------------------------------------
p("## Scorer-build estimate (process-level, coarse)")
rows = json.loads((E / "S_scorer_probe.json").read_text())
d_model = np.array([(b - (a + c) / 2) / 1e6 for a, b, c in rows])
d_null = np.array([(c - a) / 1e6 for a, b, c in rows])
rng = np.random.default_rng(SEED + 13)
lo, hi = boot_ci(d_model, rng)
nlo, nhi = boot_ci(d_null, rng)
p(f"lpq_model minus mean of the surrounding lpq runs, ms: median {med(d_model):.1f}, MAD {mad(d_model):.1f}, 95% bootstrap CI of the median [{lo:.1f}, {hi:.1f}], positive in {int((d_model > 0).sum())} of {len(d_model)}")
p(f"null (lpq after minus lpq before), ms: median {med(d_null):.1f}, MAD {mad(d_null):.1f}, CI of the median [{nlo:.1f}, {nhi:.1f}], range [{d_null.min():.1f}, {d_null.max():.1f}]")
p()

(E / "M6_analysis_output.md").write_text("\n".join(out) + "\n")
