# =============================================================================
# LPQ Atlas: full-stack images (spec section 38).
#
# Build context is the REPOSITORY ROOT (the engine, the research tooling and the
# UI all live there); compose.yaml at the root sets that up. Two final targets:
#
#   api   the C++ engine compiled from source + the FastAPI orchestration layer
#   web   the production Next.js build of the UI
#
# The engine is built on the same Debian release (bookworm) the API runs on, so
# the binaries link against the libstdc++ that is present at run time. Nothing
# from the host's build/ directory is used: a Windows build cannot run here, and
# an image that reused one would not prove that the engine builds from source.
# =============================================================================

# -----------------------------------------------------------------------------
# Engine: configure and build the release preset from a clean tree. Tests,
# microbenchmarks and fuzzers are not needed at run time (CI and
# scripts/build/linux_checks.sh cover them).
# -----------------------------------------------------------------------------
FROM python:3.13-slim-bookworm AS engine
RUN apt-get update && apt-get install --no-install-recommends -y \
        g++ cmake ninja-build git ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# .git is in the context so the engine embeds the commit it was built from
# (cmake/LpqBuildInfo.cmake); every result records it.
RUN git config --global --add safe.directory /src \
 && cmake --preset release -DLPQ_BUILD_TESTS=OFF -DLPQ_BUILD_BENCHMARKS=OFF -DLPQ_BUILD_FUZZERS=OFF \
 && cmake --build build/release \
 && ls build/release/bin

# -----------------------------------------------------------------------------
# API runtime.
# -----------------------------------------------------------------------------
FROM python:3.13-slim-bookworm AS api
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
# libexpat1: pyosmium's native module links it, and the slim image omits it;
# without it Prepare fails on every OSM dataset.
RUN apt-get update && apt-get install --no-install-recommends -y libstdc++6 libgomp1 libexpat1 curl \
    && rm -rf /var/lib/apt/lists/*
COPY python/requirements.txt /tmp/req-research.txt
COPY atlas/api/requirements.txt /tmp/req-api.txt
RUN pip install --no-cache-dir -r /tmp/req-research.txt -r /tmp/req-api.txt && rm /tmp/req-*.txt
WORKDIR /app
# The API derives every path from the repository root (atlas_api/config.py), so
# the image keeps the repository layout: sources, configs, manifests and docs
# are baked in; datasets, models and results are mounted by compose.yaml.
COPY . .
COPY --from=engine /src/build/release/bin /app/build/release/bin
RUN useradd --create-home --uid 10001 atlas && mkdir -p /data/atlas && chown -R atlas /data/atlas
USER atlas
ENV ATLAS_WORK_DIR=/data/atlas
EXPOSE 8000
HEALTHCHECK --interval=10s --timeout=5s --start-period=20s --retries=5 \
    CMD curl -sf http://127.0.0.1:8000/api/health || exit 1
CMD ["python", "-m", "uvicorn", "atlas_api.app:app", "--app-dir", "atlas/api", "--host", "0.0.0.0", "--port", "8000"]

# -----------------------------------------------------------------------------
# UI: production build. Next.js fixes rewrites at build time, so the address
# the UI proxies /api to is a build argument (the compose service name).
# -----------------------------------------------------------------------------
FROM node:22-bookworm-slim AS web-build
ARG ATLAS_API_URL=http://api:8000
ENV ATLAS_API_URL=${ATLAS_API_URL} NEXT_TELEMETRY_DISABLED=1
WORKDIR /web
COPY atlas/web/package.json atlas/web/package-lock.json ./
RUN npm ci
COPY atlas/web/ ./
RUN npm run build && npm prune --omit=dev

FROM node:22-bookworm-slim AS web
ENV NODE_ENV=production NEXT_TELEMETRY_DISABLED=1
WORKDIR /web
COPY --from=web-build --chown=node /web/package.json /web/next.config.mjs ./
COPY --from=web-build --chown=node /web/node_modules ./node_modules
COPY --from=web-build --chown=node /web/.next ./.next
USER node
EXPOSE 3000
CMD ["node_modules/.bin/next", "start", "-p", "3000", "-H", "0.0.0.0"]
