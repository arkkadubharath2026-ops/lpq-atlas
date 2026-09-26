// =============================================================================
// Learned-queue geometry derived from a graph.
//
// Shared by lpq_benchmark and lpq_run so that a single interactive run and a
// benchmark row configure the learned queue identically. Two apps each
// deriving the bucket width their own way would let the interactive
// visualiser show a queue the benchmark never measured.
// =============================================================================
#ifndef LPQ_LPQ_QUEUE_CONFIG_HPP
#define LPQ_LPQ_QUEUE_CONFIG_HPP

#include <cstddef>
#include <cstdint>

#include "lpq/graph/graph_metadata.hpp"

namespace lpq {

/// The substrate bucket width to use.
///
/// `requested` wins when non-zero. Otherwise the width is chosen so the bucket
/// array spans roughly the range of edge weights: a bucket then holds entries
/// of genuinely similar key rather than everything, and a width of 1 on a road
/// network whose weights run to six figures would put almost every entry in
/// the overflow ring and measure the overflow path rather than the queue.
[[nodiscard]] inline std::uint64_t derive_bucket_width(const GraphMetadata& meta,
                                                       std::size_t    bucket_count,
                                                       std::uint64_t  requested) noexcept {
    if (requested != 0) {
        return requested;
    }
    const auto span = static_cast<std::uint64_t>(meta.max_weight > 0.0 ? meta.max_weight : 1.0);
    const auto buckets = static_cast<std::uint64_t>(bucket_count);
    const std::uint64_t width = (span + buckets - 1U) / buckets;
    return width == 0 ? 1 : width;
}

}  // namespace lpq

#endif  // LPQ_LPQ_QUEUE_CONFIG_HPP
