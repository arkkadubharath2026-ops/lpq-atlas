// Fuzz target: the binary graph reader (LPQ Atlas spec D-07).
//
// Every malformed input must be rejected with an LpqError. A crash, a hang, an
// allocation the header did not justify, or any other exception type is a bug.
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

#include "lpq/graph/graph_io.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::istringstream is(std::string(reinterpret_cast<const char*>(data), size));
    try {
        const auto g = lpq::read_binary_graph<std::uint32_t>(is);
        // A graph that loads must be internally consistent.
        for (lpq::NodeId u = 0; u < g.graph.num_nodes(); ++u) {
            for (const auto& a : g.graph.out_arcs(u)) {
                if (a.target >= g.graph.num_nodes()) {
                    __builtin_trap();
                }
            }
        }
    } catch (const lpq::LpqError&) {
        // Rejected cleanly: the expected outcome for malformed input.
    }
    return 0;
}
