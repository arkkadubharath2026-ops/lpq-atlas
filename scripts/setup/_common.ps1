# =============================================================================
# Shared helpers for scripts/setup/*.ps1. Dot-sourced, never run directly:
#     . "$PSScriptRoot\_common.ps1"
#
# Mirrors scripts/setup/_common.sh so the two behave identically. The bash
# version reuses scripts/reproduce/common.sh; PowerShell cannot source that, so
# the few things it provides (repository root, build directory build/release,
# tool lookup with the .exe suffix) are restated here.
#
# Written for Windows PowerShell 5.1 and PowerShell 7. ASCII only.
# =============================================================================
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$script:SetupDir = $PSScriptRoot
$script:Root     = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$script:WebDir   = Join-Path $script:Root 'atlas/web'
$script:RunDir   = Join-Path $script:Root 'results/atlas/launcher'   # git-ignored
$script:BuildDir = Join-Path $script:Root 'build/release'
$script:BinDir   = Join-Path $script:BuildDir 'bin'
$script:HostName = '127.0.0.1'
$script:ApiPort  = if ($env:ATLAS_API_PORT) { [int]$env:ATLAS_API_PORT } else { 8000 }
$script:WebPort  = if ($env:ATLAS_WEB_PORT) { [int]$env:ATLAS_WEB_PORT } else { 3000 }

# $IsWindows exists only in PowerShell 7; Windows PowerShell 5.1 is Windows-only.
$script:OnWindows = $true
if (Test-Path variable:IsWindows) { $script:OnWindows = [bool]$IsWindows }
# npm.cmd, not npm: PowerShell prefers npm.ps1, which an execution policy can block.
$script:Npm = if ($script:OnWindows) { 'npm.cmd' } else { 'npm' }

# The preset's binaryDir is build/release and the API only searches
# build/release/bin (python/lpq_tools/utilities/paths.py).
if ($env:LPQ_BUILD_DIR) {
    throw "LPQ_BUILD_DIR is set. The setup scripts use the CMake preset (build/release), the only place the Atlas API looks. Remove it: Remove-Item Env:LPQ_BUILD_DIR"
}

$script:EngineTools = @('lpq_run', 'lpq_benchmark', 'lpq_convert', 'lpq_export', 'lpq_model_info')

# --- output ------------------------------------------------------------------
function Say([string]$Message)  { Write-Host "==> $Message" -ForegroundColor Cyan }
function Warn([string]$Message) { Write-Host "WARNING: $Message" -ForegroundColor Yellow }
function Die([string]$Message)  { Write-Host "FAILED: $Message" -ForegroundColor Red; exit 1 }
function Report([string]$Status, [string]$Name, [string]$Detail) {
    $color = switch ($Status) { 'ok' { 'Green' } 'FAIL' { 'Red' } default { 'Yellow' } }
    Write-Host ('  [{0,-4}] {1,-14} {2}' -f $Status, $Name, $Detail) -ForegroundColor $color
}

# --- running native programs ----------------------------------------------------
# Runs a program, lets its output through, and fails on a non-zero exit code.
# $ErrorActionPreference is relaxed for the call: Windows PowerShell 5.1 turns any
# stderr line of a native program into a terminating error under 'Stop'.
function Invoke-Checked {
    param([string]$Exe, [string[]]$Arguments = @(), [string]$WorkDir = $script:Root)
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    Push-Location $WorkDir
    try {
        & $Exe @Arguments
        $code = $LASTEXITCODE
    } finally {
        Pop-Location
        $ErrorActionPreference = $saved
    }
    if ($code -ne 0) { throw "$Exe $($Arguments -join ' ') failed with exit code $code" }
}

# Runs a program and returns @{ Code; Output } without throwing.
function Get-NativeOutput {
    param([string]$Exe, [string[]]$Arguments = @())
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $Exe @Arguments 2>&1 | Out-String
        return @{ Code = $LASTEXITCODE; Output = $out.Trim() }
    } catch {
        return @{ Code = -1; Output = "$_" }
    } finally {
        $ErrorActionPreference = $saved
    }
}

function Test-Command([string]$Name) { return [bool](Get-Command $Name -ErrorAction SilentlyContinue) }

function Get-Version([string]$Text) {
    if ($Text -match '(\d+\.\d+(\.\d+)?)') { return [version]$Matches[1] }
    return $null
}

