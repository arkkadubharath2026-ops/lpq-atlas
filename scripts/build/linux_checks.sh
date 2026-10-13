#!/usr/bin/env bash
# =============================================================================
# Linux verification matrix, run inside the pinned lpq-dev container.
#
# The Windows development build cannot provide these checks: MinGW ships no
# sanitizer runtimes, and a single compiler hides undefined behaviour that the
# other one's codegen exposes. This script is the Linux half of ESD.md
# sections 34 and 35.
#
# Configurations:
#   1. GCC 12    Release  -Werror     -- the portable benchmark build
#   2. Clang 16  Release  -Werror     -- second opinion on warnings
#   3. Clang 16  Debug    ASan+UBSan+LSan
#
# Usage:
#   scripts/build/linux_checks.sh            # all three
#   scripts/build/linux_checks.sh sanitize   # just the sanitizer configuration
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="${LPQ_IMAGE:-lpq-dev:latest}"
WHICH="${1:-all}"

# -----------------------------------------------------------------------------
# LeakSanitizer stops threads with ptrace to scan for reachable memory. Docker's
# default seccomp profile blocks the ptrace syscall, so LSan aborts during
# process teardown -- the tests all pass and then the process dies with SIGSEGV,
# which looks alarmingly like a bug in the code under test but is not.
#
# Granting SYS_PTRACE and relaxing seccomp lets leak detection run properly.
# The alternative, ASAN_OPTIONS=detect_leaks=0, silences the crash by giving up
# the leak checking, which is the opposite of what a memory-heavy graph engine
# wants from its Debug build.
# -----------------------------------------------------------------------------
DOCKER_SECURITY_ARGS=(--cap-add=SYS_PTRACE --security-opt seccomp=unconfined)

# MSYS/Git Bash rewrites anything that looks like a Unix path in an argument
# into a Windows path before the process sees it, so `-w /workspace` arrived at
# the daemon as `C:/Program Files/Git/workspace` and the run died with "the
# working directory is invalid". The container paths are meaningless on the host
# and must be passed through untouched; the host path in -v is produced by
# `pwd` and is already correct for the platform.
#
# The script body is fed on stdin from a quoted heredoc rather than passed as an
# argument. It used to be a single-quoted shell string, and that spelling was a
# trap: any apostrophe anywhere inside it -- including one in ordinary comment
# prose such as "the build's exit status" -- silently closed the string, and the
# remainder of the file was then parsed as host shell. The failure surfaced as a
# syntax error hundreds of lines away from its cause, after the configurations
# had already run. A quoted heredoc has no such escape processing: apostrophes,
# backticks and dollar signs all pass through literally, so the body can be
# written and commented like normal shell.
#
# -i keeps stdin attached; WHICH crosses the boundary as an environment variable
# rather than by interpolation, so nothing in the body is substituted by the host.
run_in_container() {
    MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' \
    docker run --rm -i "${DOCKER_SECURITY_ARGS[@]}" \
        -v "${REPO_ROOT}:/workspace" -w /workspace \
        -e "WHICH=${WHICH}" \
        "${IMAGE}" bash -ls
}

if ! command -v docker > /dev/null 2>&1; then
    echo "error: docker not found on PATH." >&2
    echo "       On Windows, Docker Desktop installs it under:" >&2
    echo "       %LOCALAPPDATA%\\Programs\\DockerDesktop\\resources\\bin" >&2
    exit 1
fi

# Each configuration builds into its own directory under /tmp so that the
# host's build/ tree, produced by a different compiler, is never reused.
run_in_container <<'CONTAINER_SCRIPT'
set -euo pipefail

section() { printf "\n\033[1m=== %s ===\033[0m\n" "$1"; }

# ASan reserves a large fixed shadow-memory region at startup. The WSL2 kernel
# sets vm.mmap_rnd_bits=32, the maximum entropy, and at that setting the kernel
# sometimes places mappings where the shadow region must go -- the process then
# dies with SIGSEGV before main() runs. Measured on this host: 7 failures in 20
# startups with ASLR on, 0 in 20 with it off.
#
# The failure is intermittent and lands on a different test each run, which
# makes it look like a memory bug in the code under test. It is not.
#
# setarch -R clears the address-space randomisation flag, which is inherited by
# child processes, so launching ctest under it covers every test binary it
# spawns. The alternative fix, sysctl vm.mmap_rnd_bits=28, needs a privileged
# container and mutates a host-wide kernel setting, so it is not used here.
# Losing ASLR does not weaken these runs: ASan detects the overflow by shadow
# state, not by where the heap happens to land.
aslr_off() {
    if command -v setarch > /dev/null 2>&1; then
        echo "setarch -R"
    fi
}

build_and_test() {
    local name="$1" dir="$2" sanitized="$3"; shift 3
    section "$name"
    cmake -S . -B "$dir" -G Ninja "$@" > /dev/null

    # Warnings are errors in these configurations, so a clean build is a
    # pass/fail signal in its own right -- which means the build's exit status
    # has to actually be checked. It previously ran as
    #
    #     cmake --build "$dir" 2>&1 | grep ... || true
    #
    # where the `|| true`, present so that grep's "no matches" status would not
    # abort the run, also swallowed a failed compile. The run would print the
    # errors, continue on to ctest, and report whatever ctest made of a stale or
    # absent build directory. A verification script that can report success on a
    # broken build is worse than none at all, because commits are gated on it.
    #
    # The status is captured before the pipeline so that grep filters the output
    # without deciding the outcome.
    local build_log build_status=0
    build_log="$(cmake --build "$dir" 2>&1)" || build_status=$?
    printf "%s\n" "$build_log" | grep -E "error:|warning:" | grep -v googletest || true
    if [ "$build_status" -ne 0 ]; then
        echo "FAILED: build did not succeed for $name" >&2
        return "$build_status"
    fi

    local launcher=""
    if [ "$sanitized" = "sanitized" ]; then
        launcher="$(aslr_off)"
    fi
    # tail -10, not -4: ctest prints the "N% tests passed out of M" line
    # ABOVE the label time summary, and a shorter tail cut it off -- leaving a
    # run that had verified nothing looking indistinguishable from one that
    # passed. The build directories live in the container, which is --rm, so
    # there is no way to go back and check afterwards.
    # shellcheck disable=SC2086
    $launcher ctest --test-dir "$dir" --output-on-failure 2>&1 | tail -10
}

if [ "$WHICH" = "all" ]; then
    build_and_test "GCC 12 / Release / -Werror" /tmp/b-gcc plain \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ -DLPQ_ENABLE_WERROR=ON

    build_and_test "Clang 16 / Release / -Werror" /tmp/b-clang plain \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ -DLPQ_ENABLE_WERROR=ON
fi

build_and_test "Clang 16 / Debug / ASan+UBSan+LSan" /tmp/b-san sanitized \
    -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ \
    -DLPQ_ENABLE_SANITIZERS=ON -DLPQ_ENABLE_WERROR=ON
CONTAINER_SCRIPT

# Reached only when every configuration above returned zero, because the body
# runs under `set -e` and the heredoc feeds a pipeline whose status is the
# container process's. A failed build or a failed test therefore stops the
# script here rather than printing a completion line over the top of it.
echo
echo "[linux_checks] all configurations completed"
