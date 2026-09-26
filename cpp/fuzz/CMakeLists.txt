# ==============================================================================
# Fuzz targets (LPQ Atlas spec D-07).
#
# -DLPQ_FUZZ_ENGINE=libfuzzer (clang only) builds coverage-guided libFuzzer
# binaries with ASan/UBSan. Otherwise each target links a standalone driver
# that replays a corpus or a seeded random stream; the test suite runs that as
# a quick smoke test (label fuzz-smoke) so normal CI stays fast.
# ==============================================================================
set(LPQ_FUZZ_ENGINE "standalone" CACHE STRING "standalone | libfuzzer")

function(lpq_add_fuzzer name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ${ARGN} lpq::compiler_settings)
    # Byte-level decoding casts freely; these warnings flag nothing real here.
    # -Wuseless-cast is GCC-only; Clang rejects the unknown option under -Werror.
    target_compile_options(${name} PRIVATE
        "$<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wno-old-style-cast;-Wno-conversion;-Wno-sign-conversion>"
        "$<$<CXX_COMPILER_ID:GNU>:-Wno-useless-cast>")
    if(LPQ_FUZZ_ENGINE STREQUAL "libfuzzer")
        target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined -g)
        target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    else()
        target_sources(${name} PRIVATE standalone_driver.cpp)
        if(LPQ_BUILD_TESTS)
            add_test(NAME ${name}_smoke COMMAND ${name} --random 3000 20260926)
            set_tests_properties(${name}_smoke PROPERTIES LABELS "fuzz-smoke")
        endif()
    endif()
endfunction()

lpq_add_fuzzer(fuzz_graph_binary lpq::graph)
lpq_add_fuzzer(fuzz_model_load lpq::models lpq::features)
lpq_add_fuzzer(fuzz_experiment_manifest lpq::metrics)
lpq_add_fuzzer(fuzz_queue_ops lpq::lpq lpq::verification lpq::pq)
