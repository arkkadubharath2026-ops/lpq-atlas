// =============================================================================
// Adaptive controller (ESD.md section 18, PRD.md section 14).
//
// ESD.md section 18 gives the control law:
//
//     if performance_gain > upper_threshold: enter learned mode
//     if performance_gain < lower_threshold: enter classical mode
//     where upper_threshold > lower_threshold
//
// and requires exponentially weighted statistics behind it. PRD.md section 14
// adds that "the controller must have hysteresis or equivalent stabilization so
// it does not oscillate rapidly between modes".
//
// =============================================================================
// WHAT "PERFORMANCE GAIN" ACTUALLY IS
// =============================================================================
//
// The specification names the quantity but does not define it, and the choice
// decides whether the controller works at all. Defining it as prediction
// accuracy would be the obvious move and would be wrong: a model can be
// accurate and still lose, because accuracy says nothing about how much work an
// acceptance saved or how much the checking cost.
//
// So gain is defined in the unit the saving actually occurs in -- key
// comparisons per extraction:
//
//     gain = (entries the learned path avoided touching)
//          - (comparisons verification spent to avoid them)
//
// Positive means the learned path is doing less total work than the classical
// scan it replaced. Negative means it is paying more to check than it saves,
// which is precisely the failure mode PRD.md section 10 warns about: "a model
// that predicts perfectly but costs more to feed than the queue saves is a
// failure".
//
// Both terms are measured, not modelled. Nothing here estimates what a scan
// "would have" cost from a formula; the substrate reports the bucket size it
// skipped and the verifier reports the comparisons it spent.
//
// -----------------------------------------------------------------------------
// Why hysteresis alone is not enough
// -----------------------------------------------------------------------------
//
// Two thresholds stop the controller flipping while the signal sits exactly on
// one boundary. They do NOT stop it flipping when the signal oscillates across
// the whole band, which is what happens when a search moves between regions of
// a graph with different structure -- a road network's city core and its rural
// edges, say.
//
// So there is also a dwell time: a minimum number of extractions in a mode
// before another switch is considered. A mode change costs more than the
// comparison that triggered it -- it invalidates the warm statistics the next
// decision will be made from -- so changing modes every few extractions is
// worse than being in the wrong mode briefly.
// =============================================================================
#ifndef LPQ_ADAPTATION_ADAPTIVE_CONTROLLER_HPP
#define LPQ_ADAPTATION_ADAPTIVE_CONTROLLER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "lpq/adaptation/ewma.hpp"
#include "lpq/common/error.hpp"
#include "lpq/lpq/candidate_manager.hpp"
#include "lpq/verification/correction_engine.hpp"

namespace lpq {

/// The behaviours the controller selects between (PRD.md section 14).
enum class QueueMode : std::uint8_t {
    /// Narrow window, trust the model. Cheapest per extraction and the most
    /// sensitive to a bad prediction.
    kAggressiveLearned = 0,

    /// Wider window, so verification succeeds more often at more cost per
    /// attempt. The right mode when predictions are useful but imprecise.
    kConservativeLearned,

    /// Learned path with no expansion: one attempt, then straight to the
    /// classical scan. Cheap to be wrong in, so it suits a model that is right
    /// often enough to be worth trying and not often enough to be worth
    /// retrying.
    kHybrid,

    /// The learned path is skipped entirely. The queue behaves as the classical
    /// bucket queue, paying none of the LPQ overhead.
    kClassical,
};

[[nodiscard]] inline constexpr std::string_view queue_mode_name(QueueMode m) noexcept {
    switch (m) {
        case QueueMode::kAggressiveLearned:   return "aggressive_learned";
        case QueueMode::kConservativeLearned: return "conservative_learned";
        case QueueMode::kHybrid:              return "hybrid";
        case QueueMode::kClassical:           return "classical";
    }
    return "unknown";
}

/// What a mode means operationally.
struct QueuePolicy {
    bool          use_learned_path = true;
    std::uint32_t window_size      = 4;
    std::uint32_t max_expansions   = 2;
};

struct ControllerConfig {
    /// Gain, in comparisons saved per extraction, above which the controller
    /// becomes more aggressive.
    double upper_threshold = 2.0;

    /// Gain below which it retreats. Strictly less than upper_threshold; the
    /// band between them is the hysteresis.
    double lower_threshold = 0.0;

    /// Extractions the controller must observe before it will change mode at
    /// all. Without it the first few extractions -- which run against a cold
    /// model and an empty queue, and are unrepresentative of anything -- would
    /// decide the mode for the rest of the query.
    std::uint64_t warmup_extractions = 256;

    /// Minimum extractions between mode changes. See the header on why two
    /// thresholds are not sufficient on their own.
    std::uint64_t dwell_extractions = 512;

