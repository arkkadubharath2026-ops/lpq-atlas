// Quick Compare (the landing page), against the real API and engine.
// Which lanes run depends on the engine build: lanes whose queue the build lacks
// must show a per-lane error and must not break the page, so every assertion
// below holds for both a build with and one without the Optimized queue.
import { expect, test, type APIRequestContext, type Page } from "@playwright/test";

const API = process.env.ATLAS_API_URL ?? "http://127.0.0.1:8000";

async function readyDataset(request: APIRequestContext): Promise<string | null> {
  const list = (await (await request.get(`${API}/api/datasets`)).json()) as {
    id: string; status: string; nodes?: number;
  }[];
  const ready = list.filter((d) => d.status === "READY");
  // The smallest ready graph keeps the test fast; the page works the same on any.
  ready.sort((a, b) => (a.nodes ?? 0) - (b.nodes ?? 0));
  return ready[0]?.id ?? null;
}

async function runFromRandomRoute(page: Page): Promise<string> {
  await page.getByTestId("random-pair").click();
  await expect(page.getByTestId("source-node")).not.toHaveValue("");
  await expect(page.getByTestId("target-node")).not.toHaveValue("");
  const created = page.waitForResponse(
    (r) => r.url().endsWith("/api/lane-comparisons") && r.request().method() === "POST",
  );
  await page.getByTestId("run-comparison").click();
  const res = await created;
  expect(res.status()).toBe(202);
  return (await res.json()).id as string;
}

test("quick compare: pick, run, and see ranked lanes with the API's own numbers", async ({ page, request }) => {
  const ds = await readyDataset(request);
  test.skip(!ds, "no dataset is READY");
  await page.goto(`/?dataset=${ds}`);
  await expect(page.getByRole("heading", { name: /Which shortest-path method is fastest/ })).toBeVisible();
  await expect(page.getByTestId("dataset-select")).toHaveValue(ds!);
  await expect(page.getByTestId("run-comparison")).toBeDisabled();

  const id = await runFromRandomRoute(page);
  await expect(page.getByTestId("results")).toBeVisible({ timeout: 120_000 });

  const job = await (await request.get(`${API}/api/lane-comparisons/${id}`)).json();
  const result = job.result;
  expect(result.measurement.repetitions).toBe(7);
  expect(result.measurement.warmup).toBe(2);
  expect(result.measurement.tracing).toBe("off");

  // One row per requested lane, each either timed or showing its own error.
  const rows = page.getByTestId("lane-row");
  await expect(rows).toHaveCount(result.lanes.length);
  for (const lane of result.lanes) {
    const row = page.locator(`[data-testid=lane-row][data-lane=${lane.id}]`);
    await expect(row).toHaveAttribute("data-status", lane.status);
    if (lane.status === "error") {
      await expect(row.getByTestId("lane-error")).toContainText("did not run");
    } else {
      await expect(row.getByTestId("lane-median")).toBeVisible();
    }
  }

  // The fastest lane shown is the one the API named, and only that row is marked.
  if (result.fastest) {
    await expect(page.locator("[data-testid=lane-row][data-fastest=true]")).toHaveCount(1);
    await expect(
      page.locator(`[data-testid=lane-row][data-lane=${result.fastest.lane_id}]`),
    ).toHaveAttribute("data-fastest", "true");
  }

  // Honest labelling: separate time and count sections, the repeat count, the same-distance check.
  await expect(page.getByRole("heading", { name: "Measured wall-clock time" })).toBeVisible();
  await expect(page.getByRole("heading", { name: "Algorithmic operation counts" })).toBeVisible();
  await expect(page.getByTestId("measurement-note")).toContainText("ran 7 times after 2 warm-up runs");
  await expect(page.getByTestId("measurement-note")).toContainText("on this machine");
  await expect(page.getByTestId("correctness")).toContainText(/same optimal distance|no route/);
  await expect(page.getByTestId("summary")).toBeVisible();

  // Advanced details stay collapsed until asked for.
  await expect(page.getByTestId("results-advanced")).not.toHaveAttribute("open", "");
});

