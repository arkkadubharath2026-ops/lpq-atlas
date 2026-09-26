// =============================================================================
// Execution trace recorder (LPQ Atlas spec sections 8, 9 and 42).
//
// Turns the operations a real search performs into a JSONL event stream that
// the visualiser replays. Two seams feed it, and neither lets it change what
// the search computes:
//
//   * search_observer.hpp -- Dijkstra and A* report pops, settles, relaxations
//     and examined arcs as they happen.
//   * extraction_observer.hpp -- the learned queue reports how each extraction
//     was resolved. Before each extraction the recorder also reads the
//     substrate's current bucket and front through a snapshot callback, which
//     only reads state the queue's own pop() is about to read.
//
// Attaching an ExtractionObserver changes one thing in LearnedPriorityQueue:
// the window is capped by policy().window_size. The recorder answers with an
// uncapped window unless it is wrapping the adaptive controller, in which case
// it forwards the controller's policy verbatim -- so a traced run takes the same
// path as the untraced run it describes. lpq_run checks that claim on every
// traced run by comparing the two results, and test_trace.cpp asserts it.
//
// Nothing here is timed. Traced runs are reported with their own, explicitly
// instrumented wall clock and never as the benchmark runtime (spec 3.4).
// =============================================================================
#ifndef LPQ_TRACE_TRACE_RECORDER_HPP
#define LPQ_TRACE_TRACE_RECORDER_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "lpq/algorithms/search_observer.hpp"
#include "lpq/common/error.hpp"
#include "lpq/common/types.hpp"
#include "lpq/lpq/extraction_observer.hpp"
#include "lpq/verification/correction_engine.hpp"

namespace lpq {

/// Version of the event schema. Bumped whenever a field changes meaning.
inline constexpr int kTraceSchemaVersion = 1;

/// How much of an execution is recorded (spec section 9).
enum class TraceLevel : std::uint8_t {
    kNone,     ///< no trace at all: the benchmark configuration
    kSummary,  ///< lifecycle and final path only
    kVisual,   ///< settles, relaxations and every learned-queue decision
    kFull,     ///< additionally every pop, stale pop, push and examined arc
};

[[nodiscard]] inline TraceLevel parse_trace_level(std::string_view s) {
    if (s == "none") return TraceLevel::kNone;
    if (s == "summary") return TraceLevel::kSummary;
    if (s == "visual") return TraceLevel::kVisual;
    if (s == "full") return TraceLevel::kFull;
    throw InvalidConfigError("unknown trace level: " + std::string(s));
}

[[nodiscard]] inline constexpr const char* trace_level_name(TraceLevel l) noexcept {
    switch (l) {
        case TraceLevel::kNone: return "none";
        case TraceLevel::kSummary: return "summary";
        case TraceLevel::kVisual: return "visual";
        case TraceLevel::kFull: return "full";
    }
    return "unknown";
}

/// The learned queue's state immediately before an extraction.
struct LpqSnapshot {
    struct FrontEntry {
        NodeId node  = 0;
        double key   = 0.0;
        float  score = 0.0F;
    };
    double                  bucket_base  = 0.0;
    double                  bucket_width = 0.0;
    std::size_t             bucket_size  = 0;
    double                  bound        = 0.0;
    std::size_t             queue_size   = 0;
    std::vector<FrontEntry> front;
};

/// One JSONL line under construction. Numbers are written with enough digits
/// to round-trip, so a replayed distance is the engine's distance.
class TraceLine {
public:
    TraceLine(std::uint64_t seq, std::string_view type) {
        buf_.reserve(128);
        buf_ += "{\"schema_version\":";
        buf_ += std::to_string(kTraceSchemaVersion);
        buf_ += ",\"seq\":";
        buf_ += std::to_string(seq);
        buf_ += ",\"type\":\"";
        buf_ += type;
        buf_ += '"';
    }
    TraceLine& u(std::string_view k, std::uint64_t v) {
        key(k);
        buf_ += std::to_string(v);
        return *this;
    }
    TraceLine& d(std::string_view k, double v) {
        key(k);
        number(v);
        return *this;
    }
    /// A distance, where a negative value means "unreached" and is written null.
    TraceLine& dist(std::string_view k, double v) {
        key(k);
        if (v < 0.0) {
            buf_ += "null";
        } else {
            number(v);
        }
        return *this;
    }
    TraceLine& b(std::string_view k, bool v) {
        key(k);
        buf_ += v ? "true" : "false";
        return *this;
    }
    TraceLine& s(std::string_view k, std::string_view v) {
        key(k);
        buf_ += '"';
        for (const char c : v) {
            if (c == '"' || c == '\\') {
                buf_ += '\\';
            }
            buf_ += c;
        }
        buf_ += '"';
        return *this;
    }
    TraceLine& nodes(std::string_view k, const std::vector<NodeId>& v) {
        key(k);
        buf_ += '[';
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i != 0) buf_ += ',';
            buf_ += std::to_string(v[i]);
        }
        buf_ += ']';
        return *this;
    }
    TraceLine& front(std::string_view k, const std::vector<LpqSnapshot::FrontEntry>& v) {
        key(k);
        buf_ += '[';
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i != 0) buf_ += ',';
            buf_ += "{\"node\":";
            buf_ += std::to_string(v[i].node);
            buf_ += ",\"key\":";
            number(v[i].key);
            buf_ += ",\"score\":";
            number(static_cast<double>(v[i].score));
            buf_ += '}';
        }
        buf_ += ']';
        return *this;
    }
    [[nodiscard]] std::string finish() {
        buf_ += "}\n";
        return std::move(buf_);
    }

