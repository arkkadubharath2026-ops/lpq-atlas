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

say "configuring ${BUILD_DIR}"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release > /dev/null ||
    die "cmake configure failed"

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
