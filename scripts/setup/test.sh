#!/usr/bin/env bash
# =============================================================================
# test: run every test suite and report a summary.
#
#   scripts/setup/test.sh [cpp] [python] [api] [web]     (default: all four)
#
#   cpp      ctest --preset release-fast   (the C++ suite minus the 'slow' label;
#                                           the full suite: ctest --preset release)
#   python   pytest in python/             (dataset pipeline, statistics, training)
#   api      pytest in atlas/api/          (API <-> engine contract tests)
#   web      npm run lint, typecheck, test in atlas/web
#
# Every selected suite runs even if an earlier one fails; the exit status is
# non-zero if any failed. Needs setup first (scripts/setup/setup.sh).
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
cd "$ROOT"

suites=("$@")
[ ${#suites[@]} -gt 0 ] || suites=(cpp python api web)
VPY="$(venv_python)"
[ -n "$VPY" ] || die "no .venv found. Run scripts/setup/setup.sh first."
engine_built || die "the engine is not built. Run scripts/setup/setup.sh first."

declare -a names=() results=()
run_suite() { # run_suite NAME CMD...
    local name="$1"
    shift
    say "tests: $name"
    names+=("$name")
    if "$@"; then results+=(PASS); else results+=(FAIL); fi
}

for suite in "${suites[@]}"; do
    case "$suite" in
        cpp) run_suite "C++ (ctest release-fast)" ctest --preset release-fast ;;
        python) run_suite "python/ pytest" bash -c "cd python && '$VPY' -m pytest -q" ;;
        api) run_suite "atlas/api pytest" bash -c "cd atlas/api && '$VPY' -m pytest -q" ;;
        web)
            run_suite "web lint" bash -c "cd atlas/web && npm run lint"
            run_suite "web typecheck" bash -c "cd atlas/web && npm run typecheck"
            run_suite "web unit tests" bash -c "cd atlas/web && npm test"
            ;;
        *) die "unknown suite '$suite' (cpp, python, api, web)" ;;
    esac
done

echo
say "summary"
failed=0
for i in "${!names[@]}"; do
    printf "  %-6s %s\n" "${results[$i]}" "${names[$i]}"
    [ "${results[$i]}" = PASS ] || failed=1
done
[ "$failed" = 0 ] || die "one or more test suites failed (output above)"
say "all selected suites passed"
