# =============================================================================
# run: start LPQ Atlas (FastAPI on :8000, Next.js on :3000) and wait until both
# answer. Runs setup first when this clone has not been set up yet.
#
#   powershell -ExecutionPolicy Bypass -File scripts\setup\run.ps1 [-Test] [-Dev]
#
#   -Test   run scripts\setup\test.ps1 before launching (stops if it fails)
#   -Dev    serve the UI with `next dev` instead of a production build
#
# Ports: ATLAS_API_PORT (default 8000), ATLAS_WEB_PORT (default 3000).
# Press Ctrl+C to stop both. Logs: results\atlas\launcher\{api,web}.{out,err}.log
# =============================================================================
param([switch]$Test, [switch]$Dev)
. "$PSScriptRoot/_common.ps1"
Set-Location $Root

# --- set up on first use ---------------------------------------------------------------
$demoGraph = Join-Path $Root 'datasets/processed/demo_road.lpqgraph'
if (-not (Get-VenvPython) -or -not (Test-Path (Join-Path $WebDir 'node_modules')) -or
    -not (Test-EngineBuilt) -or -not (Test-Path $demoGraph)) {
    Say 'this clone is not set up yet; running setup first'
    if ($Dev) { & "$PSScriptRoot/setup.ps1" -SkipWeb } else { & "$PSScriptRoot/setup.ps1" }
    if ($LASTEXITCODE -ne 0) { exit 1 }
}
$vpy = Get-VenvPython

if ($Test) {
    & "$PSScriptRoot/test.ps1"
    if ($LASTEXITCODE -ne 0) { exit 1 }
}

# --- ports -----------------------------------------------------------------------------
foreach ($p in @($ApiPort, $WebPort)) {
    if (Test-PortInUse $p) {
        Die "port $p is already in use. Stop whatever is using it, or choose other ports:`n       `$env:ATLAS_API_PORT=8001; `$env:ATLAS_WEB_PORT=3001; scripts\setup\run.ps1"
    }
}

New-Item -ItemType Directory -Force -Path $RunDir | Out-Null
if (-not $Dev) { Ensure-WebBuilt }

function Quote([string]$s) { return '"' + $s + '"' }
# -WindowStyle exists only on Windows; the servers should not open console windows.
$hidden = @{}
if ($OnWindows) { $hidden['WindowStyle'] = 'Hidden' }
$procs = @()
$exitCode = 0
try {
    # --- API ---------------------------------------------------------------------------
    Say "starting the API on http://${HostName}:$ApiPort"
    $api = Start-Process @hidden -FilePath $vpy -PassThru -WorkingDirectory $Root `
        -RedirectStandardOutput (Join-Path $RunDir 'api.out.log') -RedirectStandardError (Join-Path $RunDir 'api.err.log') `
        -ArgumentList @('-m', 'uvicorn', 'atlas_api.app:app', '--app-dir', (Quote (Join-Path $Root 'atlas/api')),
                        '--host', $HostName, '--port', "$ApiPort")
    $procs += $api
    $deadline = (Get-Date).AddSeconds(60)
    while (-not (Test-HttpOk "http://${HostName}:$ApiPort/api/health")) {
        if ($api.HasExited) {
            Get-Content (Join-Path $RunDir 'api.err.log') -Tail 20 | Write-Host
            Die "the API exited during start-up (logs: $RunDir)"
        }
        if ((Get-Date) -gt $deadline) { Die "the API did not answer within 60 s (logs: $RunDir)" }
        Start-Sleep -Seconds 1
    }

    # --- Web ---------------------------------------------------------------------------
    $mode = if ($Dev) { 'dev' } else { 'start' }
    Say "starting the web UI (next $mode) on http://${HostName}:$WebPort"
    $env:ATLAS_API_URL = "http://${HostName}:$ApiPort"
    $nextCli = Join-Path $WebDir 'node_modules/next/dist/bin/next'
    $web = Start-Process @hidden -FilePath 'node' -PassThru -WorkingDirectory $WebDir `
        -RedirectStandardOutput (Join-Path $RunDir 'web.out.log') -RedirectStandardError (Join-Path $RunDir 'web.err.log') `
        -ArgumentList @((Quote $nextCli), $mode, '-p', "$WebPort", '-H', $HostName)
    $procs += $web
    $deadline = (Get-Date).AddSeconds(120)
    while (-not (Test-HttpOk "http://${HostName}:$WebPort/")) {
        if ($web.HasExited) {
            Get-Content (Join-Path $RunDir 'web.err.log') -Tail 20 | Write-Host
            Die "the web UI exited during start-up (logs: $RunDir)"
        }
        if ((Get-Date) -gt $deadline) { Die "the web UI did not answer within 120 s (logs: $RunDir)" }
        Start-Sleep -Seconds 1
    }

    Write-Host ''
    Write-Host '  LPQ Atlas is running.' -ForegroundColor Green
    Write-Host ''
    Write-Host "    Open        http://${HostName}:$WebPort"
    Write-Host "    API         http://${HostName}:$ApiPort/api/health   (docs: http://${HostName}:$ApiPort/docs)"
    Write-Host '    Dataset     demo_road (synthetic, 19,600 nodes) is ready; pick it on the Explore page'
    Write-Host "    Logs        $RunDir"
    Write-Host ''
    Write-Host '  Press Ctrl+C to stop.'

    # Exit as soon as either server dies, so a crash is not mistaken for "running".
    while (-not $api.HasExited -and -not $web.HasExited) { Start-Sleep -Seconds 2 }
    Write-Host "a server process exited unexpectedly; see $RunDir" -ForegroundColor Red
    $exitCode = 1
} finally {
    Write-Host ''
    Say 'stopping'
    foreach ($p in $procs) {
        if (-not $p.HasExited) {
            # taskkill /T also ends Next's child processes.
            if ($OnWindows) { & taskkill.exe /PID $p.Id /T /F > $null 2>&1 } else { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
        }
    }
}
exit $exitCode
