// =============================================================================
// Tests for synthetic weight assignment (PRD.md section 17, ESD.md section 28).
//
// The properties under test are the ones the design depends on, not the shapes
// of the distributions themselves:
//
//   * symmetry -- w(u,v) == w(v,u), without which an undirected graph silently
//     stops being undirected,
//   * purity -- the same edge and seed always gives the same weight, on any
//     traversal order,
//   * bounds -- no draw escapes [minimum, maximum], so no weight can overflow
//     the weight type or become free,
//   * seed sensitivity -- a different seed gives a genuinely different graph.
//
// The distributional claims are checked only coarsely (ordering of means,
// presence of a tail). A goodness-of-fit test would be testing the standard
// library's transcendental functions, and the families exist here to stress the
// queues, not to be published as samplers.
// =============================================================================
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "lpq/common/rng.hpp"
#include "lpq/graph/graph_builder.hpp"
#include "lpq/graph/weight_distributions.hpp"

namespace lpq {
namespace {

using Weight = std::uint32_t;

WeightDistributionConfig make_config(WeightDistribution kind, SeedType seed = 7) {
    WeightDistributionConfig cfg;
    cfg.kind    = kind;
    cfg.minimum = 1.0;
    cfg.maximum = 1000.0;
    cfg.seed    = seed;
    return cfg;
}

const std::vector<WeightDistribution>& all_kinds() {
    static const std::vector<WeightDistribution> kinds = {
        WeightDistribution::kConstant,    WeightDistribution::kUniform,
        WeightDistribution::kLogNormal,   WeightDistribution::kExponential,
        WeightDistribution::kNarrowRange, WeightDistribution::kHeavyTailed,
    };
    return kinds;
}

// -----------------------------------------------------------------------------
// Core invariants
// -----------------------------------------------------------------------------

TEST(WeightDistributions, EveryFamilyIsSymmetricInItsEndpoints) {
    // The invariant that keeps an undirected graph undirected.
    for (const WeightDistribution kind : all_kinds()) {
        const WeightDistributionConfig cfg = make_config(kind);
        for (NodeId u = 0; u < 64; ++u) {
            for (NodeId v = 0; v < 64; ++v) {
                EXPECT_DOUBLE_EQ(sample_weight(cfg, u, v), sample_weight(cfg, v, u))
                    << "kind " << weight_distribution_name(kind) << ", edge {" << u << ',' << v
                    << '}';
            }
        }
    }
}

TEST(WeightDistributions, EveryFamilyStaysWithinItsBounds) {
    for (const WeightDistribution kind : all_kinds()) {
        const WeightDistributionConfig cfg = make_config(kind);
        for (NodeId u = 0; u < 200; ++u) {
            for (NodeId v = u; v < 200; ++v) {
                const double w = sample_weight(cfg, u, v);
                EXPECT_TRUE(std::isfinite(w)) << weight_distribution_name(kind);
                EXPECT_GE(w, cfg.minimum) << weight_distribution_name(kind);
                EXPECT_LE(w, cfg.maximum) << weight_distribution_name(kind);
            }
        }
    }
}

TEST(WeightDistributions, TheSameEdgeAndSeedAlwaysGiveTheSameWeight) {
    for (const WeightDistribution kind : all_kinds()) {
        const WeightDistributionConfig cfg = make_config(kind);
        for (NodeId u = 0; u < 50; ++u) {
            const double first = sample_weight(cfg, u, u + 1);
            for (int repeat = 0; repeat < 5; ++repeat) {
                EXPECT_DOUBLE_EQ(first, sample_weight(cfg, u, u + 1));
            }
        }
    }
}

TEST(WeightDistributions, ADifferentSeedGivesADifferentAssignment) {
    const WeightDistributionConfig a = make_config(WeightDistribution::kUniform, 1);
    const WeightDistributionConfig b = make_config(WeightDistribution::kUniform, 2);

    std::size_t differing = 0;
    for (NodeId u = 0; u < 200; ++u) {
        if (sample_weight(a, u, u + 1) != sample_weight(b, u, u + 1)) {
            ++differing;
        }
    }
    // Not "all differ": two seeds may coincide on an individual edge. The claim
    // is that the assignment as a whole changed.
    EXPECT_GT(differing, 190U);
}

TEST(WeightDistributions, AdjacentEdgesAreNotCorrelated) {
    // Road-network node ids are heavily sequential, so a weak mix would give
    // neighbouring edges near-identical weights and quietly turn every graph
    // into a locally-constant-weight one.
    const WeightDistributionConfig cfg = make_config(WeightDistribution::kUniform);

    double sum_abs_step = 0.0;
    const int kSamples  = 500;
    for (NodeId u = 0; u < kSamples; ++u) {
        sum_abs_step += std::abs(sample_weight(cfg, u, u + 1) - sample_weight(cfg, u + 1, u + 2));
    }
    // For independent uniforms on [1, 1000] the mean absolute difference is
    // about a third of the range. A correlated generator would fall far below.
    EXPECT_GT(sum_abs_step / kSamples, 200.0);
}

// -----------------------------------------------------------------------------
// Shape, checked coarsely
// -----------------------------------------------------------------------------

TEST(WeightDistributions, NarrowRangeIsTighterThanUniform) {
    const WeightDistributionConfig narrow  = make_config(WeightDistribution::kNarrowRange);
    const WeightDistributionConfig uniform = make_config(WeightDistribution::kUniform);

    double narrow_lo = 1e18, narrow_hi = -1e18;
    double uniform_lo = 1e18, uniform_hi = -1e18;
    for (NodeId u = 0; u < 2000; ++u) {
        const double n = sample_weight(narrow, u, u + 1);
        const double v = sample_weight(uniform, u, u + 1);
        narrow_lo = std::min(narrow_lo, n);
        narrow_hi = std::max(narrow_hi, n);
        uniform_lo = std::min(uniform_lo, v);
        uniform_hi = std::max(uniform_hi, v);
    }
    EXPECT_LT(narrow_hi - narrow_lo, 0.25 * (uniform_hi - uniform_lo));
}

TEST(WeightDistributions, HeavyTailedProducesAFewVeryLargeWeights) {
    WeightDistributionConfig cfg = make_config(WeightDistribution::kHeavyTailed);
    cfg.alpha                    = 1.2;

    std::vector<double> draws;
    draws.reserve(5000);
    for (NodeId u = 0; u < 5000; ++u) {
        draws.push_back(sample_weight(cfg, u, u + 1));
    }
    std::sort(draws.begin(), draws.end());

    const double median = draws[draws.size() / 2];
    const double top    = draws.back();

    // The defining property: the extreme is orders of magnitude above the bulk.
    EXPECT_GT(top, 50.0 * median);
}

TEST(WeightDistributions, ExponentialConcentratesBelowTheMidpoint) {
    const WeightDistributionConfig cfg = make_config(WeightDistribution::kExponential);

    std::size_t below = 0;
    const int   kN    = 4000;
    for (NodeId u = 0; u < kN; ++u) {
        if (sample_weight(cfg, u, u + 1) < 0.5 * (cfg.minimum + cfg.maximum)) {
            ++below;
        }
    }
    EXPECT_GT(below, static_cast<std::size_t>(0.75 * kN));
}

TEST(WeightDistributions, ConstantIgnoresTheEndpointsEntirely) {
    const WeightDistributionConfig cfg = make_config(WeightDistribution::kConstant);
    for (NodeId u = 0; u < 100; ++u) {
        EXPECT_DOUBLE_EQ(sample_weight(cfg, u, u + 1), cfg.minimum);
    }
}

// -----------------------------------------------------------------------------
// Quantization
// -----------------------------------------------------------------------------

TEST(WeightDistributions, QuantizationNeverProducesAFreeEdge) {
    // A weight that rounds to zero would make an edge free, which changes the
    // shortest-path problem rather than approximating it.
    WeightDistributionConfig cfg = make_config(WeightDistribution::kUniform);
    cfg.minimum                  = 0.4;
    cfg.maximum                  = 0.6;

    for (NodeId u = 0; u < 500; ++u) {
        const Weight w = quantize_weight<Weight>(sample_weight(cfg, u, u + 1), cfg);
        EXPECT_GE(w, 1U);
    }
}

TEST(WeightDistributions, QuantizationAllowsZeroWhenZeroWasConfigured) {
    WeightDistributionConfig cfg = make_config(WeightDistribution::kConstant);
    cfg.minimum                  = 0.0;
    cfg.maximum                  = 0.0;
    EXPECT_EQ(quantize_weight<Weight>(0.0, cfg), 0U);
}

// -----------------------------------------------------------------------------
// Configuration validation
// -----------------------------------------------------------------------------

TEST(WeightDistributions, RejectsNegativeMinimum) {
    WeightDistributionConfig cfg = make_config(WeightDistribution::kUniform);
    cfg.minimum                  = -1.0;
    EXPECT_THROW(cfg.validate(), InvalidConfigError);
}

TEST(WeightDistributions, RejectsInvertedBounds) {
    WeightDistributionConfig cfg = make_config(WeightDistribution::kUniform);
    cfg.minimum                  = 100.0;
    cfg.maximum                  = 10.0;
    EXPECT_THROW(cfg.validate(), InvalidConfigError);
}

TEST(WeightDistributions, RejectsNonPositiveLogNormalSigma) {
    WeightDistributionConfig cfg = make_config(WeightDistribution::kLogNormal);
    cfg.sigma                    = 0.0;
    EXPECT_THROW(cfg.validate(), InvalidConfigError);
}

TEST(WeightDistributions, RejectsNonPositiveHeavyTailedAlpha) {
    WeightDistributionConfig cfg = make_config(WeightDistribution::kHeavyTailed);
    cfg.alpha                    = -0.5;
    EXPECT_THROW(cfg.validate(), InvalidConfigError);
}

TEST(WeightDistributions, ParsesEveryFamilyNameItPrints) {
    for (const WeightDistribution kind : all_kinds()) {
        WeightDistribution parsed = WeightDistribution::kConstant;
        ASSERT_TRUE(parse_weight_distribution(weight_distribution_name(kind), parsed))
            << weight_distribution_name(kind);
        EXPECT_EQ(parsed, kind);
    }
}

TEST(WeightDistributions, RejectsAnUnknownFamilyNameRatherThanDefaulting) {
    WeightDistribution parsed = WeightDistribution::kHeavyTailed;
    EXPECT_FALSE(parse_weight_distribution("uniforn", parsed));
    // Left untouched, so a caller that ignores the result cannot silently run
    // a different experiment from the one it configured.
    EXPECT_EQ(parsed, WeightDistribution::kHeavyTailed);
}

// -----------------------------------------------------------------------------
// Application to a graph
// -----------------------------------------------------------------------------

CsrGraph<Weight> make_undirected_ring(std::size_t n) {
    std::vector<InputEdge<Weight>> edges;
    for (std::size_t u = 0; u < n; ++u) {
        edges.push_back(InputEdge<Weight>{static_cast<NodeId>(u),
                                          static_cast<NodeId>((u + 1) % n), 1U});
    }
    GraphBuildOptions opts;
    opts.directedness = Directedness::kUndirected;
    return GraphBuilder<Weight>::build(n, std::move(edges), opts);
}

TEST(SynthesizeWeights, KeepsTheStructureAndTheDirectednessLabel) {
    const CsrGraph<Weight> original = make_undirected_ring(64);
    const CsrGraph<Weight> weighted =
        synthesize_weights<Weight>(original, make_config(WeightDistribution::kUniform));

    EXPECT_EQ(weighted.num_nodes(), original.num_nodes());
    EXPECT_EQ(weighted.num_arcs(), original.num_arcs());
    // The label matters: num_edges() halves the arc count only for undirected
    // graphs, so losing it would double the reported edge count in metadata.
    EXPECT_EQ(weighted.directedness(), original.directedness());
    EXPECT_EQ(weighted.num_edges(), original.num_edges());

    for (NodeId u = 0; u < original.num_nodes(); ++u) {
        ASSERT_EQ(weighted.out_degree(u), original.out_degree(u));
    }
}

TEST(SynthesizeWeights, ProducesTheSameWeightForBothArcsOfAnUndirectedEdge) {
    const CsrGraph<Weight> g =
        synthesize_weights<Weight>(make_undirected_ring(128),
                                   make_config(WeightDistribution::kLogNormal));

    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        for (const Arc<Weight>& a : g.out_arcs(u)) {
            // Find the reverse arc and compare.
            bool found = false;
            for (const Arc<Weight>& back : g.out_arcs(a.target)) {
                if (back.target == u) {
                    EXPECT_EQ(back.weight, a.weight)
                        << "asymmetric weight on edge {" << u << ',' << a.target << '}';
                    found = true;
                    break;
                }
            }
            ASSERT_TRUE(found) << "missing reverse arc for " << u << " -> " << a.target;
        }
    }
}

TEST(SynthesizeWeights, IsIndependentOfTheOrderTheEdgesWereSuppliedIn) {
    // The reason weights are hashed rather than streamed: a reconversion that
    // presents the same edges in a different order must produce the same graph.
    const std::size_t n = 100;

    std::vector<InputEdge<Weight>> forward;
    for (std::size_t u = 0; u + 1 < n; ++u) {
        forward.push_back(InputEdge<Weight>{static_cast<NodeId>(u),
                                            static_cast<NodeId>(u + 1), 1U});
    }
    std::vector<InputEdge<Weight>> shuffled = forward;
    Rng                            rng(20240917);
    rng.shuffle(shuffled);

    GraphBuildOptions opts;
    opts.directedness = Directedness::kUndirected;

    const CsrGraph<Weight> a = synthesize_weights<Weight>(
        GraphBuilder<Weight>::build(n, std::move(forward), opts),
        make_config(WeightDistribution::kHeavyTailed));
    const CsrGraph<Weight> b = synthesize_weights<Weight>(
        GraphBuilder<Weight>::build(n, std::move(shuffled), opts),
        make_config(WeightDistribution::kHeavyTailed));

    ASSERT_EQ(a.num_arcs(), b.num_arcs());
    for (NodeId u = 0; u < a.num_nodes(); ++u) {
        const auto lhs = a.out_arcs(u);
        const auto rhs = b.out_arcs(u);
        ASSERT_EQ(lhs.size(), rhs.size()) << "node " << u;
        for (std::size_t i = 0; i < lhs.size(); ++i) {
            EXPECT_EQ(lhs[i].target, rhs[i].target) << "node " << u;
            EXPECT_EQ(lhs[i].weight, rhs[i].weight) << "node " << u;
        }
    }
}

TEST(SynthesizeWeights, RejectsAnInvalidConfigurationBeforeTouchingTheGraph) {
    WeightDistributionConfig cfg = make_config(WeightDistribution::kUniform);
    cfg.maximum                  = -1.0;
    const CsrGraph<Weight> g = make_undirected_ring(8);
    EXPECT_THROW({ [[maybe_unused]] const auto ignored = synthesize_weights<Weight>(g, cfg); },
                 InvalidConfigError);
}

}  // namespace
}  // namespace lpq
