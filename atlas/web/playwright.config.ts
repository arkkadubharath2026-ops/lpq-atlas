import { defineConfig } from "@playwright/test";

// Expects the API on :8000 (real engine, real datasets) and the web app on
// :3000; see atlas/README.md. Nothing is mocked except the one test that
// deliberately simulates a backend outage.
export default defineConfig({
  testDir: "e2e",
  timeout: 120_000,
  expect: { timeout: 60_000 },
  fullyParallel: false,
  workers: 1,
  reporter: [["list"]],
  use: {
    baseURL: process.env.ATLAS_WEB_URL ?? "http://127.0.0.1:3000",
    viewport: { width: 1500, height: 950 },
    trace: "retain-on-failure",
    // Optional: use an already-installed Chromium instead of the one Playwright pins
    // (e.g. a sandbox whose preinstalled browser build differs). Unset in CI.
    launchOptions: process.env.ATLAS_CHROMIUM_PATH ? { executablePath: process.env.ATLAS_CHROMIUM_PATH } : {},
  },
});