private:
    void key(std::string_view k) {
        buf_ += ",\"";
        buf_ += k;
        buf_ += "\":";
    }
    void number(double v) {
        if (v != v || v == std::numeric_limits<double>::infinity() ||
            v == -std::numeric_limits<double>::infinity()) {
            buf_ += "null";  // JSON has no NaN/inf; never emit invalid JSON
            return;
        }
        char tmp[32];
        const int n = std::snprintf(tmp, sizeof tmp, "%.17g", v);
        buf_.append(tmp, static_cast<std::size_t>(n > 0 ? n : 0));
    }
    std::string buf_;
};

/// Records a search as JSONL. Implements the search-observer hooks and, for the
/// learned queue, ExtractionObserver.
class TraceRecorder final : public ExtractionObserver {
public:
    /// Events past `max_events` are dropped and counted, except the lifecycle
    /// events that close the trace, which are always written.
    TraceRecorder(std::ostream& out, TraceLevel level, std::uint64_t max_events, bool astar)
        : out_(out), level_(level), max_events_(max_events), astar_(astar) {}

    // --- configuration --------------------------------------------------------

    /// Connects the learned queue. `snapshot` reads the queue's current bucket;
    /// `inner` is the adaptive controller's observer, or null.
    void attach_lpq(std::function<void(LpqSnapshot&, std::size_t)> snapshot,
                    ExtractionObserver* inner) {
        snapshot_ = std::move(snapshot);
        inner_    = inner;
        lpq_      = true;
    }

    // --- lifecycle ------------------------------------------------------------

    void run_started(std::string_view algorithm, std::string_view queue, NodeId source,
                     NodeId target) {
        TraceLine l(next_seq(), "RUN_STARTED");
        l.s("algorithm", algorithm).s("queue", queue).u("source", source);
        if (target != kInvalidNode) {
            l.u("target", target);
        }
        l.s("trace_level", trace_level_name(level_)).u("max_events", max_events_);
        write_always(l);
    }

    void path_reconstructed(const std::vector<NodeId>& path, double distance) {
        TraceLine l(next_seq(), "PATH_RECONSTRUCTED");
        l.nodes("path", path).dist("distance", distance);
        write_always(l);
    }

    void run_completed(std::string_view termination, bool reachable, double distance) {
        TraceLine l(next_seq(), "RUN_COMPLETED");
        l.s("termination", termination).b("reachable", reachable);
        l.dist("distance", reachable ? distance : -1.0);
        l.u("events_written", written_ + 1).u("events_dropped", dropped_);
        l.b("truncated", dropped_ > 0);
        write_always(l);
        out_.flush();
    }

