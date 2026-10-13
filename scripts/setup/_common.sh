#!/usr/bin/env bash
# =============================================================================
# Shared helpers for scripts/setup/*.sh. Sourced, never executed.
#
# Reuses scripts/reproduce/common.sh for the repository root, the build
# directory, the say/die helpers and tool_path/require_tool, so the setup
# scripts and the reproduction scripts agree on where binaries live
# (build/release/bin, the only place the Atlas API looks).
# =============================================================================
set -euo pipefail

SETUP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SETUP_DIR/../.." && pwd)"
WEB_DIR="$ROOT/atlas/web"
RUN_DIR="$ROOT/results/atlas/launcher" # git-ignored (results/atlas/)

# --- optional settings file ----------------------------------------------------
# A .env at the repository root is OPTIONAL (copy .env.example to .env to change a setting). Only ATLAS_*
# and LPQ_* keys are read, a variable that is already set in the environment wins, and the file is parsed
# as KEY=VALUE lines, never executed.
load_env_file() {
    local f="$ROOT/.env" line key val
    [ -f "$f" ] || return 0
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%$'\r'}"
        case "$line" in '' | '#'*) continue ;; esac
        case "$line" in *=*) ;; *) continue ;; esac
        key="${line%%=*}"
        key="${key//[[:space:]]/}"
        val="${line#*=}"
        case "$key" in ATLAS_* | LPQ_*) ;; *) continue ;; esac
        [[ "$key" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || continue
        [ -n "${!key+x}" ] && continue
        val="${val%\"}"
        val="${val#\"}"
        export "$key=$val"
    done < "$f"
}
load_env_file

API_PORT="${ATLAS_API_PORT:-8000}"
WEB_PORT="${ATLAS_WEB_PORT:-3000}"
HOST="127.0.0.1"

# The preset's binaryDir is build/release and the API searches
# build/release/bin (python/lpq_tools/utilities/paths.py), so a different build
# directory would build fine and then be invisible to the API.
if [ -n "${LPQ_BUILD_DIR:-}" ] && [ "$LPQ_BUILD_DIR" != "$ROOT/build/release" ]; then
    printf 'LPQ_BUILD_DIR is set to %s. The setup scripts use the CMake preset, whose build\n' "$LPQ_BUILD_DIR" >&2
    printf 'directory is build/release (the only place the Atlas API looks). Unset it.\n' >&2
    exit 1
fi
unset LPQ_BUILD_DIR

# --- OS ----------------------------------------------------------------------
case "$(uname -s)" in
    Linux*) OS=linux ;;
    Darwin*) OS=macos ;;
    MINGW* | MSYS* | CYGWIN*) OS=windows ;;
    *) OS=other ;;
esac

# --- the venv interpreter, if there is one ------------------------------------
venv_python() {
    if [ -x "$ROOT/.venv/bin/python" ]; then
        printf '%s' "$ROOT/.venv/bin/python"
    elif [ -x "$ROOT/.venv/Scripts/python.exe" ]; then
        printf '%s' "$ROOT/.venv/Scripts/python.exe"
    fi
}
if [ -n "$(venv_python)" ]; then
    export LPQ_PYTHON="$(venv_python)"
fi

# shellcheck source=../reproduce/common.sh
source "$ROOT/scripts/reproduce/common.sh"

warn() { printf "\033[1;33mWARNING:\033[0m %s\n" "$1" >&2; }
have() { command -v "$1" > /dev/null 2>&1; }

# One doctor line: status label, name, detail.
report() { printf "  [%-4s] %-14s %s\n" "$1" "$2" "$3"; }

# --- install hints -------------------------------------------------------------
apt_like() { [ -r /etc/os-release ] && grep -qiE '^(ID|ID_LIKE)=.*(debian|ubuntu)' /etc/os-release; }

