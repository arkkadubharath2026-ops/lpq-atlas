// =============================================================================
// A hierarchical bitmap over bucket indices: "first occupied bucket at or after
// i" in O(log64 n) word operations. Shared by the vector-backed substrate and
// the pooled bucket queue.
// =============================================================================
#ifndef LPQ_LPQ_BUCKET_BITMAP_HPP
#define LPQ_LPQ_BUCKET_BITMAP_HPP

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lpq {

/// A set of bucket indices that can answer "first member at or after i" in
/// O(levels) word operations, levels = ceil(log64(n)).
///
/// This is what lets the substrate use NARROW buckets. The original stepped past
/// empty buckets one at a time, which made a narrow bucket expensive exactly
/// when it was cheap to scan (a key range of R spans R / width buckets, most of
/// them empty on a road network). With the bitmap the cost of skipping a gap is
/// independent of the gap's length, so the bucket width can be chosen to
/// minimise scanning rather than to bound stepping.
class BucketBitmap {
public:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    BucketBitmap() = default;
    explicit BucketBitmap(std::size_t bits) { reset(bits); }

    void reset(std::size_t bits) {
        levels_.clear();
        std::size_t n = bits;
        do {
            n = (n + 63U) / 64U;
            levels_.emplace_back(n, 0ULL);
        } while (n > 1U);
    }

    void set(std::size_t i) noexcept {
        for (auto& level : levels_) {
            std::uint64_t& word = level[i >> 6];
            const bool     was_empty = (word == 0);
            word |= (1ULL << (i & 63U));
            if (!was_empty) {
                return;  // the parents already say "something is here"
            }
            i >>= 6;
        }
    }

    void clear(std::size_t i) noexcept {
        for (auto& level : levels_) {
            std::uint64_t& word = level[i >> 6];
            word &= ~(1ULL << (i & 63U));
            if (word != 0) {
                return;
            }
            i >>= 6;
        }
    }

    [[nodiscard]] bool test(std::size_t i) const noexcept {
        return ((levels_[0][i >> 6] >> (i & 63U)) & 1ULL) != 0;
    }

    /// The smallest member >= i, or npos.
    [[nodiscard]] std::size_t find_next(std::size_t i) const noexcept {
        std::size_t l = 0;
        while (l < levels_.size()) {
            const std::size_t w = i >> 6;
            if (w >= levels_[l].size()) {
                return npos;
            }
            const std::uint64_t word = levels_[l][w] & (~0ULL << (i & 63U));
            if (word != 0) {
                i = (w << 6) + static_cast<std::size_t>(std::countr_zero(word));
                while (l > 0) {
                    --l;
                    i = (i << 6) + static_cast<std::size_t>(std::countr_zero(levels_[l][i]));
                }
                return i;
            }
            i = w + 1;
            ++l;
        }
        return npos;
    }

    void clear_all() noexcept {
        for (auto& level : levels_) {
            std::fill(level.begin(), level.end(), 0ULL);
        }
    }

    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        std::size_t b = 0;
        for (const auto& level : levels_) {
            b += level.capacity() * sizeof(std::uint64_t);
        }
        return b;
    }

private:
    std::vector<std::vector<std::uint64_t>> levels_;
};

}  // namespace lpq

#endif  // LPQ_LPQ_BUCKET_BITMAP_HPP
