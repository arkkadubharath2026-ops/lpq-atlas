// =============================================================================
// Reading query manifests (ESD.md section 22, PRD.md section 18).
//
// A manifest is the file the dataset pipeline writes for each converted graph,
// holding the source-target pairs every queue is measured on. PRD.md section 18
// requires those pairs to be fixed and shared across queues, because per-query
// difficulty varies by orders of magnitude: comparing two queues on different
// query sets measures the query sets.
//
// -----------------------------------------------------------------------------
// This is not a YAML parser
// -----------------------------------------------------------------------------
//
// Manifests are written by lpq_tools.data.queries in one fixed shape, and this
// reader accepts exactly that shape and rejects everything else. Pulling in a
// YAML library to read a file this project also writes would add a dependency
// to the benchmark binary in exchange for flexibility nobody wants -- a
// manifest with an unexpected structure is a manifest that was hand-edited,
// which the file's own header tells the reader not to do, since a hand edit
// cannot be reproduced.
//
// So the reader is strict on purpose. Anything it does not recognise is an
// error rather than something silently skipped, because a silently skipped
// `graph_checksum_fnv1a64` line would disable the check below.
//
// -----------------------------------------------------------------------------
// Why the checksum matters more than it looks
// -----------------------------------------------------------------------------
//
// Node ids in a manifest are DENSE CSR INDICES assigned during conversion, not
// identifiers from the source dataset. Reconvert the graph from a different
// source file, or under a different normalization policy, and index 1723 is a
// different intersection -- but the manifest still loads, every query still
// runs, and every number produced is quietly about a different set of queries.
//
// Nothing downstream could detect that, so the manifest carries the checksum of
// the graph it was generated against and `validate_against` refuses a mismatch.
// =============================================================================
#ifndef LPQ_GRAPH_QUERY_MANIFEST_HPP
#define LPQ_GRAPH_QUERY_MANIFEST_HPP

#include <cstdint>
#include <fstream>
#include <istream>
#include <string>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/types.hpp"
#include "lpq/graph/csr_graph.hpp"
#include "lpq/graph/graph_io.hpp"

namespace lpq {

/// Raised when a manifest is malformed or does not match its graph.
class QueryManifestError : public LpqError {
public:
    explicit QueryManifestError(const std::string& what) : LpqError(what) {}
};

/// One source-target pair.
struct ManifestQuery {
    NodeId source = 0;
    NodeId target = 0;
};

/// A parsed query manifest.
struct QueryManifest {
    std::string dataset_id;

    std::uint64_t node_count = 0;
    std::uint64_t seed       = 0;

    /// The checksum of the graph the pairs were generated against, or 0 when
    /// the manifest declared none.
    std::uint64_t graph_checksum = 0;
    bool          has_checksum   = false;

    /// What the manifest said its query count was, for cross-checking against
    /// the number of pairs actually present.
    std::uint64_t declared_query_count = 0;

    std::vector<ManifestQuery> queries;