    /// Responsiveness of the gain estimate.
    double alpha = 0.02;

    /// Window sizes for the learned modes.
    std::uint32_t aggressive_window   = 2;
    std::uint32_t conservative_window = 16;

    QueueMode initial_mode = QueueMode::kHybrid;

    void validate() const {
        if (!(upper_threshold > lower_threshold)) {
            throw InvalidConfigError(
                "upper_threshold must exceed lower_threshold; equal or inverted thresholds "
                "remove the hysteresis band and the controller would oscillate");
        }
        if (aggressive_window == 0 || conservative_window == 0) {
            throw InvalidConfigError("controller window sizes must be at least 1");
        }
        if (conservative_window < aggressive_window) {
            throw InvalidConfigError(
                "conservative_window must be at least aggressive_window, or the modes are "
                "misnamed and every experiment reading them would be misinterpreted");
        }
        if (!(alpha > 0.0) || !(alpha <= 1.0)) {
            throw InvalidConfigError("controller alpha must lie within (0, 1]");
        }
    }
};

/// Counters describing the controller's own behaviour.
struct ControllerMetrics {
    std::uint64_t observations  = 0;
    std::uint64_t mode_changes  = 0;

    /// Switches the dwell time suppressed. A high value means the gain signal
    /// is oscillating across the whole hysteresis band and the band is too
    /// narrow for this workload -- visible rather than merely felt as jitter.
    std::uint64_t suppressed_by_dwell = 0;

    /// Extractions spent in each mode, indexed by QueueMode.
    std::array<std::uint64_t, 4> extractions_in_mode{};

    void reset() noexcept { *this = ControllerMetrics{}; }
};

/// Selects a queue mode from measured gain, with hysteresis and dwell.
class AdaptiveController {
public:
    AdaptiveController() { mode_ = config_.initial_mode; }

    explicit AdaptiveController(ControllerConfig config) : config_(config) {
        config_.validate();
        mode_ = config_.initial_mode;
        gain_.set_alpha(config_.alpha);
    }

    [[nodiscard]] const ControllerConfig& config() const noexcept { return config_; }

    // --- Observation ---------------------------------------------------------

    /// Records one extraction's outcome.
    ///
    /// `entries_skipped` is how many queued entries the learned path avoided
    /// touching -- zero on a fallback, because a fallback touched all of them.
    /// `verification_comparisons` is what the checking cost.
    void observe(std::uint64_t entries_skipped, std::uint32_t verification_comparisons,
                 const CorrectionOutcome& outcome) noexcept {
        ++metrics_.observations;
        ++metrics_.extractions_in_mode[static_cast<std::size_t>(mode_)];
        ++since_change_;

        gain_.add(static_cast<double>(entries_skipped) -
                  static_cast<double>(verification_comparisons));
        correct_.add(outcome.path == ResolutionPath::kFirstAttempt ? 1.0 : 0.0);
        fallback_.add(outcome.path == ResolutionPath::kFallback ? 1.0 : 0.0);
        rank_errors_.add(outcome.verification.rank_displacement);

        reconsider();
    }

    /// Records a timing, when the build is instrumented to collect one.
    ///
    /// Separate from observe() because reading a clock costs on the order of
    /// twenty nanoseconds, which is comparable to the work being timed. Calling
    /// this per extraction in a timed run would change the number it reports,
    /// so the benchmark leaves it alone and the profiling configuration uses
    /// it.
    void observe_inference_time(double nanoseconds) noexcept { inference_ns_.add(nanoseconds); }
    void observe_feature_time(double nanoseconds) noexcept { feature_ns_.add(nanoseconds); }
    void observe_verification_time(double nanoseconds) noexcept {
        verification_ns_.add(nanoseconds);
    }
    void observe_queue_size(double entries) noexcept { queue_size_.add(entries); }

    // --- Decision ------------------------------------------------------------

    [[nodiscard]] QueueMode mode() const noexcept { return mode_; }

    [[nodiscard]] QueuePolicy policy() const noexcept {
        QueuePolicy p;
        switch (mode_) {
            case QueueMode::kAggressiveLearned:
                p.use_learned_path = true;
                p.window_size      = config_.aggressive_window;
                p.max_expansions   = 1;
                break;
            case QueueMode::kConservativeLearned:
                p.use_learned_path = true;
                p.window_size      = config_.conservative_window;
                p.max_expansions   = 3;
                break;
            case QueueMode::kHybrid:
                p.use_learned_path = true;
                p.window_size      = config_.aggressive_window;
                p.max_expansions   = 0;  // one attempt, then the classical scan
                break;
            case QueueMode::kClassical:
                p.use_learned_path = false;
                p.window_size      = 0;
                p.max_expansions   = 0;
                break;
        }
        return p;
    }

