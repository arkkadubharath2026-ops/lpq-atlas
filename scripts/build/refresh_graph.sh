#!/usr/bin/env bash
# Rebuild the Graphify knowledge graph for this repository.
#
# Graphify indexes the C++/Python sources into graphify-out/graph.json so that
# agents and contributors can query architecture without reading every file.
# Run this after each milestone (or let the git post-commit hook run it).
#
# Usage:
#   scripts/build/refresh_graph.sh            # incremental AST re-extract (no LLM, no API key)
#   scripts/build/refresh_graph.sh --full     # full re-scan, including docs (needs an LLM backend)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

if ! command -v graphify >/dev/null 2>&1; then
    echo "error: 'graphify' not found on PATH." >&2
    echo "       Install with: python -m pip install --user graphifyy" >&2
    echo "       Then add the Python user Scripts/bin directory to PATH." >&2
    exit 1
fi

if [[ "${1:-}" == "--full" ]]; then
    # Full extraction: AST for code + semantic extraction for PRD.md/ESD.md/docs.
    # Requires an LLM backend (see: graphify extract --backend ...).
    echo "[refresh_graph] full extraction (code + docs)"
    graphify extract . --force
else
    # Code-only: local tree-sitter AST parsing. Deterministic, offline, no API key.
    echo "[refresh_graph] code-only extraction (local AST, offline)"
    graphify extract . --code-only
fi

echo "[refresh_graph] graph written to graphify-out/"
