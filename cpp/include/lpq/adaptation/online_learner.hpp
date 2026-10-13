// =============================================================================
// Online learner (ESD.md section 14, PRD.md section 11).
//
// Consumes PredictionEvents and applies bounded updates to a model.
//
// ESD.md section 14 requires updates that are "bounded, deterministic where
// possible, optionally batched, configurable by interval", and support for cold
// start, warm start, periodic update, adaptive update and disabled. PRD.md
// section 11 adds the constraint that matters most: "Training must never cause
// unbounded pauses in the shortest-path critical path."
//
// -----------------------------------------------------------------------------
// How the pause is actually bounded
// -----------------------------------------------------------------------------
//
// Three separate limits, because each covers a hole the others leave:
//
//   * The ring buffer has a fixed capacity, so recording an event is O(1) and
//     never allocates, however long the search runs.
//
//   * An update applies at most `max_batch` events, so a single update's cost
//     is bounded even if a huge backlog has accumulated.
//
//   * Updates fire on an interval, not per event, so the per-extraction cost is
//     amortised rather than paid every time.
//
// Without the second, a long quiet period followed by one update would produce
// exactly the unbounded pause PRD.md section 11 forbids -- and it would land in
// the middle of a timed query, where it would look like a tail-latency problem
// with no obvious cause.
//
// -----------------------------------------------------------------------------
// Dropping the oldest events is the right policy
// -----------------------------------------------------------------------------
//
// When the buffer is full the oldest event is overwritten. That is not merely
// the easy choice. PRD.md section 15 makes behaviour under distribution shift a
// primary research goal, and under shift the oldest events describe a regime
// that no longer exists; training on them would actively slow adaptation.
// Dropping them is recorded in the metrics, so the rate is visible rather than
// silent.
//
// -----------------------------------------------------------------------------
// Determinism
// -----------------------------------------------------------------------------
//
// Events are applied strictly in arrival order, and nothing here samples,
// shuffles or reorders. SGD is order-dependent, so any of those would make two
// runs from the same seed produce different models and therefore different
// search paths and different timings -- a reproducibility failure that would
// still return correct answers, and so would be very hard to notice.
// =============================================================================
#ifndef LPQ_ADAPTATION_ONLINE_LEARNER_HPP
#define LPQ_ADAPTATION_ONLINE_LEARNER_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

#include "lpq/adaptation/ewma.hpp"
#include "lpq/adaptation/prediction_event.hpp"
#include "lpq/common/error.hpp"
#include "lpq/models/predictor.hpp"

namespace lpq {

/// When updates are applied (ESD.md section 14).
enum class UpdateMode : std::uint8_t {
    /// No updates at all. The frozen-model control for the ablation: it isolates
    /// the cost of recording events from the effect of learning from them.
    kDisabled = 0,

    /// Apply each event as it arrives. Highest adaptation rate, highest
    /// per-extraction cost, and the configuration most likely to violate the
    /// bounded-pause requirement on a large graph.
    kEveryEvent,

    /// Apply a bounded batch every `interval` events.
    kPeriodic,

    /// Apply a bounded batch when recent prediction error rises above a
    /// threshold, and otherwise leave the model alone.
    kAdaptive,
};

[[nodiscard]] inline constexpr std::string_view update_mode_name(UpdateMode m) noexcept {
    switch (m) {
        case UpdateMode::kDisabled:   return "disabled";
        case UpdateMode::kEveryEvent: return "every_event";
        case UpdateMode::kPeriodic:   return "periodic";
        case UpdateMode::kAdaptive:   return "adaptive";
    }
    return "unknown";
}

/// How a model enters a run (PRD.md section 11).
enum class StartPolicy : std::uint8_t {
    /// Begin from an untrained model and learn during the run.
    kCold = 0,
    /// Begin from parameters fitted offline.
    kWarm,
};

[[nodiscard]] inline constexpr std::string_view start_policy_name(StartPolicy p) noexcept {
    switch (p) {
        case StartPolicy::kCold: return "cold";
        case StartPolicy::kWarm: return "warm";
    }
    return "unknown";
}

struct OnlineLearningConfig {
    UpdateMode  mode  = UpdateMode::kPeriodic;
    StartPolicy start = StartPolicy::kCold;

    /// Events between updates in periodic mode.
    std::uint32_t interval = 256;

