// =============================================================================
// The predictor interface (ESD.md section 12).
//
// ESD.md section 12 asks for both things at once:
//
//     class Predictor {
//     public:
//         virtual Prediction predict(const FeatureVector&) = 0;
//         virtual void update(const TrainingExample&) = 0;
//     };
//
//     "Again, production hot paths should use compile-time polymorphism where
//      possible."
//
// Those pull in opposite directions, so both exist and each is used where it is
// correct.
//
// -----------------------------------------------------------------------------
// Two dispatch mechanisms, one contract
// -----------------------------------------------------------------------------
//
// PredictorLike is a concept. Templates over it -- the LPQ search, the
// candidate manager -- are instantiated per model, so predict() inlines into
// the caller. This matters more than it usually would: inference happens once
// per candidate evaluation, millions of times per query, and the work inside is
// a dot product of a few dozen floats. A virtual call there would cost an
// indirect branch and, worse, would block inlining of a body that is otherwise
// small enough to vectorise. On a hot path that cheap, the dispatch can rival
// the arithmetic.
//
// Predictor is the abstract class ESD.md specifies. It is used off the hot
// path, where a uniform runtime type is worth an indirect call: model-comparison
// tooling that iterates over a heterogeneous list, offline evaluation, command
// line selection of a model by name. PredictorAdapter wraps any concept-
// satisfying model into it, so a model is written once and works in both worlds
// without implementing anything twice.
//
// -----------------------------------------------------------------------------
// Why predict() is const and update() is not
// -----------------------------------------------------------------------------
//
// Inference must not mutate the model. If it did, two candidates evaluated in
// one extraction could see different models, predictions would depend on
// evaluation order, and a run would stop being reproducible from its seed. The
// const qualifier makes that a compile error rather than a convention -- and it
// is also what allows a single model to be shared across threads by the offline
// evaluation harness, which has no other synchronisation.
// =============================================================================
#ifndef LPQ_MODELS_PREDICTOR_HPP
#define LPQ_MODELS_PREDICTOR_HPP

#include <concepts>
#include <cstddef>
#include <fstream>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "lpq/common/error.hpp"
#include "lpq/features/feature_vector.hpp"
#include "lpq/models/prediction.hpp"
#include "lpq/models/serialization.hpp"

namespace lpq {

/// What the hot path requires of a model.
template <typename P>
concept PredictorLike = requires(P& p, const P& cp, const FeatureVector& x,
                                 const TrainingExample& ex, std::ostream& os) {
    // Inference. Const: see the header.
    { cp.predict(x) } -> std::same_as<Prediction>;

    // One online update. A model that does not learn online implements this as
    // a no-op rather than omitting it, so callers need no conditional.
    { p.update(ex) } -> std::same_as<void>;

    // Identity, known at compile time so the adapter needs no storage for it.
    { P::kKind } -> std::convertible_to<ModelKind>;
    { P::kName } -> std::convertible_to<std::string_view>;

    // Whether update() does anything. Reported rather than inferred, because a
    // no-op update is indistinguishable from a real one that had no effect.
    { cp.supports_online_update() } -> std::same_as<bool>;

    // Serialization (ESD.md section 12).
    { cp.save(os) } -> std::same_as<void>;
};

/// The runtime-polymorphic interface from ESD.md section 12.
class Predictor {
public:
    Predictor()                            = default;
    Predictor(const Predictor&)            = default;
    Predictor(Predictor&&)                 = default;
    Predictor& operator=(const Predictor&) = default;
    Predictor& operator=(Predictor&&)      = default;
    virtual ~Predictor()                   = default;

    [[nodiscard]] virtual Prediction predict(const FeatureVector& features) const = 0;
    virtual void                     update(const TrainingExample& example)       = 0;

    [[nodiscard]] virtual ModelKind        kind() const noexcept                  = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept                  = 0;
    [[nodiscard]] virtual bool             supports_online_update() const noexcept = 0;

    virtual void save(std::ostream& os) const = 0;

    /// Convenience: predicts and stamps the node id.
    [[nodiscard]] Prediction predict_for(NodeId u, const FeatureVector& features) const {
        Prediction p = predict(features);
        p.node       = u;
        return p;
    }
};

/// Exposes any PredictorLike model through the virtual interface.
///
/// Owns the model by value. A reference-holding variant was considered and
/// rejected: the adapter is used by tooling that builds models into a
/// container, and a dangling reference there would be a use-after-free in code
/// whose whole purpose is convenience.
template <PredictorLike Model>
class PredictorAdapter final : public Predictor {
public:
    PredictorAdapter() = default;

    explicit PredictorAdapter(Model model) : model_(std::move(model)) {}

    [[nodiscard]] Prediction predict(const FeatureVector& features) const override {
        return model_.predict(features);
    }

    void update(const TrainingExample& example) override { model_.update(example); }

    [[nodiscard]] ModelKind        kind() const noexcept override { return Model::kKind; }
    [[nodiscard]] std::string_view name() const noexcept override { return Model::kName; }

    [[nodiscard]] bool supports_online_update() const noexcept override {
        return model_.supports_online_update();
    }

    void save(std::ostream& os) const override { model_.save(os); }

    [[nodiscard]] Model&       model() noexcept { return model_; }
    [[nodiscard]] const Model& model() const noexcept { return model_; }

private:
    Model model_{};
};

/// Wraps a model into a Predictor handle.
template <PredictorLike Model>
[[nodiscard]] std::unique_ptr<Predictor> make_predictor(Model model) {
    return std::make_unique<PredictorAdapter<Model>>(std::move(model));
}

// -----------------------------------------------------------------------------
// File helpers
//
// Streams are opened in binary mode explicitly. On Windows a text-mode stream
// translates 0x0A to 0x0D 0x0A on write, which corrupts a float whose bit
// pattern happens to contain that byte -- roughly one byte in 256, so it would
// hit almost every real model and would look like a subtly wrong weight rather
// than a broken file.
// -----------------------------------------------------------------------------

/// Saves a model to a path.
template <PredictorLike Model>
void save_model(const Model& model, const std::string& path) {
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os) {
        throw ModelSerializationError("cannot open '" + path + "' for writing");
    }
    model.save(os);
    os.flush();
    if (!os) {
        throw ModelSerializationError("failed while writing model to '" + path + "'");
    }
}

/// Loads a model from a path using the model type's own static load().
template <typename Model>
[[nodiscard]] Model load_model(const std::string& path) {
    std::ifstream is(path, std::ios::binary);
    if (!is) {
        throw ModelSerializationError("cannot open '" + path + "' for reading");
    }
    return Model::load(is);
}

/// Round-trips a model through an in-memory buffer.
///
/// Used by tests, and by the export validation in ESD.md section 13, where a
/// model must be shown to survive serialization before it is trusted in a
/// measured run.
template <PredictorLike Model>
[[nodiscard]] Model round_trip(const Model& model) {
    std::stringstream buffer(std::ios::in | std::ios::out | std::ios::binary);
    model.save(buffer);
    buffer.seekg(0);
    return Model::load(buffer);
}

}  // namespace lpq

#endif  // LPQ_MODELS_PREDICTOR_HPP
