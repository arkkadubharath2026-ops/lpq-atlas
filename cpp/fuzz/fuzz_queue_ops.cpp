// Fuzz target: priority-queue operation sequences (LPQ Atlas spec D-07).
//
// The input decodes into pushes and pops with Dijkstra's monotone usage: no
// pushed key is below the last extracted one. Every queue -- including the
// learned queue in exact mode -- must return the minimum key on each pop, with
// std::multiset as the reference.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <set>
#include <utility>

#include "lpq/lpq/learned_priority_queue.hpp"
#include "lpq/pq/binary_heap.hpp"
#include "lpq/pq/radix_heap.hpp"

namespace {

using W    = std::uint32_t;
using Dist = lpq::DistanceType<W>;

constexpr lpq::NodeId kMaxNodes = 60000;

[[noreturn]] void fail(const char* queue, const char* what, unsigned long long a,
                       unsigned long long b) {
    std::fprintf(stderr, "%s: %s (%llu vs %llu)\n", queue, what, a, b);
    __builtin_trap();
}

template <typename Queue>
void run(const char* name, Queue& q, const std::uint8_t* data, std::size_t size) {
    std::multiset<std::pair<Dist, lpq::NodeId>> ref;
    Dist        floor = 0;
    lpq::NodeId next  = 0;
    const auto  pop_and_check = [&] {
        const auto e = q.pop();
        if (e.key != ref.begin()->first) {
            fail(name, "popped key is not the minimum", e.key, ref.begin()->first);
        }
        const auto it = ref.find({e.key, e.node});
        if (it == ref.end()) {
            fail(name, "popped an entry that is not queued", e.node, e.key);
        }
        ref.erase(it);
        floor = e.key;
    };
    for (std::size_t i = 0; i + 1 < size && next < kMaxNodes; i += 2) {
        if ((data[i] & 3U) == 0 && !ref.empty()) {
            pop_and_check();
        } else {
            const Dist key =
                floor + static_cast<Dist>(data[i + 1]) * static_cast<Dist>((data[i] >> 2U) + 1U);
            q.push(next, key);
            ref.insert({key, next});
            ++next;
        }
        if (q.size() != ref.size()) {
            fail(name, "size disagrees", q.size(), ref.size());
        }
    }
    while (!ref.empty()) {
        pop_and_check();
    }
    if (!q.empty()) {
        fail(name, "not empty after draining", q.size(), 0);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    {
        lpq::BinaryHeap<Dist> q;
        q.reserve_nodes(kMaxNodes);
        run("binary_heap", q, data, size);
    }
    {
        lpq::RadixHeap<Dist> q;
        q.reserve_nodes(kMaxNodes);
        run("radix_heap", q, data, size);
    }
    {
        const Dist width = size > 0 ? static_cast<Dist>((data[0] % 16U) + 1U) : Dist{1};
        lpq::LearnedPriorityQueue<W, lpq::ConstantNodeScorer> q{
            width, lpq::CandidateWindowConfig{}, lpq::CorrectnessPolicy::exact()};
        q.reserve_nodes(kMaxNodes);
        run("lpq", q, data, size);
    }
    return 0;
}
