<#
.SYNOPSIS
    Rebuild the Graphify knowledge graph for this repository (Windows).
.DESCRIPTION
    Graphify indexes the C++/Python sources into graphify-out/graph.json so that
    agents and contributors can query architecture without reading every file.
    Run after each milestone, or let the git post-commit hook run it.
.PARAMETER Full
    Perform a full re-scan including documentation (requires an LLM backend).
#>
param([switch]$Full)

$ErrorActionPreference = 'Stop'
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $repoRoot

if (-not (Get-Command graphify -ErrorAction SilentlyContinue)) {
    Write-Error @'
'graphify' not found on PATH.
Install with: python -m pip install --user graphifyy
Then add the Python user Scripts directory to PATH.
'@
}

if ($Full) {
    Write-Host '[refresh_graph] full extraction (code + docs)'
    graphify extract . --force
} else {
    Write-Host '[refresh_graph] code-only extraction (local AST, offline)'
    graphify extract . --code-only
}

Write-Host '[refresh_graph] graph written to graphify-out/'
