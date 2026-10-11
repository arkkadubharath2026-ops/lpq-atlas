#!/usr/bin/env bash
# =============================================================================
# setup: install dependencies, build the engine, prepare the demo dataset.
#
#   scripts/setup/setup.sh [--no-tests] [--skip-web] [--force]
#
#   --no-tests   do not build the C++ tests (skips the googletest download, so
#                this works offline; scripts/setup/test.sh then cannot run ctest)
#   --skip-web   do not run `next build` (run.sh builds it on demand, or use
#                run.sh --dev)
#   --force      redo steps that look finished (npm ci, demo conversion)
#
# Idempotent: re-running does only the work that is missing. Steps:
#   1. doctor            2. .venv + pip install     3. npm ci
#   4. cmake --preset release && cmake --build      5. demo dataset
#   6. next build
# Environment: LPQ_JOBS (build parallelism), LPQ_USE_SYSTEM_GTEST=1,
#              CXX / CC (compiler override), LPQ_SETUP_PYTHON (interpreter).
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

build_tests=ON skip_web=0 force=0
for arg in "$@"; do
    case "$arg" in
        --no-tests) build_tests=OFF ;;
        --skip-web) skip_web=1 ;;
        --force) force=1 ;;
        -h | --help) sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option: $arg" ;;
    esac
done

cd "$ROOT"
bash "$SETUP_DIR/doctor.sh"
find_cxx || die "no C++20 compiler (doctor should have caught this)"
find_python || die "$PY_PROBLEM"

# --- 2. Python environment ---------------------------------------------------
say "Python environment (.venv)"
if [ -z "$(venv_python)" ]; then
    "$PY_BIN" -m venv "$ROOT/.venv"
fi
VPY="$(venv_python)"
venv_minor="$("$VPY" -c 'import sys; print(sys.version_info[1])')"
if [ "$venv_minor" -lt 11 ] || [ "$venv_minor" -gt 13 ]; then
    die ".venv uses Python 3.$venv_minor, outside 3.11-3.13. Delete .venv and re-run."
fi
export LPQ_PYTHON="$VPY"
"$VPY" -m pip install --disable-pip-version-check -q --upgrade pip
"$VPY" -m pip install --disable-pip-version-check -r python/requirements-dev.txt -r atlas/api/requirements.txt ||
    die "pip install failed. If a package could not be built, check the Python version (3.13 recommended)."

# --- 3. Web dependencies -----------------------------------------------------
say "Web dependencies (npm ci in atlas/web)"
if [ "$force" = 0 ] && [ "$WEB_DIR/node_modules/.package-lock.json" -nt "$WEB_DIR/package-lock.json" ]; then
    echo "    node_modules is current"
else
    (cd "$WEB_DIR" && npm ci) || die "npm ci failed"
fi

# --- 4. Engine -----------------------------------------------------------------
say "Engine: CMake preset 'release' (tests: $build_tests), compiler $CXX_BIN"
export CXX="$CXX_BIN"
cc="$(c_compiler_for "$CXX_BIN")"
[ -n "$cc" ] && export CC="$cc"
cmake_args=(--preset release "-DLPQ_BUILD_TESTS=$build_tests")
[ "${LPQ_USE_SYSTEM_GTEST:-0}" = 1 ] && cmake_args+=(-DLPQ_USE_SYSTEM_GTEST=ON)
cmake -S "$ROOT" "${cmake_args[@]}" || die "cmake configure failed"
build_args=(--build "$BUILD_DIR")
[ -n "${LPQ_JOBS:-}" ] && build_args+=(-j "$LPQ_JOBS")
cmake "${build_args[@]}" || die "the engine build failed"
for t in "${ENGINE_TOOLS[@]}"; do require_tool "$t"; done
echo "    tools in $BIN_DIR: ${ENGINE_TOOLS[*]}"

# --- 5. Demo dataset (offline) --------------------------------------------------
say "Demo dataset (demo_road, committed source files, no network)"
force_flag=()
[ "$force" = 1 ] && force_flag=(--force)
"$VPY" scripts/demo/prepare_demo.py ${force_flag[@]+"${force_flag[@]}"}

# --- 6. Web build ----------------------------------------------------------------
if [ "$skip_web" = 0 ]; then ensure_web_built; fi

say "setup complete. Next: scripts/setup/run.sh   (add --test to run the test suites first)"