    // --- search observer hooks ------------------------------------------------

    void on_source_pushed(NodeId source, double key) {
        {
            TraceLine l(next_seq(), "SOURCE_INITIALIZED");
            l.u("node", source).d("distance", 0.0);
            if (astar_) {
                l.d("h", key).d("f", key);
            }
            write_capped(l, level_ >= TraceLevel::kVisual);
        }
        if (level_ == TraceLevel::kFull) {
            TraceLine l(next_seq(), "QUEUE_PUSH");
            l.u("node", source).d("key", key);
            write_capped(l, true);
        }
    }

    void on_pop(NodeId node, double key, std::size_t queue_size_after) {
        ++pops_;
        if (lpq_ && pending_extraction_) {
            // policy() ran but no outcome was reported: the queue took the
            // classical scan because the front was empty.
            TraceLine l(next_seq(), "LPQ_FALLBACK_STARTED");
            l.s("reason", "empty_front");
            write_capped(l, level_ >= TraceLevel::kVisual);
        }
        pending_extraction_ = false;
        if (level_ == TraceLevel::kFull && room(1)) {
            TraceLine l(next_seq(), "QUEUE_POP");
            l.u("node", node).d("key", key).u("queue_size", queue_size_after);
            write_capped(l, true);
        }
    }

    void on_stale_pop(NodeId node, double key) {
        ++stale_;
        if (level_ == TraceLevel::kFull && room(1)) {
            TraceLine l(next_seq(), "STALE_POP");
            l.u("node", node).d("key", key);
            write_capped(l, true);
        }
    }

    void on_settle(NodeId node, double distance) {
        ++settled_;
        if (level_ >= TraceLevel::kVisual && room(1)) {
            TraceLine l(next_seq(), "NODE_SETTLED");
            l.u("node", node).d("distance", distance);
            write_capped(l, true);
        }
    }

    void on_edge_examined(NodeId from, NodeId to, double weight, double candidate) {
        if (level_ == TraceLevel::kFull && room(1)) {
            TraceLine l(next_seq(), "EDGE_EXAMINED");
            l.u("from", from).u("to", to).d("weight", weight).d("candidate", candidate);
            write_capped(l, true);
        }
    }

    void on_edge_relaxed(NodeId from, NodeId to, double old_distance, double new_distance,
                         double key, QueueUpdate how) {
        ++relaxed_;
        if (level_ >= TraceLevel::kVisual && !room(level_ == TraceLevel::kFull ? 2 : 1)) {
            return;
        }
        if (level_ >= TraceLevel::kVisual) {
            TraceLine l(next_seq(), "EDGE_RELAXED");
            l.u("from", from).u("to", to).dist("old_distance", old_distance);
            l.d("new_distance", new_distance).d("key", key);
            if (astar_) {
                // key is f = g + h; the heuristic term is recovered exactly
                // because both were computed in integer weight units.
                l.d("h", key - new_distance);
            }
            write_capped(l, true);
        }
        if (level_ == TraceLevel::kFull) {
            TraceLine l(next_seq(), how == QueueUpdate::kPush ? "QUEUE_PUSH" : "QUEUE_DECREASE_KEY");
            l.u("node", to).d("key", key);
            write_capped(l, true);
        }
    }

    void on_target_reached(NodeId node, double distance) {
        TraceLine l(next_seq(), "DESTINATION_REACHED");
        l.u("node", node).d("distance", distance);
        write_always(l);
    }

    // --- ExtractionObserver ---------------------------------------------------

    [[nodiscard]] ExtractionPolicy policy() const noexcept override {
        ExtractionPolicy p;
        if (inner_ != nullptr) {
            p = inner_->policy();
        } else {
            // No controller: never narrow the window below what the queue's own
            // candidate manager chose, so the run is the untraced run.
            p.use_learned_path = true;
            p.window_size      = std::numeric_limits<std::uint32_t>::max();
        }
        auto* self = const_cast<TraceRecorder*>(this);
        try {
            self->before_extraction(p);
        } catch (...) {
            // Recording must never alter the search. A failure to record (out
            // of memory for the line buffer) loses the event, not the run.
            ++self->dropped_;
        }
        return p;
    }

