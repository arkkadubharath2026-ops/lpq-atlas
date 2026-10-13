// =============================================================================
// Reading experiment manifests (ESD.md sections 21 and 22).
//
// ESD.md section 21 requires the benchmark runner to support batch execution
// from experiment manifests. An experiment manifest names one dataset and
// several algorithms, queues and modes, and the runner measures every
// combination of them in a single pinned process.
//
// Running the matrix in one process is the point, not a convenience. Separate
// invocations pay separate process startup, get separate page-cache states and
// may land on different cores, and those differences are attributed to whatever
// queue happened to run in which invocation. One process, one pinning decision,
// one loaded graph, one query set: the only thing that varies between records
// is the configuration the manifest asked to vary.
//
// -----------------------------------------------------------------------------
// This is not a YAML parser, for the same reason query_manifest.hpp is not
// -----------------------------------------------------------------------------
//
// It reads the exact shape ESD.md section 22 specifies and rejects everything
// else. An unrecognised key is an error rather than something skipped, because
// a silently skipped `mode` line would run the wrong experiment and label the
// results with the right name.
//
// -----------------------------------------------------------------------------
// Refusing what the runner cannot honestly do
// -----------------------------------------------------------------------------
//
// The reader validates values against what the runner ACTUALLY supports, and
// refuses rather than degrades. This matters most for two fields:
//
//   mode: approximate  -- supported, but only meaningfully by the learned
//       queue, and only with a slack. A manifest asking for it must carry an
//       `epsilon` greater than zero, because approximate mode with no slack is
//       exact mode reported under a weaker guarantee. Classical queues in an
//       approximate matrix emit an explained unsupported row rather than exact
//       numbers wearing an approximate label -- see the runner.
//
//   collect.hardware_counters: true -- the development host exposes no CPU PMU
//       (docs/experiments/profiling.md). The request is recorded and honoured
//       as far as the platform allows, and the runner reports the counters as
//       not measured. It is NOT an error to ask, because the manifest may be
//       run later on a host that has them; it IS an error to report a number
//       that was never read.
//
// The general rule: a manifest either runs exactly what it says, or it does not
// run.
// =============================================================================
#ifndef LPQ_METRICS_EXPERIMENT_MANIFEST_HPP
#define LPQ_METRICS_EXPERIMENT_MANIFEST_HPP

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <istream>
#include <string>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/common/types.hpp"

namespace lpq {

/// Raised when an experiment manifest is malformed or asks for something the
/// runner cannot do faithfully.
class ExperimentManifestError : public LpqError {
public:
    explicit ExperimentManifestError(const std::string& what) : LpqError(what) {}
};

/// Which measurements the manifest asks to collect.
///
/// Defaults are true for the things that are always available and false for the
/// ones that depend on the host, so a manifest that omits `collect` entirely
/// gets the portable subset rather than a silent promise of counters.
struct CollectionRequest {
    bool runtime           = true;
    bool pq_operations     = true;
    bool ml_latency        = false;
    bool hardware_counters = false;
};

/// One configuration to measure: a point in the manifest's cross product.
struct ExperimentRun {
    std::string algorithm;
    std::string pq;
    std::string mode;
};

struct ExperimentManifest {
    /// The experiment's name, written into every record so a results file can
    /// be traced back to the manifest that produced it.
    std::string experiment;

    /// Dataset id, resolved by the runner to datasets/processed/<id>.lpqgraph.
    std::string dataset;

    std::vector<std::string> algorithms;
    std::vector<std::string> pqs;
    std::vector<std::string> modes;

    std::uint64_t query_count = 0;
    std::uint64_t repetitions = 0;
    std::uint64_t warmup      = 3;
    SeedType      seed        = 0;

    /// Optional explicit query manifest. When empty the runner derives the path
    /// from the dataset id and the query count, so that the common case needs
    /// no path in the file, but an experiment measuring a non-standard query
    /// set can still name one.
    std::string queries_file;

    /// Optional; only meaningful when `algorithms` contains astar.
    std::string heuristic = "euclidean";

    /// Approximation slack, required when `modes` contains approximate.
    ///
    /// Held as text rather than a double so the value written into the results
    /// is exactly the value in the file. Parsing "0.1" to a double and printing
    /// it back can produce "0.10000000000000001", and an experiment whose
    /// recorded epsilon does not match its manifest is one nobody can match up
    /// afterwards.
    std::string epsilon;

