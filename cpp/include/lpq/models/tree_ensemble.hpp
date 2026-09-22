// =============================================================================
// Gradient-boosted tree ensemble (ESD.md section 12: "Optional tree-based model
// for offline comparison", PRD.md section 10: "Small gradient-boosted model
// where appropriate").
//
// -----------------------------------------------------------------------------
// Inference here, training in Python -- and why that is the right split
// -----------------------------------------------------------------------------
//
// This class evaluates a tree ensemble. It cannot fit one. That is deliberate,
// and ESD.md section 13 describes exactly this arrangement:
//
//     1. Train in Python.  2. Export parameters.  3. Convert to a minimal
//     native representation.  4. Run inference in C++.  5. Benchmark inference
//     independently.  6. Validate C++ predictions against Python predictions.
//
// Writing a gradient-boosting trainer in C++ would reimplement scikit-learn's
// histogram splitter, with its own bugs, to produce a model that is used
// offline anyway -- training is not on any timed path. What must be native is
// INFERENCE, because that runs per candidate inside the search, and a Python
// call there is forbidden by ESD.md section 13 ("do not put Python interpreter
// calls inside Dijkstra/A*").
//
// The corresponding exporter lives in python/. Step 6 of that list is not
// optional and is not assumed: the two implementations must be shown to agree
// numerically before any measured run uses the model.
//
// -----------------------------------------------------------------------------
// Why a flat array rather than pointers
// -----------------------------------------------------------------------------
//
// Nodes live in one contiguous vector and refer to children by index. A
// pointer-linked tree would scatter nodes across the heap in allocation order,
// so a root-to-leaf walk would be a chain of dependent cache misses -- and this
// walk happens once per tree per candidate. The flat form also makes the whole
// ensemble memcpy-able and bounds-checkable, which matters because the node
// indices arrive from a file.
//
// Trees are stored back to back with an index of where each begins, rather than
// as a vector of vectors, for the same locality reason: consecutive trees are
// evaluated consecutively.
//
// -----------------------------------------------------------------------------
// Cost, stated plainly
// -----------------------------------------------------------------------------
//
// Each tree is a dependent chain of unpredictable branches -- the comparison at
// every node decides the next load address, so neither the branch predictor nor
// the prefetcher helps. A 100-tree ensemble of depth 6 is 600 such steps per
// candidate against the linear model's 28 fused multiply-adds. This model is
// expected to be the accuracy reference for offline comparison, not a
// production candidate, and it is included so that claim can be tested rather
// than asserted.
// =============================================================================
#ifndef LPQ_MODELS_TREE_ENSEMBLE_HPP
#define LPQ_MODELS_TREE_ENSEMBLE_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <limits>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/features/feature_vector.hpp"
#include "lpq/models/calibration.hpp"
#include "lpq/models/prediction.hpp"
#include "lpq/models/serialization.hpp"

namespace lpq {

/// One node of a binary regression tree.
///
/// 16 bytes, so four share a cache line. A leaf is marked by
/// feature == kLeafFeature, in which case `threshold` holds the leaf value and
/// the child indices are unused -- rather than by a separate flag, which would
/// either add a byte and round the struct up to 20, or cost a second load.
struct TreeNode {
    static constexpr std::uint32_t kLeafFeature = std::numeric_limits<std::uint32_t>::max();

    std::uint32_t feature = kLeafFeature;
    /// Split point, or the leaf value when feature == kLeafFeature.
    float         threshold = 0.0F;
    std::uint32_t left      = 0;  ///< taken when x[feature] <= threshold
    std::uint32_t right     = 0;

    [[nodiscard]] bool is_leaf() const noexcept { return feature == kLeafFeature; }
};

static_assert(sizeof(TreeNode) == 16, "TreeNode is walked once per tree per candidate");

/// Additive ensemble of regression trees producing a logit.
class TreeEnsemblePredictor {
public:
    static constexpr ModelKind        kKind = ModelKind::kTreeEnsemble;
    static constexpr std::string_view kName = "tree_ensemble";

    TreeEnsemblePredictor() = default;

    // --- Construction from an exported model ---------------------------------

