# ==============================================================================
# External dependencies.
#
# Deliberately minimal. Two design decisions worth recording:
#
#  1. GoogleTest is fetched (ESD.md section 2 names GoogleTest or Catch2). It is
#     pinned to an exact tag so a rebuilt environment produces the same test
#     binary. LPQ_USE_SYSTEM_GTEST lets an offline or distro-packaged build skip
#     the download.
#
#  2. Google Benchmark is NOT used. ESD.md section 2 permits "an equivalent
#     controlled benchmark framework", and the measurement protocol this project
#     needs (PRD.md section 18: separated warm-up, fixed repetition counts,
#     paired query sets, per-query records emitted as machine-readable JSON with
#     hardware and git metadata) is not what Google Benchmark's aggregate
#     reporting produces. We implement that protocol directly in
#     cpp/src/metrics so the benchmark harness and the result schema of ESD.md
#     section 23 stay in one place.
# ==============================================================================
include_guard(GLOBAL)

option(LPQ_USE_SYSTEM_GTEST "Use an installed GoogleTest instead of fetching" OFF)

if(LPQ_BUILD_TESTS)
    if(LPQ_USE_SYSTEM_GTEST)
        find_package(GTest REQUIRED)
        message(STATUS "GoogleTest: system package ${GTest_VERSION}")
    else()
        include(FetchContent)

        set(gtest_force_shared_crt ON  CACHE BOOL "" FORCE)
        set(BUILD_GMOCK            ON  CACHE BOOL "" FORCE)
        set(INSTALL_GTEST          OFF CACHE BOOL "" FORCE)

        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.17.0   # pinned for reproducibility
            GIT_SHALLOW    TRUE
            EXCLUDE_FROM_ALL
        )
        FetchContent_MakeAvailable(googletest)
        message(STATUS "GoogleTest: fetched v1.17.0")

        # GoogleTest's own sources do not survive our warning set; it is a
        # dependency, not code under review.
        foreach(_t gtest gtest_main gmock gmock_main)
            if(TARGET ${_t})
                target_compile_options(${_t} PRIVATE
                    "$<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-w>")
            endif()
        endforeach()
    endif()
endif()

# Threads: used by the experiment runner for process-level parallelism across
# independent queries. The measured shortest-path loop itself is single-threaded
# so that every algorithm is compared under the same thread policy
# (PRD.md section 18).
find_package(Threads REQUIRED)