    /// Hard cap on events applied in one update. See the header: this is what
    /// prevents a backlog from becoming an unbounded pause.
    std::uint32_t max_batch = 512;

    /// Ring-buffer capacity, in events. Each event carries a feature vector, so
    /// the memory cost is capacity * sizeof(PredictionEvent) and is reported by
    /// memory_bytes().
    std::uint32_t buffer_capacity = 4096;

    /// Adaptive mode fires when the exponentially weighted error rate exceeds
    /// this.
    double error_threshold = 0.25;

    /// Observations required before the adaptive trigger is trusted. Without
    /// it, the first unlucky event would fire an update on one sample.
    std::uint64_t adaptive_warmup = 64;

    /// Weight multiplier for an event that ended in a fallback.
    float fallback_penalty = 4.0F;

    /// Responsiveness of the recent-error statistic.
    double error_alpha = 0.05;

    /// Largest absolute parameter value a batch may leave behind. A batch that ends with a
    /// non-finite parameter, or one beyond this, is rolled back (see apply_batch). Fitted logistic
    /// weights on the static features are single digits; the default is far above anything
    /// legitimate, so it catches divergence and nothing else.
    float max_abs_weight = 1000.0F;

    /// Rolled-back batches tolerated before the learner halts. A halted learner stops buffering and
    /// applying; the model stays at its last good parameters and the search carries on.
    std::uint32_t max_rollbacks = 3;

    void validate() const {
        if (interval == 0) {
            throw InvalidConfigError("update interval must be at least 1");
        }
        if (max_batch == 0) {
            throw InvalidConfigError(
                "max_batch must be at least 1; zero would record events forever and never "
                "apply any, which is kDisabled wearing the wrong label");
        }
        if (buffer_capacity == 0) {
            throw InvalidConfigError("buffer_capacity must be at least 1");
        }
        if (!(fallback_penalty > 0.0F)) {
            throw InvalidConfigError("fallback_penalty must be positive");
        }
        if (!(error_threshold >= 0.0) || !(error_threshold <= 1.0)) {
            throw InvalidConfigError("error_threshold must lie within [0, 1]");
        }
        if (!std::isfinite(max_abs_weight) || !(max_abs_weight > 0.0F)) {
            throw InvalidConfigError("max_abs_weight must be finite and positive");
        }
        if (max_rollbacks == 0) {
            throw InvalidConfigError("max_rollbacks must be at least 1");
        }
    }
};

/// Counters for PRD.md section 22 and the online-learning experiments.
struct OnlineLearningMetrics {
    std::uint64_t events_recorded = 0;
    std::uint64_t events_applied  = 0;

    /// Events overwritten before they could be applied. Visible on purpose:
    /// a high rate means the buffer or the interval is misconfigured, and the
    /// learner is seeing a biased sample of its own history.
    std::uint64_t events_dropped = 0;

    std::uint64_t updates_performed = 0;
    std::uint64_t largest_batch     = 0;

    /// Training examples behind those events: an event carries one, or two when the first choice
    /// was wrong. `examples_applied` counts only examples that stayed applied (not rolled back).
    std::uint64_t examples_recorded = 0;
    std::uint64_t examples_applied  = 0;

    /// Recovery (M5-O3). Every recorded event ends in exactly one of: applied, dropped (buffer full),
    /// rolled back, discarded because the learner was disabled or halted, or still pending.
    std::uint64_t rollbacks                = 0;
    std::uint64_t events_rolled_back       = 0;
    std::uint64_t events_discarded_disabled = 0;
    std::uint64_t events_discarded_halted  = 0;

    [[nodiscard]] double drop_rate() const noexcept {
        return events_recorded == 0
                   ? 0.0
                   : static_cast<double>(events_dropped) / static_cast<double>(events_recorded);
    }

    [[nodiscard]] double mean_batch_size() const noexcept {
        return updates_performed == 0 ? 0.0
                                      : static_cast<double>(events_applied) /
                                            static_cast<double>(updates_performed);
    }