# --- python ----------------------------------------------------------------------
function Get-VenvPython {
    foreach ($p in @((Join-Path $script:Root '.venv/Scripts/python.exe'), (Join-Path $script:Root '.venv/bin/python'))) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

# Finds Python 3.11-3.13 with the venv module. Returns @{ Path; Version } or
# @{ Problem } explaining what was found instead.
function Find-Python {
    $candidates = @(@('py', '-3.13'), @('py', '-3.12'), @('py', '-3.11'), @('python3.13'), @('python3.12'),
                    @('python3.11'), @('python3'), @('python'))
    if ($env:LPQ_SETUP_PYTHON) { $candidates = @(, @($env:LPQ_SETUP_PYTHON)) + $candidates }
    $seen = @(); $tooNew = @(); $tooOld = @(); $noVenv = @()
    foreach ($c in $candidates) {
        if (-not (Test-Command $c[0])) { continue }
        $rest = @($c | Select-Object -Skip 1)
        $r = Get-NativeOutput $c[0] ($rest + @('-c', 'import sys; print("%d.%d" % sys.version_info[:2])'))
        if ($r.Code -ne 0 -or $r.Output -notmatch '^3\.\d+$') { continue }   # e.g. the Microsoft Store stub
        $minor = [int]($r.Output.Split('.')[1])
        $label = "$($c -join ' ')=$($r.Output)"
        if ($minor -lt 11) { $tooOld += $label; continue }
        if ($minor -gt 13) { $tooNew += $label; continue }
        $v = Get-NativeOutput $c[0] ($rest + @('-c', 'import venv, ensurepip'))
        if ($v.Code -ne 0) { $noVenv += $label; continue }
        $exe = Get-NativeOutput $c[0] ($rest + @('-c', 'import sys; print(sys.executable)'))
        return @{ Path = $exe.Output; Version = $r.Output; Problem = $null }
    }
    if ($noVenv.Count) { $p = "found $($noVenv -join ', ') but its venv/ensurepip module is missing; reinstall Python with the standard options" }
    elseif ($tooNew.Count) {
        $p = "found $($tooNew -join ', ') only. python/requirements.txt pins numpy 2.2.4, pandas 2.3.1 and scipy 1.15.2, which have no wheels for Python 3.14+ (pip would try to compile them and normally fails). Install Python 3.13 (CI uses 3.13); 3.11 and 3.12 also work."
    }
    elseif ($tooOld.Count) { $p = "found $($tooOld -join ', ') but 3.11 or newer (3.13 recommended) is required" }
    else { $p = 'no Python 3 found (the Microsoft Store "python" alias does not count)' }
    return @{ Path = $null; Version = $null; Problem = $p }
}

# --- C++ compiler ------------------------------------------------------------------
function Test-Cxx20([string]$Exe, [string]$Kind) {
    $dir = Join-Path ([System.IO.Path]::GetTempPath()) ("lpq_probe_" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $dir | Out-Null
    try {
        $src = Join-Path $dir 'p.cpp'
        Set-Content -Path $src -Encoding ASCII -Value @(
            '#include <version>', '#include <concepts>',
            'template <typename T> concept A = requires(T a) { a + a; };',
            'static_assert(A<int>);', 'int main() { return 0; }')
        if ($Kind -eq 'msvc') { $args2 = @('/nologo', '/EHsc', '/std:c++20', '/c', $src, ('/Fo' + (Join-Path $dir 'p.obj'))) }
        else { $args2 = @('-std=c++20', '-c', $src, '-o', (Join-Path $dir 'p.o')) }
        return ((Get-NativeOutput $Exe $args2).Code -eq 0)
    } finally {
        Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    }
}

# Imports the Visual Studio developer environment (vcvars64.bat) into this
# process, so cl, link and the Windows SDK are found by CMake and Ninja.
function Import-VsDevEnv {
    if (-not ${env:ProgramFiles(x86)}) { return $false }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $false }
    $vs = (Get-NativeOutput $vswhere @('-latest', '-products', '*', '-requires',
            'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath')).Output
    if (-not $vs) { return $false }
    $bat = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $bat)) { return $false }
    # The leading space and the doubled quotes are the long-standing idiom for
    # running a .bat through cmd /c from PowerShell when the path has spaces.
    $tmp = [System.IO.Path]::GetTempFileName()
    try {
        cmd.exe /c " `"$bat`" > nul 2>&1 && set > `"$tmp`" "
        foreach ($line in (Get-Content $tmp)) {
            if ($line -match '^([^=]+)=(.*)$') {
                [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
            }
        }
    } finally {
        Remove-Item $tmp -ErrorAction SilentlyContinue
    }
    return $true
}

