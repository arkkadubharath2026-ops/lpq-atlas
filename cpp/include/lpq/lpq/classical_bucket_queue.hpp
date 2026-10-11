// =============================================================================
// The classical bucket queue: the substrate with every learned component off.
//
// This is the "bucketing alone" rung of the ablation ladder. BucketSubstrate
// already was this queue (its header says so), but it was never available as a
// benchmark row, so the project could only compare a learned queue against
// queues with a different structure and had to infer what bucketing alone buys.
// Exposing it separates three effects that were previously fused:
//
//   radix_heap  ->  bucket_queue : a different classical structure
//   bucket_queue -> lpq          : the cost of the candidate-window machinery
//   lpq -> lpq_model             : the value of a trained scorer
//
// It scans the current bucket for the minimum on every pop, exactly the
// fallback path of the learned queue, and maintains no front.
// =============================================================================
#ifndef LPQ_LPQ_CLASSICAL_BUCKET_QUEUE_HPP
#define LPQ_LPQ_CLASSICAL_BUCKET_QUEUE_HPP

#include <cstddef>
#include <cstdint>

#include "lpq/common/types.hpp"
#include "lpq/lpq/bucket_substrate.hpp"
#include "lpq/pq/priority_queue.hpp"

namespace lpq {

template <WeightType W>
    requires(bucket_substrate_supports<W>())
class ClassicalBucketQueue {
public:
    using KeyType   = DistanceType<W>;
    using Substrate = BucketSubstrate<W>;

    static constexpr bool kSupportsDecreaseKey = false;

    ClassicalBucketQueue() : ClassicalBucketQueue(1) {}
    explicit ClassicalBucketQueue(KeyType bucket_width,
                                  std::size_t ring_size = Substrate::kBucketCount)
        : substrate_(bucket_width, ring_size) {
        substrate_.set_front_size(0);  // no front: a plain bucket queue
    }

    void push(NodeId node, KeyType key) { substrate_.push(node, key); }
    [[nodiscard]] QueueEntry<KeyType> pop() { return substrate_.pop(); }
    [[nodiscard]] QueueEntry<KeyType> top() const { return substrate_.top(); }
    [[nodiscard]] bool                empty() const noexcept { return substrate_.empty(); }
    [[nodiscard]] std::size_t         size() const noexcept { return substrate_.size(); }
    void                              clear() noexcept { substrate_.clear(); }

    [[nodiscard]] std::uint64_t comparison_count() const noexcept {
        return substrate_.comparison_count();
    }
    void reset_comparison_count() noexcept { substrate_.reset_comparison_count(); }
    void reserve_nodes(std::size_t n) { substrate_.reserve_nodes(n); }

    [[nodiscard]] Substrate&       substrate() noexcept { return substrate_; }
    [[nodiscard]] const Substrate& substrate() const noexcept { return substrate_; }

private:
    Substrate substrate_;
};

}  // namespace lpq

#endif  // LPQ_LPQ_CLASSICAL_BUCKET_QUEUE_HPP
