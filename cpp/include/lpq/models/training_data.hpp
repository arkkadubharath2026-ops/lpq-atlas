// =============================================================================
// The offline training set: what it holds, and why it holds that (PRD.md
// section 11, ESD.md section 39 milestone 15).
//
// The learned queue's deployable scorer is `StaticNodeScorer`: it runs a model
// once per node, before any timed region, and thereafter costs one array read
// per push. Its input is therefore the STATIC feature group only -- degree and
// adjacent-weight statistics -- because those are the only features constant
// per node. This file is the training set for exactly that model.
//
// -----------------------------------------------------------------------------
// What the label means
// -----------------------------------------------------------------------------
//
// The substrate groups queued entries into buckets of equal key width, and the
// scorer's job is to push the entry that will be extracted next towards the
// front of its bucket. So the question a model must answer is:
//
//     given this node's static properties, how often is it the first entry
//     extracted from the bucket it lands in?
//
// The label is derived from the AUTHORITATIVE search, never from the learned
// queue. A reference Dijkstra settles nodes in nondecreasing distance order;
// assigning each settled node to `dist / bucket_width` and taking the earliest
// settled node in each bucket gives, exactly, the entry the substrate must
// extract first from that bucket. Nothing about the learned queue's behaviour
// enters the labels, which is what stops a model from being trained on its own
// output.
//
// -----------------------------------------------------------------------------
// Why the rows are aggregated
// -----------------------------------------------------------------------------
//
// One row per (node, query) is the obvious encoding and the wrong one. The
// features are static, so every row for a given node is IDENTICAL in its inputs
// and differs only in its label. On `dimacs_ny` over 100 queries that is ~26
// million rows of which 264,346 are distinct: about 3 GB to say what fits in 30.
//
// Aggregating to (features, positives, occurrences) per node is LOSSLESS for
// any model that sees only these features. A weighted logistic regression over
// the aggregate has the same likelihood, to the last bit, as one over the
// expanded rows, because the expanded rows are identical in x and differ only
// in y -- and sum(y) with a count is a sufficient statistic for that. It is
// lossy only for a model that could distinguish two occurrences of the same
// node, and no model over static features can.
//
// The aggregate also makes the class imbalance legible instead of hidden.
// Exactly one node per bucket per query is positive, so the positive rate is
// roughly buckets/settled -- a number a trainer must know before it reports an
// accuracy, because predicting "never" scores well against it.
// =============================================================================
#ifndef LPQ_MODELS_TRAINING_DATA_HPP
#define LPQ_MODELS_TRAINING_DATA_HPP

#include <cstdint>
#include <fstream>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/types.hpp"
#include "lpq/features/feature_vector.hpp"
#include "lpq/models/serialization.hpp"

namespace lpq {

/// "LPQTRAIN" as a little-endian 64-bit value.
inline constexpr std::uint64_t kTrainingSetMagic = 0x4E4941525451504CULL;

/// Incremented when this container changes, independently of the feature layout.
inline constexpr std::uint32_t kTrainingSetFormatVersion = 1;

class TrainingSetError : public LpqError {
public:
    explicit TrainingSetError(const std::string& what) : LpqError(what) {}
};

/// One node's static features and how often it won its bucket.
struct TrainingRow {
    FeatureVector features{};

    /// Times this node was the first extraction from its bucket.
    std::uint32_t positives = 0;

    /// Times this node was settled at all, across every query in the set.
    ///
    /// Kept rather than derived so a node that never won is distinguishable
    /// from one that never appeared. Those are different observations and a
    /// trainer that conflated them would be fitting to absent data.
    std::uint32_t occurrences = 0;
};

/// A whole training set plus the provenance needed to trust it.
struct TrainingSet {
    /// The dataset the features came from. A model trained on one graph and
    /// applied to another is a legitimate experiment, but it must be a
    /// deliberate one, so the origin travels with the data.
    std::string dataset_id;

    /// The graph checksum, so a reconverted graph is detected rather than
    /// silently retrained against. Node ids here are dense CSR indices assigned
    /// at conversion time and mean nothing without the graph that assigned them.
    std::uint64_t graph_checksum = 0;

    /// The bucket width the labels were derived under.
    ///
    /// The labels are meaningless without it: "first out of its bucket" is a
    /// different question at every width, and this document's own measurements
    /// show the width changes queue behaviour by more than an order of
    /// magnitude (docs/experiments/approximate-mode.md).
    std::uint64_t bucket_width = 0;

    /// How many queries were aggregated.
    std::uint64_t query_count = 0;

    /// The seed of the query manifest, so the set can be regenerated exactly.
    std::uint64_t query_seed = 0;

    std::uint32_t feature_layout_version = static_cast<std::uint32_t>(kFeatureLayoutVersion);

    std::vector<TrainingRow> rows;

    /// Total positive and observed counts, for the imbalance a trainer must
    /// report alongside any accuracy.
    [[nodiscard]] std::uint64_t total_positives() const noexcept {
        std::uint64_t n = 0;
        for (const TrainingRow& r : rows) {
            n += r.positives;
        }
        return n;
    }

