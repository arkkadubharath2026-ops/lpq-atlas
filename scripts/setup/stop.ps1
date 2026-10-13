# =============================================================================
# stop: stop a LPQ Atlas started by scripts\setup\run.ps1, from any terminal.
#
#   powershell -ExecutionPolicy Bypass -File scripts\setup\stop.ps1
#
# Ctrl+C in the run.ps1 window is the usual way to stop. This is for when that window is not at hand. It reads
# the process id run.ps1 wrote to results\atlas\launcher\run.pid, checks that it really is run.ps1, and ends
# that process together with the API and web server it started. It never ends a process it cannot identify.
# =============================================================================
. "$PSScriptRoot/_common.ps1"

$pidFile = Join-Path $RunDir 'run.pid'
if (-not (Test-Path $pidFile)) {
    Write-Host "no launcher is recorded as running (no $pidFile)."
    foreach ($p in @($ApiPort, $WebPort)) {
        if (Test-PortInUse $p) { Warn "something is listening on port $p, but not a launcher started by run.ps1; it was left alone." }
    }
    exit 0
}
$launcherPid = [int]((Get-Content $pidFile -Raw).Trim())
$proc = Get-Process -Id $launcherPid -ErrorAction SilentlyContinue
if (-not $proc) {
    Remove-Item $pidFile -Force
    Write-Host "the recorded launcher (pid $launcherPid) is not running; removed the stale pid file."
    exit 0
}
$cmd = ''
if ($OnWindows) {
    $ci = Get-CimInstance Win32_Process -Filter "ProcessId=$launcherPid" -ErrorAction SilentlyContinue
    if ($ci) { $cmd = [string]$ci.CommandLine }
} elseif (Test-Path "/proc/$launcherPid/cmdline") {
    $cmd = (Get-Content "/proc/$launcherPid/cmdline" -Raw) -replace "`0", ' '
}
if ($cmd -notmatch 'run\.ps1') {
    Die "pid $launcherPid is not scripts\setup\run.ps1 ($cmd), so it was NOT stopped. Remove $pidFile if it is stale."
}
Say "stopping LPQ Atlas (launcher pid $launcherPid)"
# taskkill /T ends the launcher's child processes too; the launcher's own cleanup does not run when it is ended.
if ($OnWindows) { & taskkill.exe /PID $launcherPid /T /F > $null 2>&1 } else {
    & pkill -P $launcherPid 2> $null
    Stop-Process -Id $launcherPid -Force -ErrorAction SilentlyContinue
}
Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
Write-Host 'stopped.'
