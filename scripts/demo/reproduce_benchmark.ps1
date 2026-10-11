# =============================================================================
# reproduce_benchmark: a baseline-vs-optimized latency benchmark on this machine.
#
#   powershell -ExecutionPolicy Bypass -File scripts\demo\reproduce_benchmark.ps1 [options]
#
#   -Repetitions N   measured repetitions per query            (default 11)
#   -Warmup N        discarded warm-up repetitions              (default 3)
#   -CpuCore N       pin the benchmark to core N (default: the binary picks a
#                    core and avoids core 0; it reports [UNPINNED] if it cannot)
#   -Compare SPEC    configurations, "name[:w=N,ring=N,front=N][@label];..."
#   -Dataset ID,ID   datasets to run. Default: demo_road, plus dimacs_ny when
#                    datasets\processed\dimacs_ny.lpqgraph exists.
#
# Same behaviour as reproduce_benchmark.sh: lpq_benchmark --interleave (queries in
# the outer loop, configuration order shuffled per query with a fixed seed), every
# answer checked against the trusted reference search, and one NEW file per
# dataset under results\raw\local\<UTC time>-<dataset>.json. Raw results are
# immutable (the binary refuses to overwrite) and git-ignored.
#
# Timings are specific to this machine. Cloud and virtual-machine timings are
# development-stage evidence, not publication results.
# =============================================================================
param(
    [int]$Repetitions = 11,
    [int]$Warmup = 3,
    [int]$CpuCore = -1,
    # Baseline first, then the other classical queues, then the learned queue.
    # Which of these is fastest is exactly what this run measures.
    [string]$Compare = 'binary_heap@baseline;pairing_heap@pairing_heap;bucket_queue@bucket_queue;lpq_direct@lpq_direct',
    [string[]]$Dataset = @()
)
. "$PSScriptRoot/../setup/_common.ps1"
Set-Location $Root

if (-not (Test-EngineBuilt)) { Die 'the engine is not built. Run scripts\setup\setup.ps1 first.' }
$bench = Get-ToolPath 'lpq_benchmark'
$vpy = Get-VenvPython
if (-not $vpy) { Die 'no .venv found. Run scripts\setup\setup.ps1 first.' }

if ($Dataset.Count -eq 0) {
    $Dataset = @('demo_road')
    if (Test-Path (Join-Path $Root 'datasets/processed/dimacs_ny.lpqgraph')) { $Dataset += 'dimacs_ny' }
    else {
        Write-Host 'note: dimacs_ny is not prepared, so only the demo graph is benchmarked.'
        Write-Host '      To add the real New York road network (downloads ~6 MB):'
        Write-Host '        python scripts\data\prepare_datasets.py dimacs_ny'
    }
}

if ($Dataset -contains 'demo_road' -and -not (Test-Path (Join-Path $Root 'datasets/processed/demo_road.lpqgraph'))) {
    Say 'preparing the demo dataset'
    Invoke-Checked $vpy @('scripts/demo/prepare_demo.py')
}

$stamp = [DateTime]::UtcNow.ToString("yyyyMMdd'T'HHmmss'Z'")
$outDir = Join-Path $Root 'results/raw/local'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$written = @()

foreach ($ds in $Dataset) {
    $graph = Join-Path $Root "datasets/processed/$ds.lpqgraph"
    $meta = Join-Path $Root "datasets/metadata/$ds.json"
    if (-not (Test-Path $graph) -or -not (Test-Path $meta)) {
        Die "dataset '$ds' is not prepared: $graph. Run: python scripts\data\prepare_datasets.py $ds"
    }
    $manifest = Join-Path $Root "experiments/manifests/${ds}_queries_100.yaml"
    if (-not (Test-Path $manifest)) { $manifest = Join-Path $Root "experiments/manifests/${ds}_queries.yaml" }
    if (-not (Test-Path $manifest)) { Die "query manifest for '$ds' is missing. Run: python scripts\data\prepare_datasets.py $ds" }
    $out = Join-Path $outDir "$stamp-$ds.json"

    $benchArgs = @('--algorithm', 'dijkstra', '--dataset', $graph, '--queries-file', $manifest,
                   '--repetitions', "$Repetitions", '--warmup', "$Warmup", '--interleave',
                   '--compare', $Compare, '--experiment-id', "local-$stamp-$ds", '--output', $out)
    if ($CpuCore -ge 0) { $benchArgs += @('--cpu-core', "$CpuCore") }

    Say "benchmark: $ds   ($(Split-Path $manifest -Leaf), $Repetitions repetitions, $Warmup warm-up)"
    try { Invoke-Checked $bench $benchArgs } catch { Die "lpq_benchmark failed for $ds (see the output above). $_" }
    $written += $out
    Write-Host ''
    try { Invoke-Checked $vpy @('scripts/demo/summarize_results.py', $out) } catch { Warn "summarize_results.py reported a problem for ${out}: $_" }
}

Write-Host ''
Say 'done'
Write-Host '  Raw results (new files, never overwritten):'
foreach ($f in $written) { Write-Host "    $f" }
Write-Host @'

  How to read the table above: "median ns" is the median over queries of each
  query's median time; "q-MAD" is the spread across queries (workload
  heterogeneity, not noise); "noise" is the within-query relative MAD. Compare
  rows within one table only. Timings depend on this machine; if any row says
  [UNPINNED] the benchmark could not pin itself to a core and the numbers are
  noisier. See docs/local-setup.md ("Reading the benchmark output").
'@
exit 0
