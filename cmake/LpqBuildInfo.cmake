# ==============================================================================
# Build provenance injected into the binary.
#
# ESD.md section 40: "No benchmark result is generated without hardware and git
# metadata." Capturing the commit at CONFIGURE time and compiling it in is what
# makes that a property of the binary rather than of the script that happened to
# launch it -- a result file cannot then be produced without provenance, even if
# the benchmark is run by hand.
#
# The dirty flag matters as much as the hash. A commit hash identifies the code
# only when the working tree is clean; results from a dirty tree are not
# reproducible, and the result validator refuses them for publication-tier runs.
# ==============================================================================
include_guard(GLOBAL)

find_package(Git QUIET)

set(LPQ_GIT_COMMIT "unknown")
set(LPQ_GIT_DIRTY 0)

if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _lpq_commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _lpq_commit_rc)

    if(_lpq_commit_rc EQUAL 0 AND _lpq_commit)
        set(LPQ_GIT_COMMIT "${_lpq_commit}")
    endif()

    # `git status --porcelain` prints one line per modified path and nothing at
    # all for a clean tree, which makes emptiness the test. `git diff --quiet`
    # is not used because it ignores untracked files, and an untracked source
    # file can absolutely change a build.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _lpq_status
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    if(NOT _lpq_status STREQUAL "")
        set(LPQ_GIT_DIRTY 1)
    endif()

    # The values above are captured at CONFIGURE time and compiled in. Without
    # the following, they would go stale the moment a commit is made: the binary
    # would keep reporting the previous commit and a permanent "dirty" flag,
    # which is worse than having no provenance at all because it looks
    # authoritative. Listing the git state files as configure dependencies makes
    # CMake re-run configuration whenever they change.
    #
    #   .git/HEAD    changes on commit, checkout and branch switch
    #   .git/index   changes when anything is staged or unstaged
    #
    # A build is therefore never produced with provenance that does not describe
    # it, which is what ESD.md section 40 requires.
    foreach(_lpq_git_file "${CMAKE_SOURCE_DIR}/.git/HEAD" "${CMAKE_SOURCE_DIR}/.git/index")
        if(EXISTS "${_lpq_git_file}")
            set_property(DIRECTORY "${CMAKE_SOURCE_DIR}"
                         APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_lpq_git_file}")
        endif()
    endforeach()
endif()

# Reconstruct the optimization flags actually in effect, so the record describes
# the binary rather than the preset name.
set(_lpq_opt_flags "${CMAKE_CXX_FLAGS}")
string(TOUPPER "${CMAKE_BUILD_TYPE}" _lpq_build_upper)
if(_lpq_build_upper)
    set(_lpq_opt_flags "${_lpq_opt_flags} ${CMAKE_CXX_FLAGS_${_lpq_build_upper}}")
endif()
if(LPQ_NATIVE_ARCH)
    set(_lpq_opt_flags "${_lpq_opt_flags} -march=native")
    set(_lpq_native 1)
else()
    set(_lpq_native 0)
endif()
string(STRIP "${_lpq_opt_flags}" _lpq_opt_flags)

target_compile_definitions(lpq_compiler_settings INTERFACE
    LPQ_GIT_COMMIT="${LPQ_GIT_COMMIT}"
    LPQ_GIT_DIRTY=${LPQ_GIT_DIRTY}
    LPQ_OPTIMIZATION_FLAGS="${_lpq_opt_flags}"
    LPQ_NATIVE_ARCH_ENABLED=${_lpq_native})

message(STATUS "  git commit          : ${LPQ_GIT_COMMIT}")
if(LPQ_GIT_DIRTY)
    message(STATUS "  git state           : DIRTY (results are not reproducible)")
else()
    message(STATUS "  git state           : clean")
endif()