    /// Appends one tree, given its nodes with root at local index 0.
    ///
    /// Validates every child index and feature index before accepting the tree.
    /// These arrive from a file, and an out-of-range index would be an
    /// out-of-bounds read inside the hottest loop in the program -- which under
    /// a release build is silent corruption rather than a crash. Checking here
    /// costs one pass at load time and makes predict() provably in-bounds.
    void add_tree(const std::vector<TreeNode>& nodes) {
        if (nodes.empty()) {
            throw InvalidConfigError("a tree must have at least one node");
        }
        const std::uint32_t n = static_cast<std::uint32_t>(nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const TreeNode& node = nodes[i];
            if (node.is_leaf()) {
                if (!std::isfinite(node.threshold)) {
                    throw InvalidConfigError("tree leaf value at index " + std::to_string(i) +
                                             " is not finite");
                }
                continue;
            }
            if (node.feature >= kFeatureCount) {
                throw InvalidConfigError("tree node at index " + std::to_string(i) +
                                         " splits on feature " + std::to_string(node.feature) +
                                         ", outside the layout's " +
                                         std::to_string(kFeatureCount) + " features");
            }
            if (node.left >= n || node.right >= n) {
                throw InvalidConfigError("tree node at index " + std::to_string(i) +
                                         " refers to a child outside the tree");
            }
            // A self-referencing or upward edge would make the walk loop
            // forever. Requiring children to come later is a simple sufficient
            // condition, and every exporter this project uses emits nodes in
            // that order.
            if (node.left <= i || node.right <= i) {
                throw InvalidConfigError("tree node at index " + std::to_string(i) +
                                         " has a child that does not come after it; the walk "
                                         "would not terminate");
            }
            if (!std::isfinite(node.threshold)) {
                throw InvalidConfigError("tree split threshold at index " + std::to_string(i) +
                                         " is not finite");
            }
        }

        tree_offsets_.push_back(static_cast<std::uint32_t>(nodes_.size()));
        nodes_.insert(nodes_.end(), nodes.begin(), nodes.end());
    }

    void set_base_score(float v) {
        if (!std::isfinite(v)) {
            throw InvalidConfigError("base score must be finite");
        }
        base_score_ = v;
    }

    void set_learning_rate_shrinkage(float v) {
        if (!std::isfinite(v)) {
            throw InvalidConfigError("shrinkage must be finite");
        }
        shrinkage_ = v;
    }

    // --- Inference -----------------------------------------------------------

    [[nodiscard]] Prediction predict(const FeatureVector& features) const noexcept {
        const float z = raw_score(features);

        Prediction p;
        p.score          = z;
        p.confidence     = calibrator_.calibrate_logit(z);
        p.uncertainty    = uncertainty_from_confidence(p.confidence);
        p.estimated_rank = kUnknownRank;
        return p;
    }

    /// Sum of leaf values across the ensemble, shrunk, plus the base score.
    [[nodiscard]] float raw_score(const FeatureVector& features) const noexcept {
        double acc = 0.0;
        for (std::size_t t = 0; t < tree_offsets_.size(); ++t) {
            acc += static_cast<double>(evaluate_tree(t, features));
        }
        return static_cast<float>((acc * static_cast<double>(shrinkage_)) +
                                  static_cast<double>(base_score_));
    }

    /// No-op: this model is fitted offline. See the header.
    ///
    /// Reported honestly through supports_online_update() rather than silently
    /// discarding examples, so the online-learning experiments can exclude it
    /// instead of recording a flat learning curve that looks like a result.
    void update(const TrainingExample& example) noexcept { (void)example; }

    [[nodiscard]] bool supports_online_update() const noexcept { return false; }

    // --- Shape ---------------------------------------------------------------

    [[nodiscard]] std::size_t tree_count() const noexcept { return tree_offsets_.size(); }
    [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }

    [[nodiscard]] const TemperatureScaler& calibrator() const noexcept { return calibrator_; }
    void set_calibrator(TemperatureScaler c) noexcept { calibrator_ = c; }

    // --- Serialization -------------------------------------------------------

    void save(std::ostream& os) const {
        write_model_header(os, kKind);
        detail::put_f32(os, base_score_);
        detail::put_f32(os, shrinkage_);
        detail::put_f32(os, calibrator_.temperature());
        detail::put_u64(os, static_cast<std::uint64_t>(tree_offsets_.size()));
        for (std::uint32_t off : tree_offsets_) {
            detail::put_u32(os, off);
        }
        detail::put_u64(os, static_cast<std::uint64_t>(nodes_.size()));
        for (const TreeNode& n : nodes_) {
            detail::put_u32(os, n.feature);
            detail::put_f32(os, n.threshold);
            detail::put_u32(os, n.left);
            detail::put_u32(os, n.right);
        }
    }

