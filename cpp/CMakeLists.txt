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
# lpq_pq -- classical priority queues (binary/d-ary, pairing, radix, Fibonacci).
# ------------------------------------------------------------------------------
add_library(lpq_pq INTERFACE)
add_library(lpq::pq ALIAS lpq_pq)
target_include_directories(lpq_pq INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(lpq_pq INTERFACE lpq::common)

# ------------------------------------------------------------------------------
# lpq_features -- feature vector, precomputed graph features, search observations.
# ------------------------------------------------------------------------------
add_library(lpq_features INTERFACE)
add_library(lpq::features ALIAS lpq_features)
target_include_directories(lpq_features INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(lpq_features INTERFACE lpq::graph lpq::common)

# ------------------------------------------------------------------------------
# lpq_models -- predictor interface, baselines, linear/logistic/MLP, tree
# ensemble inference, calibration, model serialization.
#
# Depends on lpq::features for the feature layout (a model is meaningless
# without the column order it was trained against) and on nothing else. In
# particular it does NOT depend on lpq::algorithms: a predictor must be
# testable, trainable and serialisable without a graph or a search, which is
# what lets the model comparison run offline.
# ------------------------------------------------------------------------------
add_library(lpq_models INTERFACE)
add_library(lpq::models ALIAS lpq_models)
target_include_directories(lpq_models INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(lpq_models INTERFACE lpq::features lpq::common)

# ------------------------------------------------------------------------------
# lpq_lpq -- the learned priority queue itself: candidate management, and (as
# later milestones land) verification, correction and adaptation.
#
# Depends on lpq::models for Prediction and on lpq::features for the feature
# layout, and on lpq::pq for the QueueEntry type and the PriorityQueueLike
# concept that the authoritative substrate satisfies.
#
# The CandidateManager itself still reaches into no queue -- it is handed
# candidates -- which is what keeps candidate policy testable without a queue,
# a graph or a search. The substrate is the component that needs the queue
# vocabulary, because it IS one.
# ------------------------------------------------------------------------------
add_library(lpq_lpq INTERFACE)
add_library(lpq::lpq ALIAS lpq_lpq)
target_include_directories(lpq_lpq INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(lpq_lpq INTERFACE lpq::models lpq::features lpq::pq lpq::common)

# ------------------------------------------------------------------------------
# lpq_verification -- correctness policy, guarantee classification, and the
# verification engine that decides whether a candidate may be extracted.
#
# Separate from lpq::lpq so the correctness policy can be reasoned about, and
# tested, without the candidate machinery -- and so that a change to candidate
# selection cannot silently alter what "verified" means.
# ------------------------------------------------------------------------------
add_library(lpq_verification INTERFACE)
add_library(lpq::verification ALIAS lpq_verification)
target_include_directories(lpq_verification INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(lpq_verification INTERFACE lpq::lpq lpq::common)

# ------------------------------------------------------------------------------
# lpq_metrics -- statistics, CPU pinning, JSON, hardware info, result schema.
# ------------------------------------------------------------------------------
add_library(lpq_metrics INTERFACE)
add_library(lpq::metrics ALIAS lpq_metrics)
target_include_directories(lpq_metrics INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(lpq_metrics INTERFACE lpq::common lpq::algorithms Threads::Threads)

# ------------------------------------------------------------------------------
# lpq_algorithms -- reference Dijkstra, Dijkstra over pluggable queues, A*.
# ------------------------------------------------------------------------------
add_library(lpq_algorithms INTERFACE)
add_library(lpq::algorithms ALIAS lpq_algorithms)
target_include_directories(lpq_algorithms INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(lpq_algorithms INTERFACE lpq::graph lpq::pq lpq::common)

# ------------------------------------------------------------------------------
# Applications
# ------------------------------------------------------------------------------
if(LPQ_BUILD_APPS)
    add_subdirectory(apps)
endif()

# ------------------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------------------
if(LPQ_BUILD_TESTS)
    # enable_testing() is called in the top-level CMakeLists, not here.
    add_subdirectory(tests)
endif()
