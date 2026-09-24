#!/usr/bin/env bash
# =============================================================================
# Build everything the reproduction needs (ESD.md section 36).
#
#   scripts/reproduce/build_all.sh
#
# Configures and builds the Release preset, then verifies every tool the later
# scripts invoke actually exists. The verification is not redundant: a build can
# succeed while a target is missing from it, because a target that was never
# added to CMakeLists.txt produces no error at all -- the build simply does not
# contain it, and the failure surfaces three scripts later as "command not
# found".
#
# The test suite is run too. A reproduction built from code that does not pass
# its own tests is not a reproduction of anything, and finding that out here
# costs a minute where finding it out from a wrong figure costs a great deal
# more.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

# -----------------------------------------------------------------------------
# The compiler is checked BEFORE configuring, because the failure it produces
# otherwise is unreadable.
#
# A clean-clone run on the development host selected C:\MinGW\bin\g++, which is
# GCC 6.3.0 from 2016 and first on PATH, while the compiler this project is
# actually built with sits further down it. CMake reported
# 'the current compiler "GNU" does not support CXX20' from inside a FindThreads
# try_compile, buried in a twenty-line call stack through FetchContent -- which
# names neither the compiler it chose nor the fact that another one would work.
#
# Checking here costs one compile of an empty translation unit and turns that
# into a single actionable line.
# -----------------------------------------------------------------------------
say "checking the C++ compiler supports C++20"
cxx_probe="$(mktemp -d)"
trap 'rm -rf "$cxx_probe"' EXIT
cat > "${cxx_probe}/probe.cpp" <<'PROBE'
#include <version>
#include <concepts>
template <typename T> concept Addable = requires(T a) { a + a; };
static_assert(Addable<int>);
int main() { return 0; }
PROBE
if ! "${CXX:-c++}" -std=c++20 -c "${cxx_probe}/probe.cpp" -o "${cxx_probe}/probe.o" 2> "${cxx_probe}/err"; then
    printf '    compiler tried: %s\n' "$(command -v "${CXX:-c++}" || echo "${CXX:-c++}")" >&2
    printf '    version:        %s\n' "$("${CXX:-c++}" --version 2>/dev/null | head -1)" >&2
    sed 's/^/    /' "${cxx_probe}/err" >&2
    die "the C++ compiler on PATH does not support C++20. Set CXX to one that does, e.g.
    CXX=/path/to/modern/g++ scripts/reproduce/build_all.sh
  This project requires C++20 (concepts, <version>, if constexpr); an older
  compiler fails much later inside a CMake try_compile with a message that
  names neither the compiler nor the requirement."
fi
printf '    %s\n' "$("${CXX:-c++}" --version 2>/dev/null | head -1)"

say "configuring ${BUILD_DIR}"
# The generator is pinned rather than left to CMake's default. On Windows,
# outside a Visual Studio developer shell, that default is NMake Makefiles, and
# the configure fails with "CMAKE_CXX_COMPILER not set" -- which is exactly how
# this script failed its first run from a clean clone. The `release` preset
# already fixes the generator (Ninja) and resolves to ${REPO_ROOT}/build/release,
# which is BUILD_DIR's default, so the preset is used whenever BUILD_DIR has not
# been overridden.
if [ "$BUILD_DIR" = "${REPO_ROOT}/build/release" ]; then
    cmake -S "$REPO_ROOT" --preset release > /dev/null ||
        die "cmake configure failed (preset 'release')"
else
    # LPQ_BUILD_DIR was overridden, so the preset's binaryDir would disagree
    # with it. Ninja is still required by name for the same reason.
    command -v ninja > /dev/null ||
        die "ninja is required to configure a build outside the default directory"
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null ||
        die "cmake configure failed"
fi

say "building"
# The status is captured rather than piped away: a build whose exit code is
# discarded turns a compile error into a missing-tool error much later.
if ! cmake --build "$BUILD_DIR"; then
    die "the build failed. Nothing below this point would be reproducing anything."
fi

say "checking every tool the reproduction invokes is present"
for tool in lpq_benchmark lpq_convert lpq_collect lpq_model_info lpq_profile; do
    require_tool "$tool"
    printf "    %s\n" "$tool"
done

say "running the test suite"
if ! ctest --test-dir "$BUILD_DIR" --output-on-failure > /tmp/lpq_ctest.log 2>&1; then
    tail -30 /tmp/lpq_ctest.log >&2
    die "the test suite did not pass. Full log: /tmp/lpq_ctest.log"
fi
grep -E "tests passed" /tmp/lpq_ctest.log || true

require_python
say "checking the Python analysis stack"
"$PYTHON" -c "
import lpq_tools.evaluation.ablation
import lpq_tools.statistics.comparison
import lpq_tools.training.train
print('    lpq_tools imports cleanly')
" || die "lpq_tools could not be imported. Check PYTHONPATH and the requirements."

say "build_all: complete"