hint() {
    case "$1:$OS" in
        compiler:linux)
            if apt_like; then echo "sudo apt install build-essential   (Ubuntu 22.04+ ships GCC 11+; else: sudo apt install g++-12)"; else echo "install GCC >= 11 or Clang >= 14 with your package manager (dnf install gcc-c++)"; fi ;;
        compiler:macos) echo "xcode-select --install   (or: brew install llvm)" ;;
        compiler:windows) echo "MSYS2 UCRT64: pacman -S mingw-w64-ucrt-x86_64-gcc (needs GCC >= 12), then put C:\\msys64\\ucrt64\\bin on PATH" ;;
        cmake:linux) if apt_like; then echo "sudo apt install cmake   (needs >= 3.25; if apt is older: pip install cmake, or https://apt.kitware.com)"; else echo "install CMake >= 3.25 (pip install cmake works everywhere)"; fi ;;
        cmake:macos) echo "brew install cmake" ;;
        cmake:windows) echo "winget install Kitware.CMake   (or: pacman -S mingw-w64-ucrt-x86_64-cmake)" ;;
        ninja:linux) if apt_like; then echo "sudo apt install ninja-build   (or: pip install ninja)"; else echo "install ninja (pip install ninja works everywhere)"; fi ;;
        ninja:macos) echo "brew install ninja" ;;
        ninja:windows) echo "winget install Ninja-build.Ninja   (or: pacman -S mingw-w64-ucrt-x86_64-ninja)" ;;
        python:linux) if apt_like; then echo "sudo apt install python3 python3-venv python3-pip   (3.11 to 3.13 required)"; else echo "install Python 3.11 to 3.13 including the venv module"; fi ;;
        python:macos) echo "brew install python@3.13" ;;
        python:windows) echo "winget install Python.Python.3.13" ;;
        node:linux) echo "Node >= 20: https://nodejs.org or nvm (apt's nodejs is usually too old):  curl -fsSL https://deb.nodesource.com/setup_22.x | sudo -E bash - && sudo apt install -y nodejs" ;;
        node:macos) echo "brew install node@22" ;;
        node:windows) echo "winget install OpenJS.NodeJS.LTS" ;;
        git:linux) echo "sudo apt install git" ;;
        git:macos) echo "xcode-select --install" ;;
        git:windows) echo "winget install Git.Git" ;;
        *) echo "see docs/local-setup.md" ;;
    esac
}

# --- C++20 compiler -------------------------------------------------------------
# "gcc 13" / "clang 18" for a compiler, or non-zero if it is not either.
compiler_family() {
    local defs clang gnuc
    defs="$("$1" -dM -E -x c++ - < /dev/null 2> /dev/null)" || return 1
    clang="$(printf '%s\n' "$defs" | awk '$2=="__clang_major__"{print $3}')"
    gnuc="$(printf '%s\n' "$defs" | awk '$2=="__GNUC__"{print $3}')"
    if [ -n "$clang" ]; then
        echo "clang $clang"
    elif [ -n "$gnuc" ]; then
        echo "gcc $gnuc"
    else
        return 1
    fi
}

# Probes that the compiler really accepts C++20 (concepts, <version>).
cxx20_probe() {
    local dir rc=0
    dir="$(mktemp -d)"
    printf '#include <version>\n#include <concepts>\ntemplate <typename T> concept A = requires(T a) { a + a; };\nstatic_assert(A<int>);\nint main() { return 0; }\n' > "$dir/p.cpp"
    "$1" -std=c++20 -c "$dir/p.cpp" -o "$dir/p.o" > /dev/null 2>&1 || rc=1
    rm -rf "$dir"
    return $rc
}

# Sets CXX_BIN, CXX_DESC (success) and CXX_TRIED (a list of what was rejected).
# GCC >= 11 / Clang >= 14 (MinGW-w64 GCC >= 12 on Windows). An explicit $CXX is
# honoured exclusively: it is the user's choice, so no other compiler is tried.
find_cxx() {
    local candidates c fam major min_gcc=11
    [ "$OS" = windows ] && min_gcc=12
    CXX_BIN="" CXX_DESC="" CXX_TRIED=""
    if [ -n "${CXX:-}" ]; then
        candidates="$CXX"
    else
        candidates="g++ clang++ c++ g++-14 g++-13 g++-12 g++-11 clang++-19 clang++-18 clang++-17 clang++-16 clang++-15 clang++-14"
    fi
    for c in $candidates; do
        have "$c" || continue
        fam="$(compiler_family "$c")" || { CXX_TRIED="$CXX_TRIED$c (not GCC or Clang); "; continue; }
        major="${fam#* }"
        if { [ "${fam%% *}" = gcc ] && [ "$major" -lt "$min_gcc" ]; } || { [ "${fam%% *}" = clang ] && [ "$major" -lt 14 ]; }; then
            CXX_TRIED="$CXX_TRIED$c (${fam}, too old); "
            continue
        fi
        cxx20_probe "$c" || { CXX_TRIED="$CXX_TRIED$c (${fam}, fails the C++20 probe); "; continue; }
        CXX_BIN="$(command -v "$c")"
        CXX_DESC="$fam"
        return 0
    done
    return 1
}

