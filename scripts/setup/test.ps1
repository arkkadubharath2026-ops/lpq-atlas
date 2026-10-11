# =============================================================================
# test: run every test suite and report a summary.
#
#   powershell -ExecutionPolicy Bypass -File scripts\setup\test.ps1 [cpp] [python] [api] [web]
#
#   cpp      ctest --preset release-fast   (the C++ suite minus the 'slow' label;
#                                           the full suite: ctest --preset release)
#   python   pytest in python\             (dataset pipeline, statistics, training)
#   api      pytest in atlas\api\          (API <-> engine contract tests)
#   web      npm run lint, typecheck, test in atlas\web
#
# Default: all four. Every selected suite runs even if an earlier one fails; the
# exit status is non-zero if any failed. Needs setup first (scripts\setup\setup.ps1).
# =============================================================================
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Suites)
. "$PSScriptRoot/_common.ps1"
Set-Location $Root

if (-not $Suites -or $Suites.Count -eq 0) { $Suites = @('cpp', 'python', 'api', 'web') }
$vpy = Get-VenvPython
if (-not $vpy) { Die 'no .venv found. Run scripts\setup\setup.ps1 first.' }
if (-not (Test-EngineBuilt)) { Die 'the engine is not built. Run scripts\setup\setup.ps1 first.' }

$results = @()
function Invoke-Suite([string]$Name, [string]$Exe, [string[]]$Arguments, [string]$WorkDir) {
    Say "tests: $Name"
    try { Invoke-Checked $Exe $Arguments $WorkDir; $ok = $true }
    catch { Write-Host "$_" -ForegroundColor Red; $ok = $false }
    $script:results += [pscustomobject]@{ Name = $Name; Passed = $ok }
}

foreach ($s in $Suites) {
    switch ($s) {
        'cpp'    { Invoke-Suite 'C++ (ctest release-fast)' 'ctest' @('--preset', 'release-fast') $Root }
        'python' { Invoke-Suite 'python/ pytest' $vpy @('-m', 'pytest', '-q') (Join-Path $Root 'python') }
        'api'    { Invoke-Suite 'atlas/api pytest' $vpy @('-m', 'pytest', '-q') (Join-Path $Root 'atlas/api') }
        'web' {
            Invoke-Suite 'web lint' $Npm @('run', 'lint') $WebDir
            Invoke-Suite 'web typecheck' $Npm @('run', 'typecheck') $WebDir
            Invoke-Suite 'web unit tests' $Npm @('test') $WebDir
        }
        default { Die "unknown suite '$s' (cpp, python, api, web)" }
    }
}

Write-Host ''
Say 'summary'
$failed = $false
foreach ($r in $results) {
    if ($r.Passed) { $label = 'PASS' } else { $label = 'FAIL'; $failed = $true }
    Write-Host ('  {0,-6} {1}' -f $label, $r.Name)
}
if ($failed) { Die 'one or more test suites failed (output above)' }
Say 'all selected suites passed'
exit 0