    void on_extraction(std::uint64_t entries_skipped, std::uint32_t verification_comparisons,
                       const CorrectionOutcome& outcome) noexcept override {
        if (inner_ != nullptr) {
            inner_->on_extraction(entries_skipped, verification_comparisons, outcome);
        }
        pending_extraction_ = false;
        try {
            record_outcome(entries_skipped, outcome);
        } catch (...) {
            ++dropped_;
        }
    }

    // --- summary --------------------------------------------------------------

    [[nodiscard]] std::uint64_t events_written() const noexcept { return written_; }
    [[nodiscard]] std::uint64_t events_dropped() const noexcept { return dropped_; }
    [[nodiscard]] std::uint64_t settled_seen() const noexcept { return settled_; }
    [[nodiscard]] std::uint64_t relaxed_seen() const noexcept { return relaxed_; }
    [[nodiscard]] std::uint64_t pops_seen() const noexcept { return pops_; }
    [[nodiscard]] std::uint64_t stale_seen() const noexcept { return stale_; }
    [[nodiscard]] const std::map<std::string, std::uint64_t>& lpq_paths() const noexcept {
        return lpq_paths_;
    }

private:
    [[nodiscard]] std::uint64_t next_seq() noexcept { return ++seq_; }

    /// False once the cap is reached. Hooks test it BEFORE formatting, so a
    /// capped trace costs a counter increment per event rather than a string.
    /// `n` is how many events the caller would have written; they are counted
    /// as dropped, so events_dropped is exact rather than estimated.
    [[nodiscard]] bool room(std::uint64_t n) noexcept {
        if (written_ < max_events_) {
            return true;
        }
        dropped_ += n;
        return false;
    }

    void write_always(TraceLine& l) {
        out_ << l.finish();
        ++written_;
    }

    void write_capped(TraceLine& l, bool wanted) {
        if (!wanted) {
            --seq_;  // an unwritten event must not leave a gap in the sequence
            return;
        }
        if (written_ >= max_events_) {
            --seq_;
            ++dropped_;
            return;
        }
        write_always(l);
    }

    void before_extraction(const ExtractionPolicy& p) {
        pending_extraction_ = true;
        last_policy_        = p;
        if (inner_ != nullptr &&
            (!have_policy_ || p.use_learned_path != reported_policy_.use_learned_path ||
             p.window_size != reported_policy_.window_size)) {
            TraceLine l(next_seq(), "LPQ_ADAPTIVE_POLICY");
            l.b("use_learned_path", p.use_learned_path).u("window_size", p.window_size);
            write_capped(l, level_ >= TraceLevel::kVisual);
            reported_policy_ = p;
            have_policy_     = true;
        }
        if (level_ < TraceLevel::kVisual || !snapshot_) {
            return;
        }
        if (!room(p.use_learned_path ? 2 : 1)) {
            return;
        }
        const std::size_t cap = level_ == TraceLevel::kFull ? 32 : 8;
        snap_.front.clear();
        snapshot_(snap_, cap);
        {
            TraceLine l(next_seq(), "LPQ_BUCKET_SELECTED");
            l.d("bucket_base", snap_.bucket_base).d("bucket_width", snap_.bucket_width);
            l.u("bucket_size", snap_.bucket_size).d("bound", snap_.bound);
            l.u("queue_size", snap_.queue_size);
            write_capped(l, true);
        }
        if (p.use_learned_path) {
            // The front is what the candidate window is built from, and each
            // entry's score is the score the queue stored at push time -- the
            // learned score actually used for this extraction.
            TraceLine l(next_seq(), "LPQ_CANDIDATE_WINDOW");
            l.front("front", snap_.front).u("front_shown", snap_.front.size());
            write_capped(l, true);
        }
    }

