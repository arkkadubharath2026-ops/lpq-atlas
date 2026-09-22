// =============================================================================
// Model serialization (ESD.md section 12: "The predictor must support
// serialization", ESD.md section 13: model export from Python).
//
// -----------------------------------------------------------------------------
// Why a hand-written byte format
// -----------------------------------------------------------------------------
//
// The obvious alternatives were rejected for concrete reasons:
//
//   * Writing the struct's bytes directly (memcpy of the weight array) is not
//     portable. It bakes in the host's endianness and the compiler's padding,
//     so a model exported on one machine may load as garbage on another --
//     silently, because the bytes are still readable as floats.
//
//   * A third-party format (protobuf, flatbuffers, ONNX) would add a dependency
//     and a build step for what is, for a linear model, a version tag and an
//     array of floats.
//
// So the format is explicit: fixed-width little-endian integers, floats via
// their IEEE-754 bit pattern, every field written and read in a declared order.
// That is portable across compilers and architectures and is readable from
// Python with struct.unpack, which is what makes the ESD.md section 13 export
// path ("train in Python, convert, run inference in C++") a few lines rather
// than a project.
//
// -----------------------------------------------------------------------------
// What the header guards against
// -----------------------------------------------------------------------------
//
// The dangerous failure mode for a model file is not a crash -- it is loading
// successfully and predicting nonsense. Three checks exist to make that
// impossible rather than unlikely:
//
//   * A magic number, so a file that is not a model is rejected as such rather
//     than interpreted as one.
//   * A format version, so an old file meets an error and not a misreading.
//   * The FEATURE LAYOUT version. This is the important one. A model trained
//     against layout 1 and evaluated against layout 2 would read every feature
//     from the wrong column and produce confident, plausible, wrong
//     predictions. Nothing downstream could detect that, because the outputs
//     are the right shape and in the right range. Refusing the load is the only
//     place this can be caught.
// =============================================================================
#ifndef LPQ_MODELS_SERIALIZATION_HPP
#define LPQ_MODELS_SERIALIZATION_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/features/feature_vector.hpp"

namespace lpq {

/// "LPQMODEL" as a little-endian 64-bit value.
inline constexpr std::uint64_t kModelMagic = 0x4C45444F4D51504CULL;

/// Incremented when the container format changes, independently of the feature
/// layout version.
inline constexpr std::uint32_t kModelFormatVersion = 1;

/// Identifies which model a file holds, so loading dispatches on the file's own
/// claim rather than on the caller's expectation.
enum class ModelKind : std::uint32_t {
    kHeuristic    = 1,
    kLinear       = 2,
    kLogistic     = 3,
    kMlp          = 4,
    kTreeEnsemble = 5,
};

[[nodiscard]] inline const char* model_kind_name(ModelKind k) noexcept {
    switch (k) {
        case ModelKind::kHeuristic:    return "heuristic";
        case ModelKind::kLinear:       return "linear";
        case ModelKind::kLogistic:     return "logistic";
        case ModelKind::kMlp:          return "mlp";
        case ModelKind::kTreeEnsemble: return "tree_ensemble";
    }
    return "unknown";
}

/// Raised when a model file is malformed, truncated, or built against an
/// incompatible layout.
class ModelSerializationError : public LpqError {
public:
    explicit ModelSerializationError(const std::string& what) : LpqError(what) {}
};

/// Fields every model file begins with.
struct ModelHeader {
    std::uint64_t magic                  = kModelMagic;
    std::uint32_t format_version         = kModelFormatVersion;
    std::uint32_t feature_layout_version = static_cast<std::uint32_t>(kFeatureLayoutVersion);
    std::uint32_t feature_count          = static_cast<std::uint32_t>(kFeatureCount);
    ModelKind     kind                   = ModelKind::kLinear;
};

// -----------------------------------------------------------------------------
// Primitive encoding
//
// Little-endian regardless of host byte order, assembled a byte at a time so
// the compiler's representation never leaks into the file.
// -----------------------------------------------------------------------------
namespace detail {

inline void put_u32(std::ostream& os, std::uint32_t v) {
    char b[4];
    for (int i = 0; i < 4; ++i) {
        b[i] = static_cast<char>((v >> (8 * i)) & 0xFFU);
    }
    os.write(b, 4);
}

inline void put_u64(std::ostream& os, std::uint64_t v) {
    char b[8];
    for (int i = 0; i < 8; ++i) {
        b[i] = static_cast<char>((v >> (8 * i)) & 0xFFULL);
    }
    os.write(b, 8);
}

[[nodiscard]] inline std::uint32_t get_u32(std::istream& is) {
    char b[4];
    if (!is.read(b, 4)) {
        throw ModelSerializationError("model file truncated while reading a 32-bit field");
    }
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(static_cast<unsigned char>(b[i])) << (8 * i);
    }
    return v;
}

[[nodiscard]] inline std::uint64_t get_u64(std::istream& is) {
    char b[8];
    if (!is.read(b, 8)) {
        throw ModelSerializationError("model file truncated while reading a 64-bit field");
    }
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(b[i])) << (8 * i);
    }
    return v;
}

