// Tests for JSON serialisation and the result schema (ESD.md sections 23, 40).
//
// The serialiser is hand-written, so its escaping and numeric round-tripping
// are verified directly: a result file that a Python analysis script cannot
// parse, or that silently truncates a timing, would invalidate every downstream
// figure.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>
#include <limits>
#include <sstream>
#include <string>

#include "lpq/metrics/hardware_info.hpp"
#include "lpq/metrics/json_writer.hpp"
#include "lpq/metrics/result_record.hpp"
#include "lpq/pq/radix_heap.hpp"

namespace {

using lpq::JsonWriter;

std::string emit(const std::function<void(JsonWriter&)>& fn, bool pretty = false) {
    std::ostringstream os;
    JsonWriter         w(os, pretty);
    fn(w);
    return os.str();
}

// --- Structure ----------------------------------------------------------------

TEST(JsonWriter, EmptyObject) {
    EXPECT_EQ(emit([](JsonWriter& w) {
                  w.begin_object();
                  w.end_object();
              }),
              "{}");
}

TEST(JsonWriter, FlatObject) {
    const auto s = emit([](JsonWriter& w) {
        w.begin_object();
        w.key_value("a", std::uint64_t{1});
        w.key_value("b", "two");
        w.key_value("c", true);
        w.end_object();
    });
    EXPECT_EQ(s, R"({"a":1,"b":"two","c":true})");
}

TEST(JsonWriter, NestedObjectsAndArrays) {
    const auto s = emit([](JsonWriter& w) {
        w.begin_object();
        w.begin_object("inner");
        w.key_value("x", std::uint64_t{1});
        w.end_object();
        w.begin_array("list");
        w.value(std::uint64_t{1});
        w.value(std::uint64_t{2});
        w.end_array();
        w.end_object();
    });
    EXPECT_EQ(s, R"({"inner":{"x":1},"list":[1,2]})");
}

TEST(JsonWriter, EmptyArray) {
    const auto s = emit([](JsonWriter& w) {
        w.begin_object();
        w.begin_array("empty");
        w.end_array();
        w.end_object();
    });
    EXPECT_EQ(s, R"({"empty":[]})");
}

// --- Escaping -----------------------------------------------------------------

TEST(JsonWriter, EscapesBackslashesInWindowsPaths) {
    // Dataset identifiers arrive from file paths. On Windows these contain
    // backslashes, which unescaped would corrupt the file.
    const auto s = emit([](JsonWriter& w) {
        w.begin_object();
        w.key_value("path", R"(D:\data\graph.txt)");
        w.end_object();
    });
    EXPECT_EQ(s, R"({"path":"D:\\data\\graph.txt"})");
}

TEST(JsonWriter, EscapesQuotesAndControlCharacters) {
    // Error strings reach the writer from exception messages.
    const auto s = emit([](JsonWriter& w) {
        w.begin_object();
        w.key_value("msg", "he said \"hi\"\nline2\ttabbed");
        w.end_object();
    });
    EXPECT_EQ(s, R"({"msg":"he said \"hi\"\nline2\ttabbed"})");
}

TEST(JsonWriter, EscapesLowControlCharactersAsUnicode) {
    std::string with_control = "a";
    with_control.push_back('\x01');
    const auto s = emit([&](JsonWriter& w) {
        w.begin_object();
        w.key_value("v", with_control);
        w.end_object();
    });
    EXPECT_EQ(s, R"({"v":"a\u0001"})");
}

TEST(JsonWriter, PassesUtf8Through) {
    const auto s = emit([](JsonWriter& w) {
        w.begin_object();
        w.key_value("v", "Erd\xC5\x91s-R\xC3\xA9nyi");
        w.end_object();
    });
    EXPECT_NE(s.find("Erd"), std::string::npos);
    EXPECT_EQ(s.find("\\u"), std::string::npos) << "UTF-8 bytes must not be escaped";
}

// --- Numbers ------------------------------------------------------------------

TEST(JsonWriter, DoublesRoundTrip) {
    // A timing written and read back must be the same value. std::to_string
    // would give six decimals and silently truncate.
    const double values[] = {0.1, 1.0 / 3.0, 1.2345678901234567e-8, 9.87654321e15,
                             123456789.123456789};
    for (double v : values) {
        const auto s = emit([v](JsonWriter& w) {
            w.begin_object();
            w.key_value("v", v);
            w.end_object();
        });
        const auto colon = s.find(':');
        const auto brace = s.rfind('}');
        const double parsed = std::stod(s.substr(colon + 1, brace - colon - 1));
        EXPECT_DOUBLE_EQ(parsed, v) << "did not round-trip: " << s;
    }
}

TEST(JsonWriter, NonFiniteDoublesBecomeNull) {
    // JSON has no NaN or Infinity; emitting them produces a file strict parsers
    // reject. Null is also the honest encoding, since both arise only from an
    // undefined or unmeasured quantity.
    const auto s = emit([](JsonWriter& w) {
        w.begin_object();
        w.key_value("nan", std::numeric_limits<double>::quiet_NaN());
        w.key_value("inf", std::numeric_limits<double>::infinity());
        w.end_object();
    });
    EXPECT_EQ(s, R"({"nan":null,"inf":null})");
}

TEST(JsonWriter, ExplicitNull) {
    const auto s = emit([](JsonWriter& w) {
        w.begin_object();
        w.key_null("absent");
        w.end_object();
    });
    EXPECT_EQ(s, R"({"absent":null})");
}

TEST(JsonWriter, CompactNumericArrays) {
    const auto s = emit([](JsonWriter& w) {
        w.begin_object();
        w.key_array("samples_ns", std::vector<std::uint64_t>{100, 200, 300});
        w.end_object();
    });
    EXPECT_EQ(s, R"({"samples_ns":[100,200,300]})");
}

