# ==============================================================================
# Compiler settings shared by every LPQ target.
#
# Exposed as the INTERFACE target `lpq_compiler_settings`, which every library
# and executable links against. Keeping flags in one interface target means the
# benchmark binary and the test binary are compiled identically -- a measurement
# built with different flags than the code under test is not a measurement.
# ==============================================================================
include_guard(GLOBAL)

add_library(lpq_compiler_settings INTERFACE)

set(_lpq_gnu_like "$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>")
set(_lpq_msvc     "$<CXX_COMPILER_ID:MSVC>")

# ------------------------------------------------------------------------------
# Warnings. Research code that silently truncates an index or compares signed to
# unsigned will produce wrong shortest paths, so the warning set is aggressive.
# ------------------------------------------------------------------------------
target_compile_options(lpq_compiler_settings INTERFACE
    "$<${_lpq_gnu_like}:-Wall;-Wextra;-Wpedantic;-Wshadow;-Wconversion;-Wsign-conversion;-Wcast-qual;-Wold-style-cast;-Wnon-virtual-dtor;-Woverloaded-virtual;-Wdouble-promotion;-Wformat=2;-Wimplicit-fallthrough>"
    "$<${_lpq_msvc}:/W4;/permissive-;/Zc:__cplusplus;/utf-8;/bigobj>"
)

if(LPQ_ENABLE_WERROR)
    target_compile_options(lpq_compiler_settings INTERFACE
        "$<${_lpq_gnu_like}:-Werror>" "$<${_lpq_msvc}:/WX>")
endif()

# ------------------------------------------------------------------------------
# Floating-point policy (ESD.md section 25).
#
# Fast-math is deliberately NOT enabled. Reassociation would make path costs
# depend on optimization level, which would destroy the differential test that
# compares LPQ against the reference implementation. Determinism beats a few
# percent of throughput here.
# ------------------------------------------------------------------------------
target_compile_options(lpq_compiler_settings INTERFACE
    "$<${_lpq_gnu_like}:-fno-fast-math>"
    "$<${_lpq_msvc}:/fp:precise>"
)

# ------------------------------------------------------------------------------
# Per-configuration optimization.
#
# Debug keeps assertions and frame pointers. Release is -O3 with NDEBUG.
# RelWithDebInfo doubles as the "Profile" profile of ESD.md section 34: release
# optimization plus symbols and frame pointers so perf can unwind.
# ------------------------------------------------------------------------------
target_compile_options(lpq_compiler_settings INTERFACE
    "$<${_lpq_gnu_like}:$<$<CONFIG:Debug>:-O0;-g3;-fno-omit-frame-pointer>>"
    "$<${_lpq_gnu_like}:$<$<CONFIG:Release>:-O3>>"
    "$<${_lpq_gnu_like}:$<$<CONFIG:RelWithDebInfo>:-O2;-g;-fno-omit-frame-pointer>>"
    "$<${_lpq_msvc}:$<$<CONFIG:Debug>:/Od;/Zi>>"
    "$<${_lpq_msvc}:$<$<CONFIG:Release>:/O2>>"
)

# ------------------------------------------------------------------------------
# -march=native. Off by default: a binary built with it is not portable, and
# ESD.md section 34 requires a portable release profile to exist. Benchmarks
# that enable it must record the fact (the runner reports LPQ_NATIVE_ARCH).
# ------------------------------------------------------------------------------
if(LPQ_NATIVE_ARCH)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=native" LPQ_HAS_MARCH_NATIVE)
    if(LPQ_HAS_MARCH_NATIVE)
        target_compile_options(lpq_compiler_settings INTERFACE -march=native)
    else()
        message(WARNING "LPQ_NATIVE_ARCH requested but -march=native is unsupported; ignoring.")
    endif()
endif()

if(LPQ_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _lpq_ipo_ok OUTPUT _lpq_ipo_msg)
    if(_lpq_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON PARENT_SCOPE)
    else()
        message(WARNING "LTO requested but unsupported: ${_lpq_ipo_msg}")
    endif()
endif()

# ------------------------------------------------------------------------------
# Sanitizers.
#
# ASan/UBSan are the intended Debug configuration (ESD.md section 34). They are
# unavailable on MinGW targets, where the runtime is simply not shipped; rather
# than emit flags that fail at link time we detect support and warn, so a
# Windows/MinGW developer gets a working Debug build and an explicit notice that
# sanitizer coverage must come from CI on Linux.
# ------------------------------------------------------------------------------
if(LPQ_ENABLE_SANITIZERS)
    if(MINGW)
        message(WARNING
            "LPQ_ENABLE_SANITIZERS=ON but the MinGW target does not ship the "
            "ASan/UBSan runtimes. Sanitizers are disabled for this build; run "
            "the sanitizer configuration on Linux/Clang or in CI instead.")
    elseif(MSVC)
        target_compile_options(lpq_compiler_settings INTERFACE /fsanitize=address)
    else()
        set(_lpq_san "-fsanitize=address,undefined;-fno-sanitize-recover=undefined;-fno-omit-frame-pointer")
        target_compile_options(lpq_compiler_settings INTERFACE "${_lpq_san}")
        target_link_options(lpq_compiler_settings INTERFACE "-fsanitize=address,undefined")
    endif()
endif()

# ------------------------------------------------------------------------------
# Feature macros consumed by the C++ sources.
# ------------------------------------------------------------------------------
target_compile_definitions(lpq_compiler_settings INTERFACE
    $<$<BOOL:${LPQ_ENABLE_INSTRUMENTATION}>:LPQ_ENABLE_INSTRUMENTATION=1>
    $<$<NOT:$<BOOL:${LPQ_ENABLE_INSTRUMENTATION}>>:LPQ_ENABLE_INSTRUMENTATION=0>
)

# Windows: avoid min/max macros clobbering std::min/std::max, and keep the
# lean header set so <windows.h> does not slow every translation unit down.
if(WIN32)
    target_compile_definitions(lpq_compiler_settings INTERFACE
        NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ------------------------------------------------------------------------------
# MinGW: link the GCC and C++ runtimes statically.
#
# Without this, every binary needs libstdc++-6.dll, libgcc_s_seh-1.dll and
# libwinpthread-1.dll from the toolchain directory on PATH. That turns running a
# test or a converted-dataset build into "did you remember to set PATH", and the
# failure is a bare 0xC0000139 exit code with no message -- which is exactly the
# kind of thing that makes a reproduction script unreproducible on someone
# else's machine.
#
# It also removes a measurement hazard: a dynamically linked libstdc++ can be
# resolved to a different version on a different machine, so a benchmark would
# not necessarily be running the same standard-library code it was validated
# against.
# ------------------------------------------------------------------------------
if(MINGW)
    target_link_options(lpq_compiler_settings INTERFACE
        -static-libgcc -static-libstdc++ -static)
endif()

add_library(lpq::compiler_settings ALIAS lpq_compiler_settings)
