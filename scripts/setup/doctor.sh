#!/usr/bin/env bash
# =============================================================================
# doctor: check that this machine can build and run LPQ Atlas.
#
#   scripts/setup/doctor.sh
#
# Checks (essential ones make the exit status non-zero):
#   git + a .git directory   the benchmark refuses to run on a binary built
#                            without git provenance (ESD.md section 40)
#   C++20 compiler           GCC >= 11, Clang >= 14 (MinGW-w64 GCC >= 12)
#   CMake >= 3.25, Ninja     CMakePresets.json uses preset schema version 6, which
#                            CMake 3.24 rejects (CMakeLists.txt itself says 3.24)
#   Python 3.11 to 3.13      the pinned requirements have no wheels for 3.14+
#   Node >= 20, npm          the web UI
# Non-essential problems (network for the googletest download, a deep path on
# Windows) are warnings. Nothing is installed or modified.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

failures=0
fail() { # fail NAME DETAIL HINT-KEY
    report FAIL "$1" "$2"
    printf "           -> %s\n" "$(hint "$3")"
    failures=$((failures + 1))
}

say "LPQ Atlas doctor ($OS)"

# --- git ----------------------------------------------------------------------
if ! have git; then
    fail git "not found" git
elif [ ! -e "$ROOT/.git" ]; then
    fail git "found, but $ROOT is not a git checkout (a downloaded zip has no .git)" git
    printf "           -> lpq_benchmark refuses to run without git provenance. Use: git clone <repo-url>\n"
else
    report ok git "$(git --version | sed 's/git version //')"
fi

# --- compiler ----------------------------------------------------------------
if find_cxx; then
    report ok "C++ compiler" "$CXX_DESC ($CXX_BIN), C++20 probe passed"
else
    fail "C++ compiler" "no usable C++20 compiler. Rejected: ${CXX_TRIED:-none found}" compiler
    if [ -n "${CXX:-}" ]; then printf "           -> \$CXX is set to '%s' and is used exclusively; unset it to auto-detect\n" "$CXX"; fi
fi

# --- cmake / ninja -----------------------------------------------------------
if have cmake; then
    cmake_v="$(cmake --version | head -n1 | awk '{print $3}')"
    if version_ge "$cmake_v" 3.25; then
        report ok CMake "$cmake_v"
    else
        fail CMake "$cmake_v is older than the required 3.25 (the presets use schema version 6)" cmake
    fi
else
    fail CMake "not found" cmake
fi
if have ninja; then report ok Ninja "$(ninja --version)"; else fail Ninja "not found" ninja; fi

# --- python ------------------------------------------------------------------
if find_python; then
    report ok Python "$PY_DESC ($PY_BIN)"
    if [ "${PY_DESC#*.}" != 13 ]; then
        report note Python "CI uses 3.13; $PY_DESC should work but is less tested"
    fi
else
    fail Python "$PY_PROBLEM" python
fi

# --- node / npm --------------------------------------------------------------
if have node; then
    node_v="$(node --version | sed 's/^v//')"
    if [ "${node_v%%.*}" -ge 20 ]; then report ok Node "$node_v"; else fail Node "$node_v is older than the required 20" node; fi
else
    fail Node "not found" node
fi
if have npm; then report ok npm "$(npm --version)"; else fail npm "not found (installed with Node)" node; fi

# --- warnings ----------------------------------------------------------------
if [ "$OS" = windows ] && [ "${#ROOT}" -gt 60 ]; then
    warn "the repository path is ${#ROOT} characters long. On Windows the build can fail past 260-character paths (googletest's FetchContent stamp files); clone to a short path such as C:\\lpq."
fi
if have git && have timeout && ! timeout 15 git ls-remote --exit-code https://github.com/google/googletest.git HEAD > /dev/null 2>&1; then
    warn "cannot reach github.com. The test build downloads googletest at configure time. Offline: install GoogleTest and run setup with LPQ_USE_SYSTEM_GTEST=1, or 'setup --no-tests' (then the C++ tests are not built or run)."
fi

echo
if [ "$failures" -gt 0 ]; then
    die "$failures essential requirement(s) missing. Fix the lines marked FAIL, then re-run scripts/setup/doctor.sh. Details: docs/local-setup.md"
fi
say "all essential requirements are present. Next: scripts/setup/run.sh --test"
