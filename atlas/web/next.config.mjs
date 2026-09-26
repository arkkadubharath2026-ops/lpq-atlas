// The UI talks to the FastAPI orchestration layer through a same-origin
// rewrite, so the browser never needs CORS and the API address is one setting.
const API = process.env.ATLAS_API_URL ?? "http://127.0.0.1:8000";

/** @type {import('next').NextConfig} */
const nextConfig = {
  reactStrictMode: true,
  agentRules: false,
  // The dev server serves its bundles only to allowed origins; 127.0.0.1 is how
  // the Playwright suite and the API address the app locally.
  allowedDevOrigins: ["127.0.0.1"],
  async rewrites() {
    return [{ source: "/api/:path*", destination: `${API}/api/:path*` }];
  },
};
export default nextConfig;
