# =============================================================================
# doctor: check that this machine can build and run LPQ Atlas (Windows).
#
#   powershell -ExecutionPolicy Bypass -File scripts\setup\doctor.ps1
#
# Same checks as doctor.sh: git + a .git directory, a C++20 compiler (MinGW-w64
# GCC >= 12 or MSVC 2022), CMake >= 3.25, Ninja, Python 3.11-3.13, Node >= 20 and
# npm. Essential problems make the exit status non-zero; nothing is installed.
# =============================================================================
. "$PSScriptRoot/_common.ps1"

$failures = 0
function Fail([string]$Name, [string]$Detail, [string]$HintKey) {
    Report 'FAIL' $Name $Detail
    Write-Host ("           -> " + (Get-Hint $HintKey))
    $script:failures++
}

Say "LPQ Atlas doctor (Windows PowerShell)"

# --- git -----------------------------------------------------------------------
if (-not (Test-Command 'git')) { Fail 'git' 'not found' 'git' }
elseif (-not (Test-Path (Join-Path $Root '.git'))) {
    Fail 'git' "found, but $Root is not a git checkout (a downloaded zip has no .git)" 'git'
    Write-Host '           -> lpq_benchmark refuses to run without git provenance. Use: git clone <repo-url>'
} else { Report 'ok' 'git' ((Get-NativeOutput 'git' @('--version')).Output -replace 'git version ', '') }

# --- compiler --------------------------------------------------------------------
$cxx = Find-Cxx
if ($cxx.Kind) { Report 'ok' 'C++ compiler' "$($cxx.Desc), C++20 probe passed" }
else {
    $rej = if ($cxx.Tried.Count) { $cxx.Tried -join '; ' } else { 'none found' }
    Fail 'C++ compiler' "no usable C++20 compiler. Rejected: $rej" 'compiler'
    if ($env:CXX) { Write-Host "           -> `$env:CXX is set to '$($env:CXX)' and is used exclusively; clear it to auto-detect" }
}

# --- cmake / ninja ---------------------------------------------------------------
if (Test-Command 'cmake') {
    $v = Get-Version (Get-NativeOutput 'cmake' @('--version')).Output
    if ($v -and $v -ge [version]'3.25') { Report 'ok' 'CMake' "$v" }
    else { Fail 'CMake' "$v is older than the required 3.25 (the presets use schema version 6)" 'cmake' }
} else { Fail 'CMake' 'not found' 'cmake' }
if (Test-Command 'ninja') { Report 'ok' 'Ninja' (Get-NativeOutput 'ninja' @('--version')).Output }
else { Fail 'Ninja' 'not found' 'ninja' }

# --- python ----------------------------------------------------------------------
$py = Find-Python
if ($py.Path) {
    Report 'ok' 'Python' "$($py.Version) ($($py.Path))"
    if ($py.Version -ne '3.13') { Report 'note' 'Python' "CI uses 3.13; $($py.Version) should work but is less tested" }
} else { Fail 'Python' $py.Problem 'python' }

# --- node / npm ------------------------------------------------------------------
if (Test-Command 'node') {
    $nv = Get-Version (Get-NativeOutput 'node' @('--version')).Output
    if ($nv -and $nv.Major -ge 20) { Report 'ok' 'Node' "$nv" } else { Fail 'Node' "$nv is older than the required 20" 'node' }
} else { Fail 'Node' 'not found' 'node' }
if (Test-Command $Npm) { Report 'ok' 'npm' (Get-NativeOutput $Npm @('--version')).Output }
else { Fail 'npm' 'not found (installed with Node)' 'node' }

# --- warnings --------------------------------------------------------------------
if ($OnWindows -and $Root.Length -gt 60) {
    Warn "the repository path is $($Root.Length) characters long. The build can fail past 260-character paths (googletest's FetchContent stamp files); clone to a short path such as C:\lpq."
}

Write-Host ''
if ($failures -gt 0) {
    Die "$failures essential requirement(s) missing. Fix the lines marked FAIL, then re-run scripts\setup\doctor.ps1. Details: docs/local-setup.md"
}
Say 'all essential requirements are present. Next: scripts\setup\run.ps1 -Test'
exit 0
