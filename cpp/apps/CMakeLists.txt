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
