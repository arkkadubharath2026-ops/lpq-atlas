# ==============================================================================
# C++ targets.
#
# One target per module (ESD.md section 4.3: graph storage, shortest-path logic,
# priority queues, prediction, verification, adaptation and instrumentation must
# be independent). ESD.md section 40 further requires that "all modules compile
# independently", so each module is its own library with an explicit dependency
# edge rather than one monolithic target.
#
# Modules that are currently header-only are declared INTERFACE. They become
# STATIC as soon as they gain a translation unit; dependents link against
# lpq::<module> either way and do not need to change.
# ==============================================================================

# ------------------------------------------------------------------------------
# lpq_common -- scalar types, numeric policy, errors, clock, RNG.
# Depends on nothing but the standard library.
# ------------------------------------------------------------------------------
add_library(lpq_common INTERFACE)
add_library(lpq::common ALIAS lpq_common)
target_include_directories(lpq_common INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(lpq_common INTERFACE lpq::compiler_settings)

# ------------------------------------------------------------------------------
# lpq_graph -- CSR representation, construction, metadata, dataset IO.
# ------------------------------------------------------------------------------
add_library(lpq_graph INTERFACE)
add_library(lpq::graph ALIAS lpq_graph)
target_include_directories(lpq_graph INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(lpq_graph INTERFACE lpq::common)

# ------------------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------------------
if(LPQ_BUILD_TESTS)
    # enable_testing() is called in the top-level CMakeLists, not here.
    add_subdirectory(tests)
endif()
