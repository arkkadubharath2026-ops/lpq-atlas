// =============================================================================
// Minimal JSON writer for machine-readable results (ESD.md sections 21, 23).
//
// Hand-written rather than pulled in as a dependency. The output shape is
// fixed and small -- nested objects, arrays, strings, numbers and booleans --
// and a serialiser is the one place where an external library's version drift
// could silently change the schema of previously written results. ESD.md
// section 31 requires raw results to remain immutable and comparable over the
// life of the project; owning ~150 lines of serialisation is a cheaper way to
// guarantee that than pinning a dependency.
//
// Two correctness requirements this must not get wrong:
//
//   * Strings are escaped. Dataset identifiers and error strings reach this
//     writer from file paths and exception messages, both of which can contain
//     quotes and backslashes -- Windows paths contain backslashes by default.
//     Unescaped, they would produce a file that no analysis script can parse.
//
//   * Doubles round-trip. Timings and approximation ratios are analysed in
//     Python afterwards, so a value must read back as the value written.
//     std::to_string gives 6 decimal places and would quietly truncate a
//     nanosecond timing; 17 significant digits is the shortest precision that
//     round-trips every IEEE-754 double.
// =============================================================================
#ifndef LPQ_METRICS_JSON_WRITER_HPP
#define LPQ_METRICS_JSON_WRITER_HPP

#include <cmath>
#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace lpq {

/// Streaming JSON writer with automatic comma and indentation handling.
class JsonWriter {
public:
    explicit JsonWriter(std::ostream& out, bool pretty = true) : out_(out), pretty_(pretty) {}

    // --- Objects --------------------------------------------------------------

    void begin_object() {
        separate();
        out_ << '{';
        push(true);
    }

    void begin_object(std::string_view key) {
        write_key(key);
        out_ << '{';
        push(true);
    }

    void end_object() {
        pop();
        newline_indent();
        out_ << '}';
    }

    // --- Arrays ---------------------------------------------------------------

    void begin_array(std::string_view key) {
        write_key(key);
        out_ << '[';
        push(false);
    }

    void end_array() {
        pop();
        newline_indent();
        out_ << ']';
    }

    // --- Values ---------------------------------------------------------------

    void key_value(std::string_view key, std::string_view value) {
        write_key(key);
        write_escaped(value);
    }

    void key_value(std::string_view key, const char* value) {
        key_value(key, std::string_view(value));
    }

    void key_value(std::string_view key, bool value) {
        write_key(key);
        out_ << (value ? "true" : "false");
    }

    void key_value(std::string_view key, std::uint64_t value) {
        write_key(key);
        out_ << value;
    }

    void key_value(std::string_view key, std::int64_t value) {
        write_key(key);
        out_ << value;
    }

    // No std::size_t overload: on the 64-bit targets this project builds for,
    // size_t and uint64_t are the same type, and declaring both is a redefinition
    // rather than an overload. size_t arguments widen to uint64_t implicitly,
    // which is lossless on every platform where they differ.

    void key_value(std::string_view key, int value) {
        key_value(key, static_cast<std::int64_t>(value));
    }

    void key_value(std::string_view key, double value) {
        write_key(key);
        write_double(value);
    }

    /// Writes a null. Used for genuinely absent measurements -- notably the
    /// hardware counters, which must be absent rather than zero on a host with
    /// no PMU, since a reader cannot distinguish "zero cache misses" from
    /// "cache misses were never measured".
    void key_null(std::string_view key) {
        write_key(key);
        out_ << "null";
    }

    void value(double v) {
        separate();
        write_double(v);
    }

    void value(std::uint64_t v) {
        separate();
        out_ << v;
    }

    void value(std::string_view v) {
        separate();
        write_escaped(v);
    }

    /// Writes an array of numbers compactly on one line.
    ///
    /// Used for raw per-repetition samples, which must be retained (PRD.md
    /// section 22) but would otherwise dominate the file's line count.
    void key_array(std::string_view key, const std::vector<std::uint64_t>& values) {
        write_key(key);
        out_ << '[';
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) {
                out_ << ',';
            }
            out_ << values[i];
        }
        out_ << ']';
    }

    void key_array(std::string_view key, const std::vector<double>& values) {
        write_key(key);
        out_ << '[';
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) {
                out_ << ',';
            }
            write_double(values[i]);
        }
        out_ << ']';
    }

private:
    struct Level {
        bool is_object;
        bool has_entries;
    };

    void push(bool is_object) { stack_.push_back({is_object, false}); }

    void pop() {
        if (!stack_.empty()) {
            stack_.pop_back();
        }
    }

    /// Emits a comma before any entry that is not the first at its level.
    void separate() {
        if (stack_.empty()) {
            return;
        }
        if (stack_.back().has_entries) {
            out_ << ',';
        }
        stack_.back().has_entries = true;
        newline_indent();
    }

    void write_key(std::string_view key) {
        separate();
        write_escaped(key);
        out_ << ':';
        if (pretty_) {
            out_ << ' ';
        }
    }

    void newline_indent() {
        if (!pretty_) {
            return;
        }
        out_ << '\n';
        for (std::size_t i = 0; i < stack_.size(); ++i) {
            out_ << "  ";
        }
    }

    void write_escaped(std::string_view s) {
        out_ << '"';
        for (const char c : s) {
            switch (c) {
                case '"':  out_ << "\\\""; break;
                case '\\': out_ << "\\\\"; break;
                case '\n': out_ << "\\n";  break;
                case '\r': out_ << "\\r";  break;
                case '\t': out_ << "\\t";  break;
                case '\b': out_ << "\\b";  break;
                case '\f': out_ << "\\f";  break;
                default:
                    // Control characters must be escaped as \u00XX; everything
                    // else, including UTF-8 continuation bytes, passes through.
                    if (static_cast<unsigned char>(c) < 0x20U) {
                        static const char* kHex = "0123456789abcdef";
                        out_ << "\\u00" << kHex[(static_cast<unsigned char>(c) >> 4) & 0xF]
                             << kHex[static_cast<unsigned char>(c) & 0xF];
                    } else {
                        out_ << c;
                    }
            }
        }
        out_ << '"';
    }

    void write_double(double v) {
        // JSON has no representation for NaN or infinity. Emitting them anyway
        // produces a file that strict parsers reject, so they become null --
        // which is also the honest encoding, since both arise here only from an
        // undefined or unmeasured quantity.
        if (std::isnan(v) || std::isinf(v)) {
            out_ << "null";
            return;
        }
        std::ostringstream os;
        os.precision(17);  // shortest precision that round-trips a double
        os << v;
        out_ << os.str();
    }

    std::ostream&      out_;
    bool               pretty_;
    std::vector<Level> stack_;
};

}  // namespace lpq

#endif  // LPQ_METRICS_JSON_WRITER_HPP
