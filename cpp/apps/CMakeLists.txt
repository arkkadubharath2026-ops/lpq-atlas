# ==============================================================================
# Command-line tools.
#
# ESD.md section 21 asks for "one unified executable" so that every algorithm,
# queue and mode is measured through the same code path.
# ==============================================================================

add_executable(lpq_benchmark lpq_benchmark.cpp)
target_link_libraries(lpq_benchmark PRIVATE
    lpq::algorithms
    lpq::pq
    lpq::graph
    lpq::metrics
    lpq::compiler_settings)

# The runner parses argv and formats reports; -Wold-style-cast and -Wconversion
# fire on printf-family formatting without indicating a real defect there.
target_compile_options(lpq_benchmark PRIVATE
    "$<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wno-old-style-cast>")

# ------------------------------------------------------------------------------
# lpq_convert -- dataset conversion (ESD.md section 26).
#
# Separate from lpq_benchmark because it is offline preparation, not
# measurement: it runs once per dataset and never inside a timed region.
# ------------------------------------------------------------------------------
add_executable(lpq_convert lpq_convert.cpp)
target_link_libraries(lpq_convert PRIVATE
    lpq::graph
    lpq::metrics
    lpq::compiler_settings)
