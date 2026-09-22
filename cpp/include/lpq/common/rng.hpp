// =============================================================================
// Deterministic random number generation.
//
// PRD.md section 18 requires source-target pairs to be generated
// deterministically from fixed seeds, and ESD.md section 4.5 requires an
// experiment to be fully specified by its manifest. Two consequences:
//
//   1. std::mt19937_64 is used rather than std::default_random_engine, whose
//      identity is implementation-defined and therefore not reproducible across
//      the Linux/GCC and Windows/MinGW builds this project runs on.
//
//   2. Distributions from <random> are NOT used for anything whose value ends
//      up in a manifest. std::uniform_int_distribution is also
//      implementation-defined; the same engine and seed yield different draws
//      on libstdc++ and MSVC's STL. The bounded integer draw below is written
//      out explicitly so that a query set generated on one platform is
//      byte-identical to one generated on another.
// =============================================================================
#ifndef LPQ_COMMON_RNG_HPP
#define LPQ_COMMON_RNG_HPP

#include <cmath>
#include <cstdint>
#include <random>
#include <string_view>
#include <vector>

#include "lpq/common/types.hpp"

namespace lpq {

using SeedType = std::uint64_t;

namespace detail {

/// 64x64 -> 128 bit unsigned product, split into high and low halves.
///
/// `unsigned __int128` is a GCC/Clang extension and is absent on MSVC, so a
/// portable 32-bit limb fallback is provided. Both paths compute the same
/// value, which matters: the RNG draw must not depend on the compiler.
struct WideProduct {
    std::uint64_t high;
    std::uint64_t low;
};

[[nodiscard]] inline WideProduct mul_wide(std::uint64_t a, std::uint64_t b) noexcept {
#if defined(__SIZEOF_INT128__)
    // __int128 is a compiler extension, so -Wpedantic objects to it. The guard
    // above already restricts it to compilers that document it; the diagnostic
    // is suppressed here rather than project-wide so that genuine pedantic
    // violations elsewhere still fail the build.
#    if defined(__GNUC__) || defined(__clang__)
#        pragma GCC diagnostic push
#        pragma GCC diagnostic ignored "-Wpedantic"
#    endif
    const auto m = static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b);
#    if defined(__GNUC__) || defined(__clang__)
#        pragma GCC diagnostic pop
#    endif
    return WideProduct{static_cast<std::uint64_t>(m >> 64), static_cast<std::uint64_t>(m)};
#else
    const std::uint64_t a_lo = a & 0xFFFFFFFFULL;
    const std::uint64_t a_hi = a >> 32;
    const std::uint64_t b_lo = b & 0xFFFFFFFFULL;
    const std::uint64_t b_hi = b >> 32;

    const std::uint64_t p0 = a_lo * b_lo;
    const std::uint64_t p1 = a_lo * b_hi;
    const std::uint64_t p2 = a_hi * b_lo;
    const std::uint64_t p3 = a_hi * b_hi;

    const std::uint64_t carry = ((p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL)) >> 32;
    const std::uint64_t low   = p0 + (p1 << 32) + (p2 << 32);
    const std::uint64_t high  = p3 + (p1 >> 32) + (p2 >> 32) + carry;
    return WideProduct{high, low};
#endif
}

}  // namespace detail

/// Reproducible random source. Portable across standard-library implementations
/// for every method defined here.
class Rng {
public:
    explicit Rng(SeedType seed) noexcept : engine_(seed), seed_(seed) {}

    [[nodiscard]] SeedType seed() const noexcept { return seed_; }

    /// Raw 64-bit draw.
    [[nodiscard]] std::uint64_t next_u64() noexcept { return engine_(); }

    /// Uniform integer in [0, bound). Returns 0 when `bound == 0`.
    ///
    /// Lemire's multiply-shift method with rejection: unbiased, branch-free in
    /// the common case, and -- unlike std::uniform_int_distribution --
    /// specified here rather than by the standard library, so results are
    /// portable.
    [[nodiscard]] std::uint64_t next_below(std::uint64_t bound) noexcept {
        if (bound == 0) {
            return 0;
        }
        // Rejection threshold: (2^64 - bound) % bound, computed without 2^64.
        const std::uint64_t threshold = (~bound + 1U) % bound;
        detail::WideProduct p = detail::mul_wide(next_u64(), bound);
        while (p.low < threshold) {
            p = detail::mul_wide(next_u64(), bound);
        }
        return p.high;
    }

    /// Uniform integer in [lo, hi] inclusive.
    [[nodiscard]] std::uint64_t next_in_range(std::uint64_t lo, std::uint64_t hi) noexcept {
        return lo + next_below(hi - lo + 1U);
    }

    /// Uniform node id in [0, node_count).
    [[nodiscard]] NodeId next_node(std::size_t node_count) noexcept {
        return static_cast<NodeId>(next_below(static_cast<std::uint64_t>(node_count)));
    }

    /// Uniform double in [0, 1).
    ///
    /// Built from the top 53 bits so every value is exactly representable and
    /// the mapping is identical on any IEEE-754 platform.
    [[nodiscard]] double next_unit() noexcept {
        return static_cast<double>(next_u64() >> 11) * 0x1.0p-53;
    }

    /// Uniform double in [lo, hi).
    [[nodiscard]] double next_double(double lo, double hi) noexcept {
        return lo + next_unit() * (hi - lo);
    }

    /// Standard normal draw via the Box-Muller transform.
    ///
    /// Box-Muller rather than std::normal_distribution for the portability
    /// reason above; the cached second variate keeps the cost at one transform
    /// per two draws.
    [[nodiscard]] double next_normal() noexcept {
        if (has_cached_normal_) {
            has_cached_normal_ = false;
            return cached_normal_;
        }
        double u1 = next_unit();
        // Guard the logarithm: next_unit() can legitimately return exactly 0.
        if (u1 <= 0.0) {
            u1 = 0x1.0p-53;
        }
        const double u2  = next_unit();
        const double r   = std::sqrt(-2.0 * std::log(u1));
        const double th  = 6.283185307179586476925286766559 * u2;  // 2*pi
        cached_normal_     = r * std::sin(th);
        has_cached_normal_ = true;
        return r * std::cos(th);
    }

    /// In-place Fisher-Yates shuffle. Portable for the same reason as above.
    template <typename T>
    void shuffle(std::vector<T>& v) noexcept {
        for (std::size_t i = v.size(); i > 1; --i) {
            const auto j = static_cast<std::size_t>(next_below(static_cast<std::uint64_t>(i)));
            using std::swap;
            swap(v[i - 1], v[j]);
        }
    }

private:
    std::mt19937_64 engine_;
    SeedType        seed_;
    double          cached_normal_     = 0.0;
    bool            has_cached_normal_ = false;
};

/// Derives a reproducible sub-seed from a master seed and a label.
///
/// Lets one manifest seed drive several independent streams (query generation,
/// weight generation, model initialisation) without those streams sharing
/// state. Uses the FNV-1a hash of the label mixed through SplitMix64, so the
/// derivation is stable across platforms and compilers.
[[nodiscard]] inline SeedType derive_seed(SeedType master, std::string_view label) noexcept {
    std::uint64_t h = 1469598103934665603ULL;  // FNV-1a offset basis
    for (const char c : label) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 1099511628211ULL;                 // FNV-1a prime
    }
    std::uint64_t z = master + h + 0x9E3779B97F4A7C15ULL;  // SplitMix64 finalizer
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

}  // namespace lpq

#endif  // LPQ_COMMON_RNG_HPP