    void record_outcome(std::uint64_t skipped, const CorrectionOutcome& o) {
        const auto path = resolution_path_name(o.path);
        ++lpq_paths_[std::string(path)];
        if (level_ < TraceLevel::kVisual) {
            return;
        }
        if (written_ >= max_events_) {
            // Exact count of what the switch below would have written.
            switch (o.path) {
                case ResolutionPath::kFirstAttempt: dropped_ += 2; break;
                case ResolutionPath::kFirstAttemptCorrected: dropped_ += 3; break;
                case ResolutionPath::kExpanded: dropped_ += 4; break;
                case ResolutionPath::kFallback: dropped_ += 2; break;
                case ResolutionPath::kEmpty: dropped_ += 1; break;
            }
            return;
        }
        const VerificationResult& v = o.verification;
        switch (o.path) {
            case ResolutionPath::kFirstAttempt:
            case ResolutionPath::kFirstAttemptCorrected:
            case ResolutionPath::kExpanded: {
                {
                    TraceLine l(next_seq(), "LPQ_CANDIDATE_SELECTED");
                    l.b("prediction_correct", v.prediction_was_correct);
                    l.u("rank_displacement", v.rank_displacement);
                    l.d("predicted_key", v.key + v.key_displacement);
                    if (v.prediction_was_correct) {
                        l.u("node", v.node);
                    }
                    write_capped(l, true);
                }
                if (o.path == ResolutionPath::kExpanded) {
                    TraceLine r(next_seq(), "LPQ_VERIFICATION_REJECTED");
                    r.s("reason", "window_too_narrow").u("attempts_before_accept", o.attempts - 1);
                    write_capped(r, true);
                }
                if (o.path != ResolutionPath::kFirstAttempt) {
                    TraceLine c(next_seq(), "LPQ_CORRECTION_APPLIED");
                    c.s("kind", o.path == ResolutionPath::kExpanded ? "window_expanded"
                                                                    : "reordered_within_window");
                    c.u("expansions", o.expansions).u("rank_displacement", v.rank_displacement);
                    c.d("key_displacement", v.key_displacement);
                    write_capped(c, true);
                }
                TraceLine a(next_seq(), "LPQ_VERIFICATION_ACCEPTED");
                a.u("node", v.node).d("key", v.key).u("comparisons", o.comparisons);
                a.u("entries_skipped", skipped).s("resolution", resolution_path_name(o.path));
                write_capped(a, true);
                break;
            }
            case ResolutionPath::kFallback: {
                TraceLine r(next_seq(), "LPQ_VERIFICATION_REJECTED");
                r.s("reason", verification_outcome_name(v.outcome)).u("attempts", o.attempts);
                r.u("comparisons", o.comparisons);
                write_capped(r, true);
                TraceLine f(next_seq(), "LPQ_FALLBACK_STARTED");
                f.s("reason", "verification_failed");
                write_capped(f, true);
                break;
            }
            case ResolutionPath::kEmpty: {
                TraceLine f(next_seq(), "LPQ_FALLBACK_STARTED");
                f.s("reason", last_policy_.use_learned_path ? "empty_window"
                                                            : "adaptive_controller_retreat");
                write_capped(f, true);
                break;
            }
        }
    }

    std::ostream&  out_;
    TraceLevel     level_;
    std::uint64_t  max_events_;
    bool           astar_;
    std::uint64_t  seq_     = 0;
    std::uint64_t  written_ = 0;
    std::uint64_t  dropped_ = 0;
    std::uint64_t  settled_ = 0;
    std::uint64_t  relaxed_ = 0;
    std::uint64_t  pops_    = 0;
    std::uint64_t  stale_   = 0;

    bool                                            lpq_ = false;
    std::function<void(LpqSnapshot&, std::size_t)> snapshot_;
    ExtractionObserver*                             inner_              = nullptr;
    bool                                            pending_extraction_ = false;
    ExtractionPolicy                                last_policy_{};
    ExtractionPolicy                                reported_policy_{};
    bool                                            have_policy_ = false;
    LpqSnapshot                                     snap_;
    std::map<std::string, std::uint64_t>            lpq_paths_;
};

}  // namespace lpq

#endif  // LPQ_TRACE_TRACE_RECORDER_HPP