    /// Forces a mode, bypassing the control law.
    ///
    /// For the ablation in PRD.md section 21, which needs each mode measured in
    /// isolation. Resets the dwell counter so the controller does not
    /// immediately switch away from what the experiment just selected.
    void force_mode(QueueMode m) noexcept {
        if (m != mode_) {
            ++metrics_.mode_changes;
        }
        mode_         = m;
        since_change_ = 0;
    }

    // --- Observation of the controller itself (ESD.md section 18) ------------

    [[nodiscard]] double recent_gain() const noexcept { return gain_.value(); }
    [[nodiscard]] double prediction_success_rate() const noexcept { return correct_.value(); }
    [[nodiscard]] double correction_rate() const noexcept { return 1.0 - correct_.value(); }
    [[nodiscard]] double fallback_rate() const noexcept { return fallback_.value(); }
    [[nodiscard]] double mean_rank_error() const noexcept { return rank_errors_.mean(); }
    [[nodiscard]] std::uint32_t p95_rank_error() const noexcept {
        return rank_errors_.quantile(0.95);
    }
    [[nodiscard]] double mean_inference_ns() const noexcept { return inference_ns_.value(); }
    [[nodiscard]] double mean_feature_ns() const noexcept { return feature_ns_.value(); }
    [[nodiscard]] double mean_verification_ns() const noexcept {
        return verification_ns_.value();
    }
    [[nodiscard]] double mean_queue_size() const noexcept { return queue_size_.value(); }

    [[nodiscard]] const ControllerMetrics& metrics() const noexcept { return metrics_; }

    void reset() noexcept {
        metrics_.reset();
        gain_.reset();
        correct_.reset();
        fallback_.reset();
        rank_errors_.reset();
        inference_ns_.reset();
        feature_ns_.reset();
        verification_ns_.reset();
        queue_size_.reset();
        mode_         = config_.initial_mode;
        since_change_ = 0;
    }

private:
    /// Applies the control law of ESD.md section 18.
    void reconsider() noexcept {
        if (metrics_.observations < config_.warmup_extractions) {
            return;
        }

        const QueueMode target = target_mode();
        if (target == mode_) {
            return;
        }

        if (since_change_ < config_.dwell_extractions) {
            ++metrics_.suppressed_by_dwell;
            return;
        }

        mode_         = target;
        since_change_ = 0;
        ++metrics_.mode_changes;
    }

    /// The mode the current gain argues for.
    ///
    /// Note the asymmetry: the retreat from a learned mode is unconditional
    /// once the gain falls below the lower threshold, while advancing requires
    /// clearing the upper one. That is the hysteresis, and it is deliberately
    /// biased towards giving up: a learned mode that is losing costs time on
    /// every extraction, whereas the classical mode is never worse than the
    /// queue it is built on.
    [[nodiscard]] QueueMode target_mode() const noexcept {
        const double gain = gain_.value();

        if (gain < config_.lower_threshold) {
            // Losing. Step down one level rather than straight to classical, so
            // a temporary dip does not discard a model that is merely being
            // used too aggressively.
            switch (mode_) {
                case QueueMode::kAggressiveLearned:   return QueueMode::kConservativeLearned;
                case QueueMode::kConservativeLearned: return QueueMode::kHybrid;
                case QueueMode::kHybrid:              return QueueMode::kClassical;
                case QueueMode::kClassical:           return QueueMode::kClassical;
            }
        }

        if (gain > config_.upper_threshold) {
            switch (mode_) {
                case QueueMode::kClassical:           return QueueMode::kHybrid;
                case QueueMode::kHybrid:              return QueueMode::kConservativeLearned;
                case QueueMode::kConservativeLearned: return QueueMode::kAggressiveLearned;
                case QueueMode::kAggressiveLearned:   return QueueMode::kAggressiveLearned;
            }
        }

        // Inside the band: hold. This is the whole point of the band.
        return mode_;
    }

    ControllerConfig      config_{};
    QueueMode             mode_         = QueueMode::kHybrid;
    std::uint64_t         since_change_ = 0;

    ExponentiallyWeighted gain_{0.02};
    ExponentiallyWeighted correct_{0.02};
    ExponentiallyWeighted fallback_{0.02};
    ExponentiallyWeighted inference_ns_{0.02};
    ExponentiallyWeighted feature_ns_{0.02};
    ExponentiallyWeighted verification_ns_{0.02};
    ExponentiallyWeighted queue_size_{0.02};
    BoundedHistogram<64>  rank_errors_{};
    ControllerMetrics     metrics_{};
};

}  // namespace lpq

#endif  // LPQ_ADAPTATION_ADAPTIVE_CONTROLLER_HPP
