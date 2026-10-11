# =============================================================================
# setup: install dependencies, build the engine, prepare the demo dataset.
#
#   powershell -ExecutionPolicy Bypass -File scripts\setup\setup.ps1 [-NoTests] [-SkipWeb] [-Force]
#
#   -NoTests   do not build the C++ tests (skips the googletest download, so
#              this works offline; scripts\setup\test.ps1 then cannot run ctest)
#   -SkipWeb   do not run `next build` (run.ps1 builds it on demand, or -Dev)
#   -Force     redo steps that look finished (npm ci, demo conversion)
#
# Idempotent. Steps: doctor; .venv + pip install; npm ci; cmake --preset release
# and build; demo dataset; next build.
# Environment: LPQ_JOBS (build parallelism), LPQ_USE_SYSTEM_GTEST=1, CXX / CC
# (compiler override), LPQ_SETUP_PYTHON (interpreter).
# =============================================================================
param([switch]$NoTests, [switch]$SkipWeb, [switch]$Force)
. "$PSScriptRoot/_common.ps1"
Set-Location $Root

& "$PSScriptRoot/doctor.ps1"
if ($LASTEXITCODE -ne 0) { exit 1 }

$cxx = Find-Cxx
if (-not $cxx.Kind) { Die 'no C++20 compiler (doctor should have caught this)' }
$py = Find-Python
if (-not $py.Path) { Die $py.Problem }

# --- Python environment ------------------------------------------------------------
Say 'Python environment (.venv)'
if (-not (Get-VenvPython)) { Invoke-Checked $py.Path @('-m', 'venv', (Join-Path $Root '.venv')) }
$vpy = Get-VenvPython
$minor = [int](Get-NativeOutput $vpy @('-c', 'import sys; print(sys.version_info[1])')).Output
if ($minor -lt 11 -or $minor -gt 13) { Die ".venv uses Python 3.$minor, outside 3.11-3.13. Delete .venv and re-run." }
Invoke-Checked $vpy @('-m', 'pip', 'install', '--disable-pip-version-check', '-q', '--upgrade', 'pip')
try {
    Invoke-Checked $vpy @('-m', 'pip', 'install', '--disable-pip-version-check',
        '-r', 'python/requirements-dev.txt', '-r', 'atlas/api/requirements.txt')
} catch { Die "pip install failed. If a package could not be built, check the Python version (3.13 recommended). $_" }

# --- Web dependencies --------------------------------------------------------------
Say 'Web dependencies (npm ci in atlas/web)'
$lockInstalled = Join-Path $WebDir 'node_modules/.package-lock.json'
$lockSource = Join-Path $WebDir 'package-lock.json'
if (-not $Force -and (Test-Path $lockInstalled) -and ([System.IO.File]::GetLastWriteTime($lockInstalled) -gt [System.IO.File]::GetLastWriteTime($lockSource))) {
    Write-Host '    node_modules is current'
} else {
    Invoke-Checked $Npm @('ci') $WebDir
}

# --- Engine ------------------------------------------------------------------------
$tests = if ($NoTests) { 'OFF' } else { 'ON' }
Say "Engine: CMake preset 'release' (tests: $tests), compiler $($cxx.Desc)"
$env:CXX = $cxx.Path
$env:CC = $cxx.Cc
if ($cxx.Kind -eq 'gcc') {
    # Keep the compiler's own directory first on PATH: an older MinGW earlier on
    # PATH would otherwise supply the assembler, linker and runtime DLLs.
    $env:PATH = (Split-Path $cxx.Path) + ';' + $env:PATH
}
$cmakeArgs = @('-S', $Root, '--preset', 'release', "-DLPQ_BUILD_TESTS=$tests")
if ($env:LPQ_USE_SYSTEM_GTEST -eq '1') { $cmakeArgs += '-DLPQ_USE_SYSTEM_GTEST=ON' }
try { Invoke-Checked 'cmake' $cmakeArgs } catch { Die "cmake configure failed. $_" }
$buildArgs = @('--build', $BuildDir)
if ($env:LPQ_JOBS) { $buildArgs += @('-j', $env:LPQ_JOBS) }
try { Invoke-Checked 'cmake' $buildArgs } catch { Die "the engine build failed. $_" }
foreach ($t in $EngineTools) {
    if (-not (Get-ToolPath $t)) { Die "$t is not built. Run: cmake --build $BuildDir --target $t" }
}
Write-Host "    tools in ${BinDir}: $($EngineTools -join ' ')"

# --- Demo dataset (offline) ------------------------------------------------------------
Say 'Demo dataset (demo_road, committed source files, no network)'
$prepArgs = @('scripts/demo/prepare_demo.py')
if ($Force) { $prepArgs += '--force' }
Invoke-Checked $vpy $prepArgs

# --- Web build -------------------------------------------------------------------------
if (-not $SkipWeb) { Ensure-WebBuilt }

Say 'setup complete. Next: scripts\setup\run.ps1   (add -Test to run the test suites first)'