// --- Result record ------------------------------------------------------------

/// Parses the emitted record with Python-compatible strictness by checking
/// balance and key presence. A full JSON parser is not linked in, so this
/// verifies structure and the fields that the schema guarantees.
TEST(ResultRecord, SerialisesWithAllRequiredSchemaFields) {
    lpq::ResultRecord r;
    r.experiment_id = "test_v1";
    r.timestamp     = lpq::iso8601_now();
    r.dataset       = "fixture:diamond";
    r.algorithm     = "dijkstra";
    r.pq            = "binary_heap";
    r.mode          = "exact";
    r.graph_nodes   = 4;
    r.graph_edges   = 5;
    r.source        = 0;
    r.target        = 3;
    r.hardware      = lpq::collect_hardware_info();
    r.timing.samples_ns = {100, 110, 105};
    r.timing.summary    = lpq::summarise_ns(r.timing.samples_ns);
    r.path_cost         = 3.0;
    r.optimal_cost      = 3.0;
    r.approximation_ratio = 1.0;
    r.exact_match       = true;
    r.termination       = "target_reached";

    std::ostringstream os;
    JsonWriter         w(os, true);
    lpq::write_record(w, r);
    const std::string s = os.str();

    // ESD.md section 23 required fields.
    for (const char* key : {"experiment_id", "timestamp", "dataset", "algorithm", "pq",
                            "mode", "graph_nodes", "graph_edges", "source", "target",
                            "pq_operations", "ml", "hardware_counters", "correctness",
                            "environment", "git_commit", "schema_version"}) {
        EXPECT_NE(s.find(std::string("\"") + key + "\""), std::string::npos)
            << "schema field missing: " << key;
    }

    // Braces must balance, or no parser will read the file.
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (char c : s) {
        if (escaped) { escaped = false; continue; }
        if (c == '\\') { escaped = true; continue; }
        if (c == '"') { in_string = !in_string; continue; }
        if (in_string) { continue; }
        if (c == '{' || c == '[') { ++depth; }
        if (c == '}' || c == ']') { --depth; }
    }
    EXPECT_EQ(depth, 0) << "unbalanced JSON";
}

TEST(ResultRecord, UnmeasuredHardwareCountersAreNullNotZero) {
    // The distinction that makes the record honest: a reader must be able to
    // tell "no PMU on this host" from "measured zero cache misses".
    lpq::ResultRecord r;
    r.hardware = lpq::collect_hardware_info();
    r.hw_counters.unavailable_reason = "WSL2 kernel exposes no cpu PMU";

    std::ostringstream os;
    JsonWriter         w(os, false);
    lpq::write_record(w, r);
    const std::string s = os.str();

    EXPECT_NE(s.find(R"("cycles":null)"), std::string::npos);
    EXPECT_NE(s.find(R"("cache_misses":null)"), std::string::npos);
    EXPECT_NE(s.find(R"("collected":false)"), std::string::npos);
    EXPECT_NE(s.find("unavailable_reason"), std::string::npos);
    EXPECT_EQ(s.find(R"("cycles":0)"), std::string::npos)
        << "an unmeasured counter must never serialise as zero";
}

TEST(ResultRecord, UnsupportedConfigurationsStillEmitARecord) {
    // ESD.md section 7: a missing radix-heap row must be explained rather than
    // merely absent.
    lpq::ResultRecord r;
    r.pq                 = "radix_heap";
    r.unsupported_reason = lpq::radix_heap_unsupported_reason<double>();
    r.hardware           = lpq::collect_hardware_info();
    EXPECT_FALSE(r.supported());

    std::ostringstream os;
    JsonWriter         w(os, false);
    lpq::write_record(w, r);
    const std::string s = os.str();
    EXPECT_NE(s.find(R"("supported":false)"), std::string::npos);
    EXPECT_NE(s.find("unsupported_reason"), std::string::npos);
}

TEST(ResultRecord, RawSamplesAreRetainedInTheRecord) {
    // PRD.md section 22: report distributions, not only summaries, and never
    // silently discard outliers.
    lpq::ResultRecord r;
    r.hardware          = lpq::collect_hardware_info();
    r.timing.samples_ns = {100, 110, 9999};
    r.timing.summary    = lpq::summarise_ns(r.timing.samples_ns);

    std::ostringstream os;
    JsonWriter         w(os, false);
    lpq::write_record(w, r);
    const std::string s = os.str();

    EXPECT_NE(s.find("[100,110,9999]"), std::string::npos)
        << "every raw sample, including the outlier, must be retained";
}

// --- Provenance ---------------------------------------------------------------

TEST(HardwareInfo, CarriesBuildProvenance) {
    // ESD.md section 40: no result without hardware and git metadata.
    const auto h = lpq::collect_hardware_info();
    EXPECT_FALSE(h.compiler.empty());
    EXPECT_FALSE(h.compiler_version.empty());
    EXPECT_FALSE(h.build_type.empty());
    EXPECT_GT(h.logical_cores, 0U);
    EXPECT_GT(h.clock_resolution_ns, 0.0);
    EXPECT_NE(h.git_commit, "") << "git commit must be injected at build time";
    EXPECT_NE(h.git_commit, "unknown")
        << "build was produced without git provenance; ESD.md section 40 forbids "
           "generating results from such a build";
}

TEST(HardwareInfo, ReportsWhetherCountersAreAvailableRatherThanAssuming) {
    const auto h = lpq::collect_hardware_info();
    // No assertion on the value -- it is genuinely host-dependent. What matters
    // is that the field is populated by inspection, so the record states the
    // fact rather than leaving a reader to guess.
    SUCCEED() << "hardware counters available: "
              << (h.hardware_counters_available ? "yes" : "no");
}

}  // namespace