// Floats travel as their IEEE-754 bit pattern. std::bit_cast would be tidier
// but memcpy is the form that is well-defined in every compiler this project
// builds under, and it compiles to the same single move.
inline void put_f32(std::ostream& os, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u32(os, bits);
}

[[nodiscard]] inline float get_f32(std::istream& is) {
    const std::uint32_t bits = get_u32(is);
    float v = 0.0F;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

inline void put_f64(std::ostream& os, double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u64(os, bits);
}

[[nodiscard]] inline double get_f64(std::istream& is) {
    const std::uint64_t bits = get_u64(is);
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

inline void put_f32_array(std::ostream& os, const std::vector<float>& v) {
    put_u64(os, static_cast<std::uint64_t>(v.size()));
    for (float x : v) {
        put_f32(os, x);
    }
}

/// Reads a float array whose length the file declares.
///
/// max_elements is not defensive decoration. The length is read from the file
/// before any of the data, so a corrupt length would otherwise cause an
/// allocation of that size before the read fails -- turning a malformed file
/// into an out-of-memory abort. The cap bounds the damage to an exception.
[[nodiscard]] inline std::vector<float> get_f32_array(std::istream& is,
                                                      std::size_t max_elements) {
    const std::uint64_t n = get_u64(is);
    if (n > max_elements) {
        throw ModelSerializationError("model file declares an implausible array length (" +
                                      std::to_string(n) + " > " + std::to_string(max_elements) +
                                      "); the file is corrupt or not a model of this kind");
    }
    std::vector<float> v(static_cast<std::size_t>(n));
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] = get_f32(is);
    }
    return v;
}

}  // namespace detail

/// Writes the common header.
inline void write_model_header(std::ostream& os, ModelKind kind) {
    detail::put_u64(os, kModelMagic);
    detail::put_u32(os, kModelFormatVersion);
    detail::put_u32(os, static_cast<std::uint32_t>(kFeatureLayoutVersion));
    detail::put_u32(os, static_cast<std::uint32_t>(kFeatureCount));
    detail::put_u32(os, static_cast<std::uint32_t>(kind));
}

/// Reads and validates the common header.
///
/// Every check here rejects a file that would otherwise load and mispredict.
[[nodiscard]] inline ModelHeader read_model_header(std::istream& is) {
    ModelHeader h;
    h.magic = detail::get_u64(is);
    if (h.magic != kModelMagic) {
        throw ModelSerializationError("not an LPQ model file (bad magic number)");
    }

    h.format_version = detail::get_u32(is);
    if (h.format_version != kModelFormatVersion) {
        throw ModelSerializationError("model format version " +
                                      std::to_string(h.format_version) +
                                      " cannot be read by this build, which writes version " +
                                      std::to_string(kModelFormatVersion));
    }

    h.feature_layout_version = detail::get_u32(is);
    if (h.feature_layout_version != static_cast<std::uint32_t>(kFeatureLayoutVersion)) {
        throw ModelSerializationError(
            "model was trained against feature layout version " +
            std::to_string(h.feature_layout_version) + " but this build uses version " +
            std::to_string(kFeatureLayoutVersion) +
            "; loading it would read every feature from the wrong column and produce "
            "confident wrong predictions, so the load is refused");
    }

    h.feature_count = detail::get_u32(is);
    if (h.feature_count != static_cast<std::uint32_t>(kFeatureCount)) {
        throw ModelSerializationError("model expects " + std::to_string(h.feature_count) +
                                      " features but this build has " +
                                      std::to_string(kFeatureCount));
    }

    const std::uint32_t kind = detail::get_u32(is);
    switch (kind) {
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
            h.kind = static_cast<ModelKind>(kind);
            break;
        default:
            throw ModelSerializationError("model file declares unknown model kind " +
                                          std::to_string(kind));
    }
    return h;
}

/// Reads the header and requires a particular model kind.
inline ModelHeader read_model_header_expecting(std::istream& is, ModelKind expected) {
    const ModelHeader h = read_model_header(is);
    if (h.kind != expected) {
        throw ModelSerializationError(std::string("model file holds a '") +
                                      model_kind_name(h.kind) + "' model but a '" +
                                      model_kind_name(expected) + "' model was expected");
    }
    return h;
}

/// Peeks at a file's model kind without consuming it, so a caller can dispatch.
[[nodiscard]] inline ModelKind peek_model_kind(std::istream& is) {
    const std::istream::pos_type start = is.tellg();
    const ModelHeader h = read_model_header(is);
    is.clear();
    is.seekg(start);
    return h.kind;
}

}  // namespace lpq

#endif  // LPQ_MODELS_SERIALIZATION_HPP
