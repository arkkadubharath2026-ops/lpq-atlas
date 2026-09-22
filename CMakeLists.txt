# ==============================================================================
# Dynamic Approximate Shortest Path Using a Learned Priority Queue (LPQ)
#
# Build entry point. See ESD.md sections 2 (stack), 34 (build profiles) and 39
# (implementation order). Correctness first: Debug builds enable sanitizers and
# assertions; Release builds are what benchmarks must use.
# ==============================================================================
cmake_minimum_required(VERSION 3.24)

project(dynamic_lpq
    VERSION 0.1.0
    DESCRIPTION "Learning-augmented, verification-aware priority queue for shortest-path search"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ------------------------------------------------------------------------------
# Language standard. C++20 is a hard requirement (concepts, <span>, <bit>,
# <format>, ranges). No compiler-specific extensions, so the code stays portable.
# ------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Single output tree so scripts can rely on <build>/bin/<tool>.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Export compile_commands.json for clang-tidy and editor tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ------------------------------------------------------------------------------
# Options
# ------------------------------------------------------------------------------
option(LPQ_BUILD_TESTS        "Build the unit / differential / fuzz test suite" ON)
option(LPQ_BUILD_BENCHMARKS   "Build the microbenchmark targets"               ON)
option(LPQ_BUILD_APPS         "Build command-line tools (lpq_benchmark, ...)"  ON)
option(LPQ_ENABLE_SANITIZERS  "Enable ASan+UBSan (Debug profile)"              OFF)
option(LPQ_NATIVE_ARCH        "Compile with -march=native (NOT portable)"      OFF)
option(LPQ_ENABLE_LTO         "Enable link-time optimization in Release"       OFF)
option(LPQ_ENABLE_WERROR      "Treat compiler warnings as errors"              OFF)

# Instrumentation is compile-time configurable (ESD.md section 19). Counters are
# cheap but not free; timed publication runs may disable them to measure the
# true hot path, and the runner records which setting produced each result.
option(LPQ_ENABLE_INSTRUMENTATION "Compile in LPQ/PQ counters and timers" ON)

include(LpqCompilerSettings)
include(LpqBuildInfo)

# ------------------------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------------------------
include(LpqDependencies)

# ------------------------------------------------------------------------------
# Testing.
#
# enable_testing() must be called here, in the top-level list file. Calling it
# only in a subdirectory registers the tests under that subdirectory's binary
# dir, and `ctest` invoked at the build root then reports "No tests were found".
# ------------------------------------------------------------------------------
if(LPQ_BUILD_TESTS)
    enable_testing()
endif()

# ------------------------------------------------------------------------------
# Targets
# ------------------------------------------------------------------------------
add_subdirectory(cpp)

# ------------------------------------------------------------------------------
# Build summary -- printed so that every benchmark run can be traced to flags.
# ------------------------------------------------------------------------------
message(STATUS "")
message(STATUS "=== dynamic_lpq ${PROJECT_VERSION} =======================================")
message(STATUS "  build type          : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler            : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ standard        : ${CMAKE_CXX_STANDARD}")
message(STATUS "  instrumentation     : ${LPQ_ENABLE_INSTRUMENTATION}")
message(STATUS "  sanitizers          : ${LPQ_ENABLE_SANITIZERS}")
message(STATUS "  -march=native       : ${LPQ_NATIVE_ARCH}")
message(STATUS "  LTO                 : ${LPQ_ENABLE_LTO}")
message(STATUS "  tests               : ${LPQ_BUILD_TESTS}")
message(STATUS "  benchmarks          : ${LPQ_BUILD_BENCHMARKS}")
message(STATUS "==========================================================================")
message(STATUS "")
