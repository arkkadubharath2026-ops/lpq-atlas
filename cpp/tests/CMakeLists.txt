# ==============================================================================
# Test suite (ESD.md section 24).
#
# Tests are registered with labels so that ctest presets can select subsets:
#
#   unit          fast, deterministic, per-component
#   differential  one query through every implementation, outputs compared
#   fuzz          randomised small graphs against the reference
#   slow          excluded from the "release-fast" preset and from pre-commit CI
# ==============================================================================

include(GoogleTest)

# Registers one test executable from a source file.
#
#   lpq_add_test(<name> SOURCES <files...> [LIBS <targets...>] [LABELS <labels...>])
function(lpq_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS;LABELS" ${ARGN})

    add_executable(${name} ${ARG_SOURCES})

    # Link order matters. CMake emits a target's own COMPILE_OPTIONS before the
    # INTERFACE_COMPILE_OPTIONS of the libraries it links, and for GCC the last
    # of a -W/-Wno- pair wins. Putting the relaxations in an INTERFACE target
    # listed last is therefore the only placement that actually takes effect --
    # as PRIVATE options on the executable they would be re-enabled by
    # lpq::compiler_settings arriving afterwards.
    target_link_libraries(${name} PRIVATE
        ${ARG_LIBS}
        lpq::test_support
        GTest::gtest_main
        lpq::compiler_settings
        lpq::test_warnings)

    gtest_discover_tests(${name}
        PROPERTIES LABELS "${ARG_LABELS}"
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 120)
endfunction()

# ------------------------------------------------------------------------------
# Test-only warning relaxations.
#
# -Wconversion, -Wsign-conversion and -Wold-style-cast fire throughout
# GoogleTest's macro expansions and in the ordinary `for (int i ...)` idiom that
# makes test bodies readable. They stay enabled for library code, where an
# unintended narrowing of a node index is a genuine correctness risk.
# ------------------------------------------------------------------------------
add_library(lpq_test_warnings INTERFACE)
add_library(lpq::test_warnings ALIAS lpq_test_warnings)
target_compile_options(lpq_test_warnings INTERFACE
    "$<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wno-conversion;-Wno-sign-conversion;-Wno-old-style-cast>")

# ------------------------------------------------------------------------------
# Shared test support: graph fixtures, random graph generation, assertions.
# ------------------------------------------------------------------------------
add_library(lpq_test_support INTERFACE)
add_library(lpq::test_support ALIAS lpq_test_support)
target_include_directories(lpq_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(lpq_test_support INTERFACE lpq::graph lpq::common)

# ------------------------------------------------------------------------------
# Suites
# ------------------------------------------------------------------------------
lpq_add_test(test_common
    SOURCES common/test_numeric.cpp common/test_rng.cpp common/test_types.cpp
    LIBS    lpq::common
    LABELS  "unit")

lpq_add_test(test_graph
    SOURCES graph/test_csr_graph.cpp graph/test_graph_builder.cpp graph/test_graph_metadata.cpp
    LIBS    lpq::graph
    LABELS  "unit")

lpq_add_test(test_reference_dijkstra
    SOURCES algorithms/test_reference_dijkstra.cpp
    LIBS    lpq::algorithms
    LABELS  "unit;differential")