test("quick compare: a failed lane is shown on its own row and the page stays usable", async ({ page, request }) => {
  const ds = await readyDataset(request);
  test.skip(!ds, "no dataset is READY");
  // The only mocked response in this file: a synthetic API reply, so the per-lane
  // error rendering is checked even when the real engine can run every lane.
  const lane = (id: string, algorithm: string, queue: string, ok: boolean, median: number) => ({
    id, label: id, algorithm, queue, family: queue.startsWith("lpq") ? "learned" : "classical",
    run_index: 1, run_id: id, status: ok ? "ok" : "error",
    error: ok ? null : { category: "engine", message: "unknown --pq: lpq_optimized" },
    timing: ok ? { median_ns: median, mad_ns: 10, relative_mad: 0.01, min_ns: median, max_ns: median, samples_ns: [median], repetitions: 7, warmup: 2, noisy: false, pinned: true, core: 1, warnings: [] } : null,
    counts: ok ? { pops: 5, pushes: 5, decrease_keys: 0, comparisons: 9, settled_nodes: 5, stale_pops: 0, edges_examined: 9, edges_relaxed: 5, peak_queue_size: 3 } : undefined,
    distance: ok ? 42 : undefined, reachable: ok, matches_reference: ok ? true : null,
    effect: queue === "binary_heap" ? "vanilla" : "ml", vanilla_lane_id: ok ? "vanilla_dijkstra" : null,
    speedup_vs_vanilla: ok ? 1 : null, verdict: ok ? "reference" : "unavailable", within_noise: null,
    rank: ok ? 1 : null, vs_fastest: ok ? 1 : null,
  });
  const body = {
    id: "lan_mock", kind: "lane_comparison", state: "COMPLETED", created_at: 0, updated_at: 0, error: null, progress: null, summary: {},
    result: {
      schema_version: 1, dataset_id: ds, source_node: 0, target_node: 1,
      measurement: { repetitions: 7, warmup: 2, tracing: "off", execution: "sequential", run_order: ["vanilla_dijkstra", "optimized_dijkstra"], order_seed: 1, all_pinned: true, cores: [1], clock: "", machine: { os: "Linux", arch: "x86_64" }, elapsed_s: 1, contended_lanes: [] },
      heuristic: { requested: "auto", used_by_astar_lanes: null, shared: true },
      lanes: [lane("vanilla_dijkstra", "dijkstra", "binary_heap", true, 1000), lane("optimized_dijkstra", "dijkstra", "lpq_optimized", false, 0)],
      path: [], fastest: { lane_id: "vanilla_dijkstra", median_ns: 1000, tied_with: [], clear_win: true },
      correctness: { checked_lanes: 1, failed_lanes: ["optimized_dijkstra"], references_agree: true, all_lanes_same_distance: true, all_match_reference: true, mismatching_lanes: [], reference_distance: 42, reachable: true },
      effects: [], noise_rule: "", vanilla_queue: "binary_heap",
    },
  };
  await page.route("**/api/lane-comparisons", (route) =>
    route.request().method() === "POST"
      ? route.fulfill({ status: 202, contentType: "application/json", body: JSON.stringify({ ...body, state: "QUEUED", result: undefined }) })
      : route.continue(),
  );
  await page.route("**/api/lane-comparisons/lan_mock", (route) =>
    route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify(body) }),
  );
  await page.goto(`/?dataset=${ds}`);
  await page.getByTestId("random-pair").click();
  await expect(page.getByTestId("source-node")).not.toHaveValue("");
  await page.getByTestId("run-comparison").click();
  await expect(page.getByTestId("results")).toBeVisible();
  const failed = page.locator("[data-testid=lane-row][data-lane=optimized_dijkstra]");
  await expect(failed.getByTestId("lane-error")).toContainText("unknown --pq: lpq_optimized");
  await expect(page.getByTestId("summary")).toContainText("Optimized Dijkstra could not run");
  await expect(page.getByTestId("correctness")).toContainText("did not run");
  await expect(page.getByTestId("run-comparison")).toBeEnabled();
});