    void reset() noexcept { *this = OnlineLearningMetrics{}; }
};

/// Models whose parameters are a weight vector and a bias (LinearPredictor and LogisticPredictor).
/// Only these can be checked for numerical failure and rolled back without copying unbounded state.
template <typename M>
concept HasLinearParameters = requires(const M& m) {
    m.weights();
    m.bias();
};

/// True when every parameter is finite and no weight or bias exceeds `max_abs`.
template <HasLinearParameters M>
[[nodiscard]] inline bool parameters_within(const M& m, float max_abs) noexcept {
    for (const float w : m.weights()) {
        if (!std::isfinite(w) || std::fabs(w) > max_abs) {
            return false;
        }
    }
    return std::isfinite(m.bias()) && std::fabs(m.bias()) <= max_abs;
}

/// FNV-1a over the bit patterns of the weights and the bias: an identity for "did the parameters
/// change", not a cryptographic digest.
template <HasLinearParameters M>
[[nodiscard]] inline std::uint64_t parameters_fnv1a64(const M& m) noexcept {
    std::uint64_t h = 14695981039346656037ULL;
    const auto mix  = [&h](float v) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        for (int i = 0; i < 4; ++i) {
            h ^= static_cast<std::uint64_t>((bits >> (8 * i)) & 0xFFU);
            h *= 1099511628211ULL;
        }
    };
    for (const float w : m.weights()) {
        mix(w);
    }
    mix(m.bias());
    return h;
}

/// Buffers prediction events and applies bounded updates to a model.
///
/// Holds no model reference: the model is passed to each update call. That
/// keeps the learner usable with the compile-time-polymorphic models on the hot
/// path and with a Predictor handle off it, without two implementations.
class OnlineLearner {
public:
    OnlineLearner() { buffer_.resize(config_.buffer_capacity); }

    explicit OnlineLearner(OnlineLearningConfig config) : config_(config) {
        config_.validate();
        buffer_.resize(config_.buffer_capacity);
        recent_error_.set_alpha(config_.error_alpha);
    }

    [[nodiscard]] const OnlineLearningConfig& config() const noexcept { return config_; }

    // --- Recording -----------------------------------------------------------

    /// Records one verified decision. O(1), never allocates.
    void record(const PredictionEvent& event) noexcept {
        ++metrics_.events_recorded;
        metrics_.examples_recorded += event.example_count();
        recent_error_.add(event.was_correct ? 0.0 : 1.0);
        rank_errors_.add(event.rank_error);

        if (halted_) {
            ++metrics_.events_discarded_halted;
            return;
        }

        if (config_.mode == UpdateMode::kDisabled) {
            ++metrics_.events_discarded_disabled;
            // Still counted and still fed to the statistics: the controller and
            // the experiment need prediction quality even when nothing is being
            // learned from it, and the frozen-model control must pay the same
            // observation cost as the learning configurations or the comparison
            // between them would be confounded.
            return;
        }

        if (pending_ == buffer_.size()) {
            // Full: overwrite the oldest.
            head_ = (head_ + 1) % buffer_.size();
            --pending_;
            ++metrics_.events_dropped;
        }

        const std::size_t at = (head_ + pending_) % buffer_.size();
        buffer_[at]          = event;
        ++pending_;
        ++since_update_;
    }

    // --- Applying ------------------------------------------------------------

    /// Applies a bounded batch if one is due. Returns the number applied.
    ///
    /// Safe to call after every extraction: it returns immediately when nothing
    /// is due, which is the common case.
    template <PredictorLike Model>
    std::size_t maybe_update(Model& model) {
        if (!update_due()) {
            return 0;
        }
        return apply_batch(model, config_.max_batch);
    }

    /// Applies everything pending, still in bounded batches.
    ///
    /// Called between queries, where PRD.md section 11 explicitly permits
    /// deferred updates ("model updates can be batched and applied
    /// asynchronously or between queries"). Not called inside a query.
    template <PredictorLike Model>
    std::size_t flush(Model& model) {
        if (config_.mode == UpdateMode::kDisabled) {
            return 0;
        }
        std::size_t total = 0;
        while (pending_ > 0 && !halted_) {
            total += apply_batch(model, config_.max_batch);
        }
        return total;
    }

    /// Whether an update would be applied now.
    [[nodiscard]] bool update_due() const noexcept {
        if (config_.mode == UpdateMode::kDisabled || pending_ == 0 || halted_) {
            return false;
        }
        switch (config_.mode) {
            case UpdateMode::kEveryEvent:
                return true;
            case UpdateMode::kPeriodic:
                return since_update_ >= config_.interval;
            case UpdateMode::kAdaptive:
                // Gated on warmth as well as on the threshold, so a single
                // unlucky early event cannot trigger an update on one sample.
                return recent_error_.warm(config_.adaptive_warmup) &&
                       recent_error_.value() > config_.error_threshold;
            case UpdateMode::kDisabled:
                return false;
        }
        return false;
    }

