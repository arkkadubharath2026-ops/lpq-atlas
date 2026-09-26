// Critical flows (spec section 43), against the real API and engine.
import { expect, test, type APIRequestContext, type Page } from "@playwright/test";

const API = process.env.ATLAS_API_URL ?? "http://127.0.0.1:8000";

/** CI prepares only the small Monaco extract; tests needing more skip themselves. */
async function ready(request: APIRequestContext, id: string): Promise<boolean> {
  const r = await request.get(`${API}/api/datasets/${id}`);
  return r.ok() && (await r.json()).status === "READY";
}

async function clickMap(page: Page, fx: number, fy: number) {
  const map = page.locator(".leaflet-container").first();
  const box = await map.boundingBox();
  if (!box) throw new Error("map not visible");
  await page.mouse.click(box.x + box.width * fx, box.y + box.height * fy);
}

async function runAndGetId(page: Page): Promise<string> {
  const created = page.waitForResponse((r) => r.url().endsWith("/api/runs") && r.request().method() === "POST");
  await page.getByTestId("run-button").click();
  const res = await created;
  expect(res.status()).toBe(202);
  const id = (await res.json()).id as string;
  await expect(page.getByTestId("telemetry")).toBeVisible();
  return id;
}

test("explore: select on the map, run Dijkstra then LPQ, metrics match the API, replay steps", async ({ page, request }) => {
  await page.goto("/explore?dataset=osm_monaco");
  await expect(page.getByTestId("dataset-select")).toHaveValue("osm_monaco");
  await expect(page.locator(".leaflet-container")).toBeVisible();
  await page.waitForTimeout(1500); // fitBounds + coordinate download

  await clickMap(page, 0.3, 0.45);
  await expect(page.getByTestId("source-node")).not.toHaveValue("");
  await clickMap(page, 0.7, 0.55);
  await expect(page.getByTestId("target-node")).not.toHaveValue("");

  // Dijkstra + binary heap (defaults)
  const dijkstraId = await runAndGetId(page);
  const r1 = await (await request.get(`${API}/api/runs/${dijkstraId}/result`)).json();
  expect(r1.result.reachable).toBe(true);
  await expect(page.getByTestId("distance")).toContainText(Number(r1.result.distance).toLocaleString("en-US"));
  expect(r1.correctness.exact_match).toBe(true);
  expect(r1.trace.matches_untraced_run).toBe(true);

  // Replay: step one settled node and read the engine-derived explanation.
  await page.getByRole("button", { name: "Restart" }).click();
  await page.getByRole("button", { name: "Step node" }).click();
  await expect(page.getByTestId("explanation")).toContainText("settled");

  // LPQ on the same query
  await page.getByLabel("Priority queue").selectOption("lpq");
  const lpqId = await runAndGetId(page);
  const r2 = await (await request.get(`${API}/api/runs/${lpqId}/result`)).json();
  expect(r2.request.queue).toBe("lpq");
  expect(r2.result.distance).toBe(r1.result.distance);
  expect(r2.lpq.metrics.extractions).toBeGreaterThan(0);
  await expect(page.getByText("LPQ decisions")).toBeVisible();
  await expect(page.getByTestId("median")).toBeVisible();
});

test("compare: same query side by side with the reference", async ({ page }) => {
  await page.goto("/compare?dataset=osm_monaco&source=10&target=5000");
  await expect(page.locator("input[type=number]").first()).toHaveValue("10");
  await page.getByTestId("compare-run").click();
  await expect(page.getByText("Measured differences")).toBeVisible({ timeout: 90_000 });
  await expect(page.getByText("Reference agrees")).toBeVisible();
  await expect(page.locator("table.data").first()).toContainText("Benchmark runtime (median)");
});

test("unreachable destination is reported, with no ratio", async ({ page, request }) => {
  test.skip(!(await ready(request, "snap_roadnet_pa")), "snap_roadnet_pa is not prepared");
  await page.goto("/explore?dataset=snap_roadnet_pa");
  await expect(page.getByTestId("dataset-select")).toHaveValue("snap_roadnet_pa");
  await page.getByTestId("source-node").fill("0");
  await page.getByTestId("target-node").fill("12849");
  const id = await runAndGetId(page);
  await expect(page.getByText("Destination unreachable")).toBeVisible();
  const r = await (await request.get(`${API}/api/runs/${id}/result`)).json();
  expect(r.result.reachable).toBe(false);
  expect(r.correctness.approximation_ratio).toBeNull();
});

test("backend failure is surfaced, not a blank panel", async ({ page }) => {
  await page.goto("/explore?dataset=osm_monaco");
  await page.getByTestId("source-node").fill("10");
  await page.getByTestId("target-node").fill("20");
  await page.route("**/api/runs", (route) => route.abort("connectionrefused"));
  await page.getByTestId("run-button").click();
  await expect(page.locator(".notice.bad[role=alert]")).toContainText(/unavailable/i);
});

test("research and model pages render stored artifacts", async ({ page, request }) => {
  const results = await (await request.get(`${API}/api/research/results`)).json();
  test.skip(results.length === 0 || !(await ready(request, "dimacs_ny")), "no stored results / NY model here");
  await page.goto("/research");
  await expect(page.getByText("Paired comparison against")).toBeVisible();
  await page.goto("/models/dimacs_ny_static");
  await expect(page.getByText(/Training\/Evaluation split: DISJOINT/)).toBeVisible();
  await page.goto("/datasets/osm_monaco");
  await expect(page.getByText("Turn restrictions")).toBeVisible();
});