    [[nodiscard]] std::uint64_t total_occurrences() const noexcept {
        std::uint64_t n = 0;
        for (const TrainingRow& r : rows) {
            n += r.occurrences;
        }
        return n;
    }
};

namespace detail {

// put_f32 / get_f32 come from serialization.hpp: the model files and the
// training sets share one little-endian encoding, so a change to how a float is
// written cannot apply to one and not the other.

inline void put_string(std::ostream& os, const std::string& s) {
    put_u32(os, static_cast<std::uint32_t>(s.size()));
    os.write(s.data(), static_cast<std::streamsize>(s.size()));
}

[[nodiscard]] inline std::string get_string(std::istream& is) {
    const std::uint32_t n = get_u32(is);
    // Bounded so a corrupt length cannot ask for an arbitrary allocation before
    // the read fails.
    if (n > 4096U) {
        throw TrainingSetError("implausible string length in training set: " +
                               std::to_string(n));
    }
    std::string s(n, '\0');
    is.read(s.data(), static_cast<std::streamsize>(n));
    if (!is) {
        throw TrainingSetError("training set truncated inside a string");
    }
    return s;
}

}  // namespace detail

inline void write_training_set(std::ostream& os, const TrainingSet& set) {
    detail::put_u64(os, kTrainingSetMagic);
    detail::put_u32(os, kTrainingSetFormatVersion);
    detail::put_u32(os, set.feature_layout_version);
    detail::put_u32(os, static_cast<std::uint32_t>(kFeatureCount));
    detail::put_string(os, set.dataset_id);
    detail::put_u64(os, set.graph_checksum);
    detail::put_u64(os, set.bucket_width);
    detail::put_u64(os, set.query_count);
    detail::put_u64(os, set.query_seed);
    detail::put_u64(os, static_cast<std::uint64_t>(set.rows.size()));

    for (const TrainingRow& r : set.rows) {
        for (std::size_t i = 0; i < kFeatureCount; ++i) {
            detail::put_f32(os, r.features[i]);
        }
        detail::put_u32(os, r.positives);
        detail::put_u32(os, r.occurrences);
    }
    if (!os) {
        throw TrainingSetError("failed while writing a training set");
    }
}

namespace detail {

/// The body of read_training_set, wrapped below.
[[nodiscard]] inline TrainingSet read_training_set_body(std::istream& is) {
    TrainingSet set;

    if (detail::get_u64(is) != kTrainingSetMagic) {
        throw TrainingSetError(
            "not a training set: wrong magic. A file that is not a training set must be "
            "rejected as such rather than interpreted as one");
    }
    const std::uint32_t format = detail::get_u32(is);
    if (format != kTrainingSetFormatVersion) {
        throw TrainingSetError("training set format version " + std::to_string(format) +
                               " cannot be read by this build (expected " +
                               std::to_string(kTrainingSetFormatVersion) + ")");
    }

    set.feature_layout_version = detail::get_u32(is);
    const std::uint32_t count  = detail::get_u32(is);

    // The same check model files carry, for the same reason. A set built against
    // one feature layout and trained against another would put every value in
    // the wrong column and produce a confident, plausible, wrong model, and
    // nothing downstream could detect it.
    if (set.feature_layout_version != static_cast<std::uint32_t>(kFeatureLayoutVersion)) {
        throw TrainingSetError(
            "training set was built against feature layout version " +
            std::to_string(set.feature_layout_version) + " but this build uses " +
            std::to_string(kFeatureLayoutVersion) +
            "; every feature would be read from the wrong column");
    }
    if (count != static_cast<std::uint32_t>(kFeatureCount)) {
        throw TrainingSetError("training set has " + std::to_string(count) +
                               " features but this build expects " +
                               std::to_string(kFeatureCount));
    }

    set.dataset_id     = detail::get_string(is);
    set.graph_checksum = detail::get_u64(is);
    set.bucket_width   = detail::get_u64(is);
    set.query_count    = detail::get_u64(is);
    set.query_seed     = detail::get_u64(is);

    const std::uint64_t rows = detail::get_u64(is);
    if (!is) {
        throw TrainingSetError("training set truncated in its header");
    }

    set.rows.reserve(static_cast<std::size_t>(rows));
    for (std::uint64_t i = 0; i < rows; ++i) {
        TrainingRow r;
        for (std::size_t f = 0; f < kFeatureCount; ++f) {
            r.features[f] = detail::get_f32(is);
        }
        r.positives   = detail::get_u32(is);
        r.occurrences = detail::get_u32(is);
        if (!is) {
            throw TrainingSetError("training set truncated at row " + std::to_string(i) +
                                   " of a declared " + std::to_string(rows));
        }
        // A node cannot win more buckets than it was settled in. This catches a
        // corrupt file and, more usefully, a labelling bug in the collector,
        // which would otherwise produce a set that trains without complaint.
        if (r.positives > r.occurrences) {
            throw TrainingSetError("row " + std::to_string(i) + " claims " +
                                   std::to_string(r.positives) + " positives from " +
                                   std::to_string(r.occurrences) + " occurrences");
        }
        set.rows.push_back(r);
    }
    return set;
}

}  // namespace detail

/// Reads a training set.
///
/// The primitive readers are shared with the model files and raise
/// ModelSerializationError on a short read, whose message says "model file".
/// That is the wrong type for a caller reading a training set and the wrong
/// noun in the message, so it is translated here rather than leaked -- a
/// truncated training set should not report itself as a truncated model.
[[nodiscard]] inline TrainingSet read_training_set(std::istream& is) {
    try {
        return detail::read_training_set_body(is);
    } catch (const ModelSerializationError& e) {
        throw TrainingSetError(std::string("training set is truncated or malformed: ") +
                               e.what());
    }
}

[[nodiscard]] inline TrainingSet read_training_set_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw TrainingSetError("cannot open training set: " + path);
    }
    return read_training_set(in);
}

inline void write_training_set_file(const std::string& path, const TrainingSet& set) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw TrainingSetError("cannot open training set for writing: " + path);
    }
    write_training_set(out, set);
}

}  // namespace lpq

#endif  // LPQ_MODELS_TRAINING_DATA_HPP
