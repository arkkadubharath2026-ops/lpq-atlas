#!/usr/bin/env python3
"""Readiness check for a running LPQ Atlas: does a real query travel UI -> API -> engine and come back?

    python scripts/setup/smoke.py [--web URL] [--api URL] [--dataset demo_road]

Standard library only, so it runs with any Python 3 (`run.sh` and `run.ps1` call it with the virtualenv's).
Every request goes through the web server's own address (the browser's route), so it also proves the
UI's /api rewrite points at the API. The queries use the engine on a ready dataset and are validated by
the engine's trusted reference search (`exact_match`), so a "PASS" means a real search ran and was checked.
Exit status: 0 all checks passed, 1 a check failed (the first failure is explained), 2 the dataset is not ready.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request


class Failure(Exception):
    pass


def call(base: str, path: str, method: str = "GET", body: dict | None = None, timeout: float = 30.0):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(base + path, method=method, data=data, headers={"content-type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read()
            return r.status, (json.loads(raw) if raw[:1] in (b"{", b"[") else raw)
    except urllib.error.HTTPError as e:
        raw = e.read()
        try:
            return e.code, json.loads(raw)
        except ValueError:
            return e.code, raw
    except (urllib.error.URLError, OSError) as e:
        raise Failure(f"cannot reach {base}{path}: {getattr(e, 'reason', e)}") from e


def run_query(web: str, body: dict, label: str, wait_s: float = 90.0) -> dict:
    status, job = call(web, "/api/runs", "POST", body)
    if status != 202:
        raise Failure(f"{label}: the API refused the run ({status}): {json.dumps(job)[:300]}")
    deadline = time.time() + wait_s
    while time.time() < deadline:
        status, st = call(web, f"/api/runs/{job['id']}")
        if st.get("state") in ("COMPLETED", "FAILED", "CANCELLED"):
            break
        time.sleep(0.25)
    else:
        raise Failure(f"{label}: no result within {wait_s:.0f} s (run {job['id']})")
    if st["state"] != "COMPLETED":
        raise Failure(f"{label}: the run ended {st['state']}: {json.dumps(st.get('error'))[:300]}")
    status, res = call(web, f"/api/runs/{job['id']}/result")
    if status != 200:
        raise Failure(f"{label}: the result could not be read ({status})")
    return res


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--web", default="http://127.0.0.1:3000")
    ap.add_argument("--dataset", default="demo_road")
    args = ap.parse_args()
    web = args.web.rstrip("/")

    def ok(name: str, detail: str = "") -> None:
        print(f"  [ ok ] {name:<34} {detail}")

    try:
        status, _ = call(web, "/")
        if status != 200:
            raise Failure(f"the web UI answered {status}")
        ok("web UI", web)

        status, health = call(web, "/api/health")
        if status != 200 or not isinstance(health, dict) or health.get("status") != "ok":
            raise Failure(f"/api/health through the web server answered {status}: {str(health)[:200]}")
        missing = [k for k, v in health.get("capabilities", {}).items() if not v]
        if missing:
            raise Failure(f"engine tools missing: {', '.join(missing)}. Run scripts/setup/setup.sh (or setup.ps1) to build them.")
        ok("API and engine tools", ", ".join(sorted(health["capabilities"])))

        status, ds = call(web, f"/api/datasets/{args.dataset}")
        if status != 200 or ds.get("status") != "READY":
            print(f"  [skip] dataset {args.dataset} is not READY ({ds.get('status') if isinstance(ds, dict) else status}); "
                  "run: python scripts/demo/prepare_demo.py")
            return 2
        ok(f"dataset {args.dataset}", f"{ds.get('nodes')} nodes, {ds.get('arcs')} arcs, READY")

        status, pair = call(web, f"/api/graphs/{args.dataset}/random-pair?seed=1")
        if status != 200:
            raise Failure(f"could not draw a source/target pair ({status}): {str(pair)[:200]}")
        src, dst = pair["source_node"], pair["target_node"]

        base = {"dataset_id": args.dataset, "source_node": src, "target_node": dst, "repetitions": 1, "warmup": 0}
        a = run_query(web, {**base, "queue": "binary_heap", "trace_level": "none"}, "binary_heap query")
        if not a["result"]["reachable"] or a["correctness"]["exact_match"] is not True:
            raise Failure(f"binary_heap query not validated: reachable={a['result']['reachable']} "
                          f"exact_match={a['correctness']['exact_match']}")
        ok("engine query, binary heap", f"{src} -> {dst}, distance {a['result']['distance']}, exact vs reference")

        b = run_query(web, {**base, "queue": "lpq", "trace_level": "visual"}, "lpq query")
        if b["result"]["distance"] != a["result"]["distance"] or b["correctness"]["exact_match"] is not True:
            raise Failure(f"lpq query disagrees: distance {b['result']['distance']} vs {a['result']['distance']}")
        if b.get("trace", {}).get("matches_untraced_run") is not True:
            raise Failure("the traced lpq run differs from the untraced run")
        ok("engine query, learned queue + trace", f"distance {b['result']['distance']}, trace identical to untraced run")
    except Failure as e:
        print(f"  [FAIL] {e}", file=sys.stderr)
        return 1
    print("  smoke test passed: UI -> API -> engine -> validated result")
    return 0


if __name__ == "__main__":
    sys.exit(main())
