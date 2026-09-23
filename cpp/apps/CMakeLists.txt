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

# ------------------------------------------------------------------------------
# lpq_profile -- phase-level profile of the priority queue (PRD.md section 20).
#
# A SEPARATE target from lpq_benchmark, and the separation is the point. This
# one compiles LPQ_ENABLE_PHASE_PROFILING=1, which puts a pair of timestamp
# reads around every phase of every push and pop. That perturbs the code it
# measures, so it must not be the binary that produces the project's timings.
# lpq_benchmark stays free of probes; this answers a different question.
#
# It also replaces the global operator new to count allocations exactly, which
# is another reason it cannot share a binary with the benchmark.
# ------------------------------------------------------------------------------
add_executable(lpq_profile lpq_profile.cpp)
target_link_libraries(lpq_profile PRIVATE
    lpq::algorithms
    lpq::pq
    lpq::lpq
    lpq::verification
    lpq::graph
    lpq::metrics
    lpq::compiler_settings)
target_compile_definitions(lpq_profile PRIVATE LPQ_ENABLE_PHASE_PROFILING=1)
target_compile_options(lpq_profile PRIVATE
    "$<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wno-old-style-cast>")

# ------------------------------------------------------------------------------
# lpq_profile_noinstr -- the same source with the probes compiled out.
#
# This target exists to measure the instrumentation rather than assume it is
# negligible. It runs the identical region over the identical queries with
# LPQ_ENABLE_PHASE_PROFILING=0, so the ratio of its wall clock to lpq_profile's
# IS the overhead the probes impose. Reporting phase shares without that ratio
# would be reporting an instrument reading with no idea how much of it is the
# instrument.
#
# It keeps the allocation counter, which costs an increment and needs no clock.
# ------------------------------------------------------------------------------
add_executable(lpq_profile_noinstr lpq_profile.cpp)
target_link_libraries(lpq_profile_noinstr PRIVATE
    lpq::algorithms
    lpq::pq
    lpq::lpq
    lpq::verification
    lpq::graph
    lpq::metrics
    lpq::compiler_settings)
target_compile_definitions(lpq_profile_noinstr PRIVATE LPQ_ENABLE_PHASE_PROFILING=0)
target_compile_options(lpq_profile_noinstr PRIVATE
    "$<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wno-old-style-cast>")