    /// Refuses a manifest that was generated against a different graph.
    ///
    /// Checked against the graph's recomputed checksum rather than against a
    /// value carried alongside it, so a metadata file edited after the fact
    /// cannot make a mismatched pair appear to agree.
    template <WeightType W>
    void validate_against(const CsrGraph<W>& g) const {
        if (node_count != g.num_nodes()) {
            throw QueryManifestError(
                "query manifest was generated for a graph with " + std::to_string(node_count) +
                " nodes but this graph has " + std::to_string(g.num_nodes()));
        }

        if (has_checksum) {
            // Qualified: the member field of the same name shadows the free
            // function inside this struct.
            const std::uint64_t actual = ::lpq::graph_checksum<W>(g);
            if (actual != graph_checksum) {
                throw QueryManifestError(
                    "query manifest was generated against a different graph (manifest "
                    "checksum does not match this graph's). Node ids in a manifest are "
                    "dense indices assigned at conversion time, so the same pair means a "
                    "different query -- regenerate the manifest for this graph");
            }
        }

        for (const ManifestQuery& q : queries) {
            if (q.source >= g.num_nodes() || q.target >= g.num_nodes()) {
                throw QueryManifestError("query manifest refers to a node outside the graph");
            }
        }
    }
};

namespace detail {

/// Strips a trailing comment and surrounding whitespace.
[[nodiscard]] inline std::string trim(std::string s) {
    const std::size_t hash = s.find('#');
    if (hash != std::string::npos) {
        s.erase(hash);
    }
    const std::size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

[[nodiscard]] inline std::uint64_t parse_u64(const std::string& text, const std::string& field,
                                             std::size_t line) {
    if (text.empty()) {
        throw QueryManifestError("line " + std::to_string(line) + ": '" + field +
                                 "' has no value");
    }
    try {
        std::size_t consumed = 0;
        // Parsed as unsigned long long rather than via a signed type: the
        // checksum is a full-width 64-bit value and a signed parse would
        // overflow on about half of them.
        const unsigned long long value = std::stoull(text, &consumed);
        if (consumed != text.size()) {
            throw QueryManifestError("line " + std::to_string(line) + ": '" + field +
                                     "' is not an integer: '" + text + "'");
        }
        return static_cast<std::uint64_t>(value);
    } catch (const QueryManifestError&) {
        throw;
    } catch (const std::exception&) {
        throw QueryManifestError("line " + std::to_string(line) + ": '" + field +
                                 "' is not an integer: '" + text + "'");
    }
}

}  // namespace detail

/// Reads a manifest in the shape lpq_tools.data.queries writes.
[[nodiscard]] inline QueryManifest read_query_manifest(std::istream& is) {
    QueryManifest manifest;

    bool        in_queries = false;
    bool        have_open_pair = false;
    std::string line;
    std::size_t line_number = 0;

    while (std::getline(is, line)) {
        ++line_number;
        const std::string raw     = line;
        const std::string trimmed = detail::trim(line);
        if (trimmed.empty()) {
            continue;
        }

        // The queries list runs to the end of the file, so a top-level key is
        // recognised by being unindented.
        const bool indented = !raw.empty() && (raw[0] == ' ' || raw[0] == '\t' || raw[0] == '-');

        if (!in_queries || !indented) {
            const std::size_t colon = trimmed.find(':');
            if (colon == std::string::npos) {
                throw QueryManifestError("line " + std::to_string(line_number) +
                                         ": expected 'key: value'");
            }
            const std::string key   = trimmed.substr(0, colon);
            const std::string value = detail::trim(trimmed.substr(colon + 1));

            if (key == "queries") {
                in_queries = true;
                continue;
            }

            in_queries = false;
            if (key == "dataset_id") {
                manifest.dataset_id = value;
            } else if (key == "node_count") {
                manifest.node_count = detail::parse_u64(value, key, line_number);
            } else if (key == "seed") {
                manifest.seed = detail::parse_u64(value, key, line_number);
            } else if (key == "query_count") {
                manifest.declared_query_count = detail::parse_u64(value, key, line_number);
            } else if (key == "graph_checksum_fnv1a64") {
                // An absent checksum is written as a YAML null. Treated as "no
                // checksum" rather than as zero, since zero is a legal value.
                if (value != "null" && value != "~" && !value.empty()) {
                    manifest.graph_checksum = detail::parse_u64(value, key, line_number);
                    manifest.has_checksum   = true;
                }
            } else if (key == "generator" || key == "selection" ||
                       key == "reachability_filtered") {
                // Descriptive fields, recorded for the reader of the file.
            } else {
                throw QueryManifestError("line " + std::to_string(line_number) +
                                         ": unrecognised key '" + key +
                                         "'. This reader accepts only the manifest shape "
                                         "lpq_tools.data.queries writes");
            }
            continue;
        }

        // Inside the queries list: "- source: N" then "  target: N".
        std::string entry = trimmed;
        bool        starts_item = false;
        if (!entry.empty() && entry[0] == '-') {
            starts_item = true;
            entry       = detail::trim(entry.substr(1));
        }

        const std::size_t colon = entry.find(':');
        if (colon == std::string::npos) {
            throw QueryManifestError("line " + std::to_string(line_number) +
                                     ": expected 'source:' or 'target:'");
        }
        const std::string key   = entry.substr(0, colon);
        const std::string value = detail::trim(entry.substr(colon + 1));

        if (key == "source") {
            if (!starts_item) {
                throw QueryManifestError("line " + std::to_string(line_number) +
                                         ": 'source' must begin a list item");
            }
            if (have_open_pair) {
                throw QueryManifestError("line " + std::to_string(line_number) +
                                         ": previous query has no target");
            }
            const std::uint64_t v = detail::parse_u64(value, key, line_number);
            if (v > kMaxNodeCount) {
                throw QueryManifestError("line " + std::to_string(line_number) +
                                         ": source exceeds the addressable node range");
            }
            manifest.queries.push_back(ManifestQuery{static_cast<NodeId>(v), 0});
            have_open_pair = true;
        } else if (key == "target") {
            if (!have_open_pair) {
                throw QueryManifestError("line " + std::to_string(line_number) +
                                         ": 'target' without a preceding 'source'");
            }
            const std::uint64_t v = detail::parse_u64(value, key, line_number);
            if (v > kMaxNodeCount) {
                throw QueryManifestError("line " + std::to_string(line_number) +
                                         ": target exceeds the addressable node range");
            }
            manifest.queries.back().target = static_cast<NodeId>(v);
            have_open_pair                 = false;
        } else {
            throw QueryManifestError("line " + std::to_string(line_number) +
                                     ": unrecognised query field '" + key + "'");
        }
    }

    if (have_open_pair) {
        throw QueryManifestError("the last query has a source but no target");
    }
    if (manifest.queries.empty()) {
        throw QueryManifestError("query manifest contains no queries");
    }
    // A truncated manifest parses perfectly and yields a shorter, entirely
    // valid query set -- which is the failure this cross-check exists for.
    if (manifest.declared_query_count != 0 &&
        manifest.declared_query_count != manifest.queries.size()) {
        throw QueryManifestError("manifest declares " +
                                 std::to_string(manifest.declared_query_count) +
                                 " queries but holds " +
                                 std::to_string(manifest.queries.size()));
    }
    return manifest;
}

[[nodiscard]] inline QueryManifest read_query_manifest_file(const std::string& path) {
    std::ifstream is(path);
    if (!is) {
        throw QueryManifestError("cannot open query manifest '" + path + "'");
    }
    return read_query_manifest(is);
}

}  // namespace lpq

#endif  // LPQ_GRAPH_QUERY_MANIFEST_HPP