# Returns @{ Kind; Path; Cc; Desc } for the first acceptable compiler, or
# @{ Kind = $null; Tried } listing what was rejected. GCC (MinGW-w64, >= 12) is
# tried before MSVC (VS 2022, cl 19.3x) because that is what the project has been
# built with on Windows. An explicit $env:CXX is honoured exclusively.
function Find-Cxx {
    $tried = @()
    $gccs = @()
    if ($env:CXX) { $gccs = @($env:CXX) }
    else {
        $gccs += @(Get-Command g++ -All -ErrorAction SilentlyContinue | ForEach-Object { $_.Source })
        foreach ($d in @('C:\msys64\ucrt64\bin', 'C:\msys64\mingw64\bin')) {
            $p = "$d\g++.exe"
            if (Test-Path -LiteralPath $p) { $gccs += $p }
        }
    }
    foreach ($g in ($gccs | Select-Object -Unique)) {
        $r = Get-NativeOutput $g @('-dumpfullversion', '-dumpversion')
        $v = if ($r.Code -eq 0) { Get-Version $r.Output } else { $null }
        if (-not $v) { $tried += "$g (not GCC)"; continue }
        if ($v.Major -lt 12) { $tried += "$g (GCC $v, needs >= 12)"; continue }
        if (-not (Test-Cxx20 $g 'gcc')) { $tried += "$g (GCC $v, fails the C++20 probe)"; continue }
        $cc = $g -replace 'g\+\+(\.exe)?$', 'gcc$1'
        return @{ Kind = 'gcc'; Path = $g; Cc = $cc; Desc = "GCC $v ($g)"; Tried = $tried }
    }
    if ($env:CXX) { return @{ Kind = $null; Tried = $tried } }

    if (-not (Test-Command 'cl')) { [void](Import-VsDevEnv) }
    if (Test-Command 'cl') {
        $banner = (Get-NativeOutput 'cl' @()).Output
        $v = $null
        if ($banner -match 'Version (\d+\.\d+)') { $v = [version]$Matches[1] }
        if (-not $v) { $tried += 'cl (cannot read version)' }
        elseif ($v -lt [version]'19.30') { $tried += "cl $v (older than Visual Studio 2022)" }
        elseif (-not (Test-Cxx20 'cl' 'msvc')) { $tried += "cl $v (fails the C++20 probe)" }
        else { return @{ Kind = 'msvc'; Path = 'cl'; Cc = 'cl'; Desc = "MSVC $v ($((Get-Command cl).Source))"; Tried = $tried } }
    }
    return @{ Kind = $null; Tried = $tried }
}

function Get-Hint([string]$Key) {
    switch ($Key) {
        'compiler' { 'Either MSYS2 (https://www.msys2.org): pacman -S mingw-w64-ucrt-x86_64-gcc  (GCC >= 12; scripts find C:\msys64\ucrt64\bin by themselves)  -- or --  Visual Studio 2022 Build Tools: winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"' }
        'cmake'    { 'winget install Kitware.CMake   (needs >= 3.25; or: pacman -S mingw-w64-ucrt-x86_64-cmake)' }
        'ninja'    { 'winget install Ninja-build.Ninja   (or: pacman -S mingw-w64-ucrt-x86_64-ninja, or: pip install ninja)' }
        'python'   { 'winget install Python.Python.3.13' }
        'node'     { 'winget install OpenJS.NodeJS.LTS' }
        'git'      { 'winget install Git.Git' }
        default    { 'see docs/local-setup.md' }
    }
}

# --- engine / stack helpers ---------------------------------------------------------
function Get-ToolPath([string]$Name) {
    foreach ($candidate in @((Join-Path $script:BinDir "$Name.exe"), (Join-Path $script:BinDir $Name))) {
        if (Test-Path $candidate) { return $candidate }
    }
    return $null
}

function Test-EngineBuilt {
    foreach ($t in $script:EngineTools) { if (-not (Get-ToolPath $t)) { return $false } }
    return $true
}

function Test-PortInUse([int]$Port) {
    $c = New-Object System.Net.Sockets.TcpClient
    try {
        $iar = $c.BeginConnect($script:HostName, $Port, $null, $null)
        return ($iar.AsyncWaitHandle.WaitOne(500) -and $c.Connected)
    } catch { return $false } finally { $c.Close() }
}

function Test-HttpOk([string]$Url) {
    try {
        $r = Invoke-WebRequest -UseBasicParsing -Uri $Url -TimeoutSec 3
        return ($r.StatusCode -ge 200 -and $r.StatusCode -lt 300)
    } catch { return $false }
}

# next build bakes the API address into its rewrites, so a build made for one API
# port is wrong for another. The marker records which one this build is for.
function Ensure-WebBuilt {
    $marker = Join-Path $script:WebDir '.next/.atlas_api_url'
    $want = "http://$($script:HostName):$($script:ApiPort)"
    if ((Test-Path $marker) -and ((Get-Content $marker -Raw).Trim() -eq $want)) { return }
    Say "building the web UI (next build; API at $want)"
    $env:ATLAS_API_URL = $want
    Invoke-Checked $script:Npm @('run', 'build') $script:WebDir
    Set-Content -Path $marker -Value $want -NoNewline
}

# Process environment used by every Python subprocess.
$env:PYTHONPATH = (Join-Path $script:Root 'python') + $(if ($env:PYTHONPATH) { [System.IO.Path]::PathSeparator + $env:PYTHONPATH } else { '' })