    [[nodiscard]] static TreeEnsemblePredictor load(std::istream& is) {
        (void)read_model_header_expecting(is, kKind);

        TreeEnsemblePredictor m;
        m.base_score_ = detail::get_f32(is);
        m.shrinkage_  = detail::get_f32(is);
        m.calibrator_.set_temperature(detail::get_f32(is));

        const std::uint64_t tree_count = detail::get_u64(is);
        if (tree_count > kMaxTrees) {
            throw ModelSerializationError("model declares " + std::to_string(tree_count) +
                                          " trees, beyond the supported maximum of " +
                                          std::to_string(kMaxTrees));
        }
        m.tree_offsets_.resize(static_cast<std::size_t>(tree_count));
        for (std::uint32_t& off : m.tree_offsets_) {
            off = detail::get_u32(is);
        }

        const std::uint64_t node_count = detail::get_u64(is);
        if (node_count > kMaxNodes) {
            throw ModelSerializationError("model declares " + std::to_string(node_count) +
                                          " tree nodes, beyond the supported maximum of " +
                                          std::to_string(kMaxNodes));
        }
        m.nodes_.resize(static_cast<std::size_t>(node_count));
        for (TreeNode& n : m.nodes_) {
            n.feature   = detail::get_u32(is);
            n.threshold = detail::get_f32(is);
            n.left      = detail::get_u32(is);
            n.right     = detail::get_u32(is);
        }

        m.validate_loaded();
        return m;
    }

private:
    /// Caps, for the same reason get_f32_array has one: the counts are read
    /// from the file before the data, so an implausible value must not become
    /// an allocation.
    static constexpr std::uint64_t kMaxTrees = 100000;
    static constexpr std::uint64_t kMaxNodes = 50000000;

    /// Walks one tree to a leaf.
    ///
    /// Bounds are guaranteed by validation at load/insert time, so the walk
    /// itself carries no checks -- it is the hot loop. The iteration cap is a
    /// belt-and-braces guard against a malformed structure that slipped past
    /// validation; it bounds a bug to a wrong answer instead of a hang, which
    /// a benchmark run would otherwise present as an unexplained stall.
    [[nodiscard]] float evaluate_tree(std::size_t tree, const FeatureVector& x) const noexcept {
        const std::uint32_t base = tree_offsets_[tree];
        const std::uint32_t end  = (tree + 1 < tree_offsets_.size())
                                       ? tree_offsets_[tree + 1]
                                       : static_cast<std::uint32_t>(nodes_.size());
        const std::uint32_t span = end - base;

        std::uint32_t i = 0;
        for (std::uint32_t steps = 0; steps < span; ++steps) {
            const TreeNode& n = nodes_[base + i];
            if (n.is_leaf()) {
                return n.threshold;
            }
            i = (x[n.feature] <= n.threshold) ? n.left : n.right;
        }
        return 0.0F;
    }

    void validate_loaded() const {
        for (std::size_t t = 0; t < tree_offsets_.size(); ++t) {
            const std::uint32_t base = tree_offsets_[t];
            const std::uint32_t end  = (t + 1 < tree_offsets_.size())
                                           ? tree_offsets_[t + 1]
                                           : static_cast<std::uint32_t>(nodes_.size());
            if (base >= end || end > nodes_.size()) {
                throw ModelSerializationError("tree " + std::to_string(t) +
                                              " has an invalid node range");
            }
            const std::uint32_t span = end - base;
            for (std::uint32_t i = 0; i < span; ++i) {
                const TreeNode& n = nodes_[base + i];
                if (n.is_leaf()) {
                    if (!std::isfinite(n.threshold)) {
                        throw ModelSerializationError("tree file contains a non-finite leaf");
                    }
                    continue;
                }
                if (n.feature >= kFeatureCount || n.left >= span || n.right >= span ||
                    n.left <= i || n.right <= i || !std::isfinite(n.threshold)) {
                    throw ModelSerializationError(
                        "tree file contains a node whose split or children are out of range; "
                        "evaluating it would read outside the node array");
                }
            }
        }
    }

    std::vector<TreeNode>      nodes_;
    std::vector<std::uint32_t> tree_offsets_;
    float                      base_score_ = 0.0F;
    float                      shrinkage_  = 1.0F;
    TemperatureScaler          calibrator_{};
};

}  // namespace lpq

#endif  // LPQ_MODELS_TREE_ENSEMBLE_HPP
