// =============================================================================
// Reading the provenance fields of a model's .report.json (M5-O4).
//
// The report is written by python/lpq_tools/training/train.py and holds what the model was trained
// on. The engine reads only the few fields it needs to refuse an unsuitable model: the graph the
// labels came from, the bucket width they were defined for, and the training manifest's name and
// hash. It is not a general JSON library: it parses the JSON grammar well enough to find named
// scalars at known paths, and it rejects anything malformed rather than guessing.
// =============================================================================
#ifndef LPQ_MODELS_MODEL_REPORT_HPP
#define LPQ_MODELS_MODEL_REPORT_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lpq/common/error.hpp"

namespace lpq {

class ModelReportError : public LpqError {
public:
    explicit ModelReportError(const std::string& what) : LpqError(what) {}
};

struct ModelReport {
    std::string                  dataset_id;
    std::optional<std::uint64_t> graph_checksum;
    std::optional<std::uint64_t> bucket_width;
    std::optional<std::uint64_t> query_seed;
    std::optional<std::uint64_t> query_count;
    std::optional<std::uint64_t> training_seed;

    /// From the `training_manifest` record (D-21); empty for reports written before it existed.
    std::string manifest_name;
    std::string manifest_sha256;
    std::string manifest_selection;
};

namespace detail {

class ReportParser {
public:
    explicit ReportParser(std::string_view text) : text_(text) {}

    ModelReport parse() {
        skip_ws();
        parse_value(0);
        skip_ws();
        if (pos_ != text_.size()) {
            fail("trailing characters after the JSON document");
        }
        return report_;
    }

private:
    [[noreturn]] void fail(const std::string& what) const {
        throw ModelReportError("model report is not valid JSON (" + what + " at offset " +
                               std::to_string(pos_) + ")");
    }

    void skip_ws() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' ||
                                       text_[pos_] == '\t' || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    [[nodiscard]] char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    void expect(char c) {
        if (peek() != c) {
            fail(std::string("expected '") + c + "'");
        }
        ++pos_;
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (true) {
            if (pos_ >= text_.size()) {
                fail("unterminated string");
            }
            const char c = text_[pos_++];
            if (c == '"') {
                return out;
            }
            if (c == '\\') {
                if (pos_ >= text_.size()) {
                    fail("unterminated escape");
                }
                const char e = text_[pos_++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u':
                        if (pos_ + 4 > text_.size()) {
                            fail("short \\u escape");
                        }
                        pos_ += 4;
                        out += '?';
                        break;
                    default: fail("bad escape");
                }
            } else {
                out += c;
            }
        }
    }

    std::string parse_scalar_token() {
        const std::size_t start = pos_;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\n' || c == '\t' ||
                c == '\r') {
                break;
            }
            ++pos_;
        }
        if (pos_ == start) {
            fail("expected a value");
        }
        return std::string(text_.substr(start, pos_ - start));
    }

    void parse_value(int depth) {
        if (depth > 16) {
            fail("nesting is too deep");
        }
        skip_ws();
        const char c = peek();
        if (c == '{') {
            parse_object(depth);
        } else if (c == '[') {
            parse_array(depth);
        } else if (c == '"') {
            capture(parse_string(), true);
        } else {
            capture(parse_scalar_token(), false);
        }
    }

    void parse_object(int depth) {
        expect('{');
        skip_ws();
        if (peek() == '}') {
            ++pos_;
            return;
        }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            expect(':');
            path_.push_back(std::move(key));
            parse_value(depth + 1);
            path_.pop_back();
            skip_ws();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            expect('}');
            return;
        }
    }

    void parse_array(int depth) {
        expect('[');
        skip_ws();
        if (peek() == ']') {
            ++pos_;
            return;
        }
        path_.emplace_back("[]");
        while (true) {
            parse_value(depth + 1);
            skip_ws();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            expect(']');
            break;
        }
        path_.pop_back();
    }

    static std::optional<std::uint64_t> as_u64(const std::string& token, bool is_string) {
        if (is_string || token.empty() || token.size() > 20 ||
            token.find_first_not_of("0123456789") != std::string::npos) {
            return std::nullopt;
        }
        std::uint64_t v = 0;
        for (const char d : token) {
            const std::uint64_t digit = static_cast<std::uint64_t>(d - '0');
            if (v > (UINT64_MAX - digit) / 10U) {
                return std::nullopt;  // does not fit in 64 bits
            }
            v = v * 10U + digit;
        }
        return v;
    }

    void capture(const std::string& token, bool is_string) {
        if (path_.size() == 1) {
            const std::string& k = path_[0];
            if (k == "dataset_id" && is_string) report_.dataset_id = token;
            else if (k == "graph_checksum") report_.graph_checksum = as_u64(token, is_string);
            else if (k == "bucket_width") report_.bucket_width = as_u64(token, is_string);
            else if (k == "query_seed") report_.query_seed = as_u64(token, is_string);
            else if (k == "query_count") report_.query_count = as_u64(token, is_string);
            else if (k == "seed") report_.training_seed = as_u64(token, is_string);
        } else if (path_.size() == 2 && path_[0] == "training_manifest" && is_string) {
            if (path_[1] == "name") report_.manifest_name = token;
            else if (path_[1] == "sha256") report_.manifest_sha256 = token;
            else if (path_[1] == "selection") report_.manifest_selection = token;
        }
    }

    std::string_view         text_;
    std::size_t              pos_ = 0;
    std::vector<std::string> path_;
    ModelReport              report_;
};

}  // namespace detail

/// Throws ModelReportError if the text is not a valid JSON document.
[[nodiscard]] inline ModelReport parse_model_report(std::string_view json) {
    return detail::ReportParser(json).parse();
}

}  // namespace lpq

#endif  // LPQ_MODELS_MODEL_REPORT_HPP