    // --- Observation ---------------------------------------------------------

    [[nodiscard]] const OnlineLearningMetrics& metrics() const noexcept { return metrics_; }

    /// True once `max_rollbacks` batches have been rolled back. The learner then ignores events.
    [[nodiscard]] bool halted() const noexcept { return halted_; }

    /// Every recorded event is accounted for exactly once.
    [[nodiscard]] bool accounting_balanced() const noexcept {
        const auto& m = metrics_;
        return m.events_recorded == m.events_applied + m.events_dropped + m.events_rolled_back +
                                        m.events_discarded_disabled + m.events_discarded_halted +
                                        pending_;
    }

    /// Exponentially weighted fraction of predictions that were wrong.
    [[nodiscard]] double recent_error_rate() const noexcept { return recent_error_.value(); }

    [[nodiscard]] double mean_rank_error() const noexcept { return rank_errors_.mean(); }

    /// The tail statistic ESD.md section 18 asks for. Exact, not estimated.
    [[nodiscard]] std::uint32_t p95_rank_error() const noexcept {
        return rank_errors_.quantile(0.95);
    }

    [[nodiscard]] std::uint32_t max_rank_error() const noexcept { return rank_errors_.maximum(); }

    [[nodiscard]] std::size_t pending() const noexcept { return pending_; }

    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        return buffer_.capacity() * sizeof(PredictionEvent);
    }

    void reset() noexcept {
        metrics_.reset();
        recent_error_.reset();
        rank_errors_.reset();
        head_         = 0;
        pending_      = 0;
        since_update_ = 0;
        halted_       = false;
    }

private:
    /// Applies up to `limit` buffered events, oldest first.
    ///
    /// Strictly in arrival order: SGD is order-dependent, so reordering would
    /// make two runs from the same seed produce different models.
    template <PredictorLike Model>
    std::size_t apply_batch(Model& model, std::size_t limit) {
        const std::size_t count = pending_ < limit ? pending_ : limit;

        // A copy of the parameters, taken before the batch and used only if the batch leaves them
        // non-finite or out of range. Constant size (a weight vector, a bias, a calibrator), so it
        // adds nothing to the bound on one update.
        [[maybe_unused]] std::optional<Model> before;
        if constexpr (HasLinearParameters<Model>) {
            before = model;
        }

        std::uint64_t examples = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const PredictionEvent& e = buffer_[head_];
            model.update(e.to_example(config_.fallback_penalty));
            ++examples;
            if (e.has_accepted_example) {
                model.update(e.accepted_example());
                ++examples;
            }
            head_ = (head_ + 1) % buffer_.size();
        }
        pending_ -= count;
        since_update_ = 0;

        if constexpr (HasLinearParameters<Model>) {
            if (!parameters_within(model, config_.max_abs_weight)) {
                // Numerical failure: restore the last good parameters and forget this batch. The
                // search never sees the bad ones, because they were never used for a score.
                model = *before;
                ++metrics_.rollbacks;
                metrics_.events_rolled_back += count;
                if (metrics_.rollbacks >= config_.max_rollbacks) {
                    halt();
                }
                return 0;
            }
        }

        ++metrics_.updates_performed;
        metrics_.events_applied += count;
        metrics_.examples_applied += examples;
        if (count > metrics_.largest_batch) {
            metrics_.largest_batch = count;
        }
        return count;
    }

    /// Stops the learner: pending events are discarded (and counted), nothing more is buffered or
    /// applied, and the model keeps its last good parameters.
    void halt() noexcept {
        halted_ = true;
        metrics_.events_discarded_halted += pending_;
        pending_ = 0;
    }

    OnlineLearningConfig         config_{};
    std::vector<PredictionEvent> buffer_;
    std::size_t                  head_         = 0;
    std::size_t                  pending_      = 0;
    std::uint32_t                since_update_ = 0;
    bool                         halted_       = false;

    ExponentiallyWeighted  recent_error_{0.05};
    BoundedHistogram<64>   rank_errors_{};
    OnlineLearningMetrics  metrics_{};
};

}  // namespace lpq

#endif  // LPQ_ADAPTATION_ONLINE_LEARNER_HPP
