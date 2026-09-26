// Fuzz target: the experiment-manifest reader, which the web layer feeds
// (LPQ Atlas spec D-07: malformed experiment/config inputs).
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

#include "lpq/metrics/experiment_manifest.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::istringstream is(std::string(reinterpret_cast<const char*>(data), size));
    try {
        const auto m = lpq::read_experiment_manifest(is);
        (void)m.runs();
    } catch (const lpq::LpqError&) {
        // Rejected cleanly.
    }
    return 0;
}