# The C compiler that goes with CXX_BIN (CMake wants both).
c_compiler_for() {
    local cc
    cc="$(printf '%s' "$1" | sed -e 's/clang++/clang/' -e 's/g++/gcc/' -e 's/c++$/cc/')"
    if have "$cc"; then printf '%s' "$cc"; fi
}

# --- Python 3.11 to 3.13 ------------------------------------------------------
# Sets PY_BIN, PY_DESC, or PY_PROBLEM explaining what was found instead.
find_python() {
    local c v maj min seen="" too_new="" too_old="" no_venv=""
    PY_BIN="" PY_DESC="" PY_PROBLEM=""
    for c in ${LPQ_SETUP_PYTHON:-} python3.13 python3.12 python3.11 python3 python; do
        have "$c" || continue
        v="$("$c" -c 'import sys; print("%d.%d" % sys.version_info[:2])' 2> /dev/null)" || continue
        maj="${v%%.*}" min="${v#*.}"
        seen="$seen$c=$v "
        if [ "$maj" -ne 3 ] || [ "$min" -lt 11 ]; then too_old="$too_old$c=$v "; continue; fi
        if [ "$min" -gt 13 ]; then too_new="$too_new$c=$v "; continue; fi
        if ! "$c" -c 'import venv, ensurepip' > /dev/null 2>&1; then no_venv="$no_venv$c=$v "; continue; fi
        PY_BIN="$(command -v "$c")"
        PY_DESC="$v"
        return 0
    done
    if [ -n "$no_venv" ]; then
        PY_PROBLEM="found ${no_venv}but its venv/ensurepip module is missing (Debian/Ubuntu: sudo apt install python3-venv)"
    elif [ -n "$too_new" ]; then
        PY_PROBLEM="found ${too_new}only. python/requirements.txt pins numpy 2.2.4, pandas 2.3.1 and scipy 1.15.2, which have no wheels for Python 3.14+ (pip would try to compile them and normally fails). Install Python 3.13 (CI uses 3.13); 3.11 and 3.12 also work."
    elif [ -n "$too_old" ]; then
        PY_PROBLEM="found ${too_old}but 3.11 or newer (3.13 recommended) is required"
    else
        PY_PROBLEM="no python3 on PATH"
    fi
    return 1
}

version_ge() { # version_ge 3.28.3 3.24  -> true when $1 >= $2
    [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" = "$2" ]
}

# --- running the stack -----------------------------------------------------------
# True when something already listens on host:port. Uses Python rather than
# curl/nc/ss, which differ per OS; the venv interpreter exists before this runs.
port_in_use() {
    "$LPQ_PYTHON" -c "import socket,sys; s=socket.socket(); s.settimeout(0.5); sys.exit(0 if s.connect_ex(('$HOST', $1))==0 else 1)"
}

http_ok() { # http_ok URL -> 0 when it answers 2xx within 3 s
    "$LPQ_PYTHON" -c "import sys,urllib.request; urllib.request.urlopen(sys.argv[1], timeout=3).read(1)" "$1" > /dev/null 2>&1
}

# next build bakes the API address into its rewrites, so a build made for one API
# port is wrong for another. The marker records which one this build is for.
ensure_web_built() {
    local marker="$WEB_DIR/.next/.atlas_api_url" want="http://$HOST:$API_PORT"
    if [ -f "$marker" ] && [ "$(cat "$marker")" = "$want" ]; then return 0; fi
    say "building the web UI (next build; API at $want)"
    (cd "$WEB_DIR" && ATLAS_API_URL="$want" npm run build) || die "the web build failed (see the output above)"
    printf '%s' "$want" > "$marker"
}

ENGINE_TOOLS=(lpq_run lpq_benchmark lpq_convert lpq_export lpq_model_info)

engine_built() {
    local t
    for t in "${ENGINE_TOOLS[@]}"; do
        [ -x "$BIN_DIR/$t" ] || [ -x "$BIN_DIR/$t.exe" ] || return 1
    done
}