    /// Learned-queue geometry, when the experiment pins it rather than taking
    /// the runner defaults. Zero means "leave the default alone".
    ///
    /// These are here because an experiment is not reproducible without them.
    /// The candidate window and the bucket width together decide whether the
    /// window samples a bucket or covers it, and that decides whether
    /// approximation is capable of changing any outcome at all -- measured, at
    /// the derived width the slack is inert (docs/experiments/approximate-mode.md).
    /// An approximate experiment that could not state its geometry would be an
    /// experiment whose result depended on a number nobody wrote down.
    std::uint64_t lpq_window_min   = 0;
    std::uint64_t lpq_window_max   = 0;
    std::uint64_t lpq_bucket_width = 0;

    CollectionRequest collect;

    /// Every configuration to measure, in a deterministic order.
    ///
    /// Materialised at read time rather than computed by the runner so that the
    /// count is known before anything is measured -- a 60-run matrix on a large
    /// graph is worth announcing before it starts rather than discovering.
    ///
    /// Order is algorithm-major, then queue, then mode: the order the fields
    /// appear in the file, so a reader of the manifest can predict the order of
    /// the results.
    [[nodiscard]] std::vector<ExperimentRun> runs() const {
        std::vector<ExperimentRun> out;
        out.reserve(algorithms.size() * pqs.size() * modes.size());
        for (const std::string& a : algorithms) {
            for (const std::string& q : pqs) {
                for (const std::string& m : modes) {
                    out.push_back(ExperimentRun{a, q, m});
                }
            }
        }
        return out;
    }
};

namespace detail {

[[nodiscard]] inline std::string em_trim(std::string s) {
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

/// Strips a trailing `# comment`, which manifests use to explain choices.
[[nodiscard]] inline std::string em_strip_comment(const std::string& s) {
    const std::size_t hash = s.find('#');
    return hash == std::string::npos ? s : s.substr(0, hash);
}

[[nodiscard]] inline std::uint64_t em_parse_u64(const std::string& text, const std::string& field,
                                                std::size_t line_number) {
    if (text.empty()) {
        throw ExperimentManifestError("line " + std::to_string(line_number) + ": " + field +
                                      " has no value");
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            throw ExperimentManifestError("line " + std::to_string(line_number) + ": " + field +
                                          " is not a non-negative integer: '" + text + "'");
        }
        value = value * 10U + static_cast<std::uint64_t>(c - '0');
    }
    return value;
}

[[nodiscard]] inline bool em_parse_bool(const std::string& text, const std::string& field,
                                        std::size_t line_number) {
    if (text == "true") {
        return true;
    }
    if (text == "false") {
        return false;
    }
    // Not defaulted to false: a typo such as `yes` would then quietly turn a
    // requested measurement off and the results would simply lack it.
    throw ExperimentManifestError("line " + std::to_string(line_number) + ": " + field +
                                  " must be true or false, got '" + text + "'");
}

/// What the runner can actually do, kept next to the reader that enforces it.
[[nodiscard]] inline bool em_known_algorithm(const std::string& a) {
    return a == "dijkstra" || a == "astar";
}

[[nodiscard]] inline bool em_known_pq(const std::string& q) {
    return q == "binary_heap" || q == "four_ary_heap" || q == "pairing_heap" ||
           q == "fibonacci_heap" || q == "radix_heap" || q == "lpq" ||
           q == "lpq_model" || q == "lpq_online" || q == "lpq_adaptive" || q == "bucket_queue" ||
           q == "lpq_direct" || q == "lpq_direct_model" || q == "replay_floor" || q == "pooled_bucket" || q == "lpq_optimized";
}

}  // namespace detail

/// Reads an experiment manifest.
///
/// Throws `ExperimentManifestError` on anything unrecognised, on a value the
/// runner cannot honour, and on a duplicated key.
[[nodiscard]] inline ExperimentManifest read_experiment_manifest(std::istream& is) {
    ExperimentManifest m;

    // Tracked so a repeated key is an error. A manifest that sets `seed: 42`
    // and later `seed: 7` would otherwise silently take the last value, and the
    // records would name a seed the reader of the file did not expect.
    std::vector<std::string> seen_keys;

    // The list-valued key currently being accumulated into, or empty when the
    // parser is at top level. YAML block sequences put their items on the lines
    // AFTER the key, so the key has to be remembered across lines.
    std::string  current_list;
    bool         in_collect   = false;
    std::string  line;
    std::size_t  line_number  = 0;

    const auto note_key = [&seen_keys](const std::string& key, std::size_t ln) {
        if (std::find(seen_keys.begin(), seen_keys.end(), key) != seen_keys.end()) {
            throw ExperimentManifestError("line " + std::to_string(ln) + ": duplicate key '" +
                                          key + "'");
        }
        seen_keys.push_back(key);
    };

    while (std::getline(is, line)) {
        ++line_number;
        const std::string raw     = detail::em_strip_comment(line);
        const std::string trimmed = detail::em_trim(raw);
        if (trimmed.empty()) {
            continue;
        }

        // --- a list item belonging to the key above it ----------------------
        if (trimmed[0] == '-') {
            if (current_list.empty()) {
                throw ExperimentManifestError("line " + std::to_string(line_number) +
                                              ": list item with no list key above it");
            }
            const std::string value = detail::em_trim(trimmed.substr(1));
            if (value.empty()) {
                throw ExperimentManifestError("line " + std::to_string(line_number) +
                                              ": empty list item under '" + current_list + "'");
            }
            if (current_list == "algorithm") {
                if (!detail::em_known_algorithm(value)) {
                    throw ExperimentManifestError("line " + std::to_string(line_number) +
                                                  ": unknown algorithm '" + value +
                                                  "'; the runner supports dijkstra and astar");
                }
                m.algorithms.push_back(value);
            } else if (current_list == "pq") {
                if (!detail::em_known_pq(value)) {
                    throw ExperimentManifestError(
                        "line " + std::to_string(line_number) + ": unknown queue '" + value +
                        "'; the runner supports binary_heap, four_ary_heap, pairing_heap, "
                        "fibonacci_heap, radix_heap, bucket_queue, lpq, lpq_model, lpq_adaptive, "
                        "lpq_direct and lpq_direct_model");
                }
                m.pqs.push_back(value);
            } else if (current_list == "mode") {
                if (value != "exact" && value != "approximate") {
                    throw ExperimentManifestError("line " + std::to_string(line_number) +
                                                  ": unknown mode '" + value + "'");
                }
                m.modes.push_back(value);
            }
            continue;
        }

        const std::size_t colon = trimmed.find(':');
        if (colon == std::string::npos) {
            throw ExperimentManifestError("line " + std::to_string(line_number) +
                                          ": expected 'key: value', got '" + trimmed + "'");
        }
        const std::string key   = detail::em_trim(trimmed.substr(0, colon));
        const std::string value = detail::em_trim(trimmed.substr(colon + 1));

        // --- keys nested under `collect` ------------------------------------
        //
        // Detected by indentation on the raw line, because `runtime` is only
        // meaningful inside collect and a top-level `runtime:` is a mistake
        // worth reporting rather than absorbing.
        const bool indented = !raw.empty() && std::isspace(static_cast<unsigned char>(raw[0])) != 0;
        if (in_collect && indented) {
            note_key("collect." + key, line_number);
            if (key == "runtime") {
                m.collect.runtime = detail::em_parse_bool(value, key, line_number);
            } else if (key == "pq_operations") {
                m.collect.pq_operations = detail::em_parse_bool(value, key, line_number);
            } else if (key == "ml_latency") {
                m.collect.ml_latency = detail::em_parse_bool(value, key, line_number);
            } else if (key == "hardware_counters") {
                m.collect.hardware_counters = detail::em_parse_bool(value, key, line_number);
            } else {
                throw ExperimentManifestError("line " + std::to_string(line_number) +
                                              ": unknown key under collect: '" + key + "'");
            }
            continue;
        }
        in_collect   = false;
        current_list = "";

        note_key(key, line_number);

        if (key == "experiment") {
            m.experiment = value;
        } else if (key == "dataset") {
            m.dataset = value;
        } else if (key == "queries" || key == "queries_file") {
            m.queries_file = value;
        } else if (key == "heuristic") {
            m.heuristic = value;
        } else if (key == "epsilon") {
            m.epsilon = value;
        } else if (key == "lpq_bucket_width") {
            m.lpq_bucket_width = detail::em_parse_u64(value, key, line_number);
        } else if (key == "lpq_window_min") {
            m.lpq_window_min = detail::em_parse_u64(value, key, line_number);
        } else if (key == "lpq_window_max") {
            m.lpq_window_max = detail::em_parse_u64(value, key, line_number);
        } else if (key == "query_count") {
            m.query_count = detail::em_parse_u64(value, key, line_number);
        } else if (key == "repetitions") {
            m.repetitions = detail::em_parse_u64(value, key, line_number);
        } else if (key == "warmup") {
            m.warmup = detail::em_parse_u64(value, key, line_number);
        } else if (key == "seed") {
            m.seed = static_cast<SeedType>(detail::em_parse_u64(value, key, line_number));
        } else if (key == "algorithm" || key == "pq" || key == "mode") {
            if (!value.empty()) {
                throw ExperimentManifestError(
                    "line " + std::to_string(line_number) + ": '" + key +
                    "' must be a list, with each item on its own line beginning with '-'");
            }
            current_list = key;
        } else if (key == "collect") {
            if (!value.empty()) {
                throw ExperimentManifestError("line " + std::to_string(line_number) +
                                              ": 'collect' takes nested keys, not a value");
            }
            in_collect = true;
        } else {
            throw ExperimentManifestError("line " + std::to_string(line_number) +
                                          ": unknown key '" + key + "'");
        }
    }

    // --- everything a run cannot be assembled without ------------------------
    if (m.experiment.empty()) {
        throw ExperimentManifestError("manifest has no 'experiment' name");
    }
    if (m.dataset.empty()) {
        throw ExperimentManifestError("manifest has no 'dataset'");
    }
    if (m.algorithms.empty()) {
        throw ExperimentManifestError("manifest lists no algorithms");
    }
    if (m.pqs.empty()) {
        throw ExperimentManifestError("manifest lists no queues");
    }
    if (m.modes.empty()) {
        throw ExperimentManifestError("manifest lists no modes");
    }
    if (m.query_count == 0) {
        throw ExperimentManifestError("manifest has no 'query_count', or it is zero");
    }
    if (m.repetitions == 0) {
        throw ExperimentManifestError("manifest has no 'repetitions', or it is zero");
    }

    // Approximate mode is only meaningful with a slack, and a manifest that
    // omits it would otherwise run exact searches under an approximate label.
    const bool wants_approximate =
        std::find(m.modes.begin(), m.modes.end(), "approximate") != m.modes.end();
    if (wants_approximate && m.epsilon.empty()) {
        throw ExperimentManifestError(
            "mode 'approximate' requires an 'epsilon' greater than zero; without slack it is "
            "exact mode reported under a weaker guarantee");
    }
    if (!wants_approximate && !m.epsilon.empty()) {
        throw ExperimentManifestError(
            "'epsilon' is set but no mode is 'approximate', so the slack would be silently "
            "ignored and the manifest would claim an approximation it never applied");
    }

    // Half a window is not a window. Specifying one bound and leaving the other
    // at the runner default silently mixes a manifest value with a default, and
    // the resulting geometry appears in no file.
    if ((m.lpq_window_min == 0) != (m.lpq_window_max == 0)) {
        throw ExperimentManifestError(
            "lpq_window_min and lpq_window_max must be given together: setting one alone "
            "combines a manifest value with a runner default, and the geometry actually "
            "measured would then appear in no file");
    }
    if (m.lpq_window_min != 0 && m.lpq_window_max < m.lpq_window_min) {
        throw ExperimentManifestError("lpq_window_max is smaller than lpq_window_min");
    }

    // A single repetition cannot produce a median absolute deviation, and the
    // project reports median and MAD rather than a mean. Refusing here is
    // better than producing records whose dispersion field is meaningless.
    if (m.repetitions < 3) {
        throw ExperimentManifestError(
            "repetitions must be at least 3: this project reports median and MAD, and a "
            "dispersion estimate from fewer than three samples is not meaningful");
    }

    return m;
}

[[nodiscard]] inline ExperimentManifest read_experiment_manifest_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw ExperimentManifestError("cannot open experiment manifest: " + path);
    }
    return read_experiment_manifest(in);
}

}  // namespace lpq

#endif  // LPQ_METRICS_EXPERIMENT_MANIFEST_HPP
