// =============================================================================
// Tests for the phase profiler (PRD.md section 20).
//
// The thing actually worth testing here is not that a timer counts up. It is
// that the REPORT does not lie:
//
//   * sub-phase shares are expressed against the phase that contains them, not
//     against the whole, so they cannot silently sum past 100%;
//   * pop time that no sub-phase claims is surfaced as a remainder rather than
//     absorbed into a neighbour, because an unexplained remainder is a finding;
//   * the probe-cost calibration lands in a sink that no reported phase reads.
//
// A profiler that quietly redistributes unattributed time is worse than none:
// it produces a tidy breakdown that adds to exactly 100% and points at the
// wrong phase. These tests exist to keep that from happening.
//
// This translation unit is compiled with LPQ_ENABLE_PHASE_PROFILING=1 by its
// CMake target. The disabled build is covered separately, by a static check
// that the macros expand to nothing.
// =============================================================================
#include <gtest/gtest.h>

#include <cstdint>

#include "lpq/metrics/phase_profiler.hpp"

namespace lpq {
namespace {

static_assert(LPQ_ENABLE_PHASE_PROFILING == 1,
              "this test target must be built with profiling enabled, or every "
              "assertion below passes vacuously against zeroed counters");

/// Each test starts from a clean profile: the accumulators are process-global.
class PhaseProfilerTest : public ::testing::Test {
protected:
    void SetUp() override {
        PhaseProfile::instance().reset();
        AllocationCounter::instance().reset();
        AllocationCounter::instance().set_enabled(false);
    }
    void TearDown() override {
        PhaseProfile::instance().reset();
        AllocationCounter::instance().reset();
        AllocationCounter::instance().set_enabled(false);
    }

    /// Adds a known tick total to a phase without touching a clock.
    ///
    /// Synthetic rather than timed on purpose. A test that measured real
    /// elapsed time would be asserting on the host's scheduler, and would fail
    /// on a loaded machine for reasons that have nothing to do with the code.
    static void charge(QueuePhase phase, std::uint64_t ticks, int times = 1) {
        for (int i = 0; i < times; ++i) {
            PhaseProfile::instance().add(phase, ticks);
        }
    }
};

// -----------------------------------------------------------------------------
// Accumulation
// -----------------------------------------------------------------------------

TEST_F(PhaseProfilerTest, AccumulatesTicksAndCallsSeparately) {
    charge(QueuePhase::kPush, 10, 3);

    const PhaseRecord& r = PhaseProfile::instance().record(QueuePhase::kPush);
    EXPECT_EQ(r.ticks, 30U);
    // The call count is what makes probe cost visible in the report, so it is
    // tracked independently rather than derived.
    EXPECT_EQ(r.calls, 3U);
}

TEST_F(PhaseProfilerTest, ResetClearsEveryPhase) {
    charge(QueuePhase::kPush, 10);
    charge(QueuePhase::kPop, 20);
    PhaseProfile::instance().reset();

    EXPECT_EQ(PhaseProfile::instance().record(QueuePhase::kPush).ticks, 0U);
    EXPECT_EQ(PhaseProfile::instance().record(QueuePhase::kPop).calls, 0U);
    EXPECT_EQ(PhaseProfile::instance().total_ticks(), 0U);
}

TEST_F(PhaseProfilerTest, TotalCountsOnlyTheDisjointTopLevelPhases) {
    // Sub-phases are CONTAINED in pop. Adding them to the total would count
    // the same ticks twice and deflate every reported share.
    charge(QueuePhase::kPush, 100);
    charge(QueuePhase::kPop, 900);
    charge(QueuePhase::kVerificationScan, 400);
    charge(QueuePhase::kWindowMaintenance, 300);

    EXPECT_EQ(PhaseProfile::instance().total_ticks(), 1000U);
}

// -----------------------------------------------------------------------------
// The report
// -----------------------------------------------------------------------------

TEST_F(PhaseProfilerTest, TopLevelSharesAreAgainstTheTotal) {
    charge(QueuePhase::kPush, 250);
    charge(QueuePhase::kPop, 750);

    const PhaseReport rep = build_phase_report();
    for (const PhaseShare& s : rep.shares) {
        if (s.phase == QueuePhase::kPush) {
            EXPECT_NEAR(s.share_of_total, 0.25, 1e-12);
        } else if (s.phase == QueuePhase::kPop) {
            EXPECT_NEAR(s.share_of_total, 0.75, 1e-12);
        }
    }
}

TEST_F(PhaseProfilerTest, SubPhaseSharesAreAgainstPopNotTheTotal) {
    // The distinction that keeps the table honest. Verification is 300 of pop's
    // 600 ticks -- half of pop -- but only 30% of the 1000-tick total. Reported
    // against the wrong denominator it would look half as important as it is.
    charge(QueuePhase::kPush, 400);
    charge(QueuePhase::kPop, 600);
    charge(QueuePhase::kVerificationScan, 300);

    const PhaseReport rep = build_phase_report();
    for (const PhaseShare& s : rep.shares) {
        if (s.phase == QueuePhase::kVerificationScan) {
            EXPECT_NEAR(s.share_of_pop, 0.50, 1e-12);
            EXPECT_NEAR(s.share_of_total, 0.30, 1e-12);
        }
    }
}

TEST_F(PhaseProfilerTest, UnattributedPopTimeIsReportedRatherThanAbsorbed) {
    // 1000 ticks in pop, 600 claimed. The missing 400 is the finding, and it
    // must appear as itself instead of being folded into the sub-phases.
    charge(QueuePhase::kPop, 1000);
    charge(QueuePhase::kWindowMaintenance, 350);
    charge(QueuePhase::kVerificationScan, 250);

    const PhaseReport rep = build_phase_report();
    EXPECT_EQ(rep.pop_unattributed_ticks, 400U);
    EXPECT_NEAR(rep.pop_unattributed_share, 0.40, 1e-12);
}

TEST_F(PhaseProfilerTest, TheFallbackScanCountsTowardsPopRatherThanTwice) {
    // pop() calls the fallback scan, so its ticks are inside pop. Treating it
    // as a further sub-phase on top would make the remainder negative; treating
    // it as disjoint would leave it in the remainder and hide it.
    charge(QueuePhase::kPop, 1000);
    charge(QueuePhase::kFallbackScan, 600);

    const PhaseReport rep = build_phase_report();
    EXPECT_EQ(rep.pop_unattributed_ticks, 400U);
}

TEST_F(PhaseProfilerTest, AnOverclaimedPopClampsInsteadOfUnderflowing) {
    // Sub-phase totals should never exceed pop, but they are unsigned: an
    // instrumentation mistake that made them do so would wrap to an enormous
    // remainder and read as a spectacular unexplained cost.
    charge(QueuePhase::kPop, 100);
    charge(QueuePhase::kVerificationScan, 500);

    const PhaseReport rep = build_phase_report();
    EXPECT_EQ(rep.pop_unattributed_ticks, 0U);
}

TEST_F(PhaseProfilerTest, ReportsZeroSharesRatherThanDividingByZeroOnAnEmptyRun) {
    const PhaseReport rep = build_phase_report();

    EXPECT_EQ(rep.total_ticks, 0U);
    for (const PhaseShare& s : rep.shares) {
        EXPECT_DOUBLE_EQ(s.share_of_total, 0.0);
        EXPECT_DOUBLE_EQ(s.share_of_pop, 0.0);
    }
}

TEST_F(PhaseProfilerTest, ProbePairsCountEveryPhaseEntry) {
    charge(QueuePhase::kPush, 1, 5);
    charge(QueuePhase::kPop, 1, 7);
    charge(QueuePhase::kVerificationScan, 1, 7);

    EXPECT_EQ(build_phase_report().probe_pairs, 19U);
}

TEST_F(PhaseProfilerTest, TheCalibrationSinkNeverReachesTheReport) {
    // measure_probe_pair_cost() charges to kCount so that measuring the
    // instrument cannot show up as queue work. If the sink were ever folded in,
    // a calibration of millions of iterations would dominate every share.
    charge(QueuePhase::kPop, 100);
    PhaseProfile::instance().add(QueuePhase::kCount, 1'000'000);

    const PhaseReport rep = build_phase_report();
    EXPECT_EQ(rep.total_ticks, 100U);
    EXPECT_EQ(rep.probe_pairs, 1U);
    for (const PhaseShare& s : rep.shares) {
        EXPECT_NE(s.phase, QueuePhase::kCount);
    }
}

// -----------------------------------------------------------------------------
// Scoped probes
// -----------------------------------------------------------------------------

TEST_F(PhaseProfilerTest, AScopedProbeRecordsExactlyOneCallPerScope) {
    {
        LPQ_PROFILE_SCOPE(QueuePhase::kPush);
    }
    {
        LPQ_PROFILE_SCOPE(QueuePhase::kPush);
    }
    EXPECT_EQ(PhaseProfile::instance().record(QueuePhase::kPush).calls, 2U);
}

TEST_F(PhaseProfilerTest, TwoScopedProbesInOneBlockDoNotCollide) {
    // The macro pastes __LINE__ into the variable name through an indirection.
    // Without it both expansions would declare `lpq_scoped_phase___LINE__` and
    // the second would fail to compile -- or, worse under a different spelling,
    // shadow the first and lose a phase.
    {
        LPQ_PROFILE_SCOPE(QueuePhase::kPush);
        LPQ_PROFILE_SCOPE(QueuePhase::kPop);
    }
    EXPECT_EQ(PhaseProfile::instance().record(QueuePhase::kPush).calls, 1U);
    EXPECT_EQ(PhaseProfile::instance().record(QueuePhase::kPop).calls, 1U);
}

TEST_F(PhaseProfilerTest, ABeginEndPairRecordsARegionThatIsNotAScope) {
    LPQ_PROFILE_BEGIN(region);
    LPQ_PROFILE_END(region, QueuePhase::kBucketTraversal);

    EXPECT_EQ(PhaseProfile::instance().record(QueuePhase::kBucketTraversal).calls, 1U);
}

// -----------------------------------------------------------------------------
// Phase vocabulary
// -----------------------------------------------------------------------------

TEST_F(PhaseProfilerTest, EveryPhaseHasADistinctReadableName) {
    // The report is read by a person deciding where the time goes, so a phase
    // that renders as "?" or shares a name with another is a defect.
    for (std::size_t i = 0; i < static_cast<std::size_t>(QueuePhase::kCount); ++i) {
        const auto             p    = static_cast<QueuePhase>(i);
        const std::string_view name = phase_name(p);
        EXPECT_FALSE(name.empty());
        EXPECT_NE(name, "?");
        for (std::size_t j = i + 1; j < static_cast<std::size_t>(QueuePhase::kCount); ++j) {
            EXPECT_NE(name, phase_name(static_cast<QueuePhase>(j)));
        }
    }
}

TEST_F(PhaseProfilerTest, OnlyPopsOwnSubPhasesAreClassifiedAsNested) {
    EXPECT_FALSE(is_pop_subphase(QueuePhase::kPush));
    EXPECT_FALSE(is_pop_subphase(QueuePhase::kPop));
    // The fallback scan replaces the window path rather than sitting inside it,
    // so it is deliberately not a sub-phase even though pop calls it.
    EXPECT_FALSE(is_pop_subphase(QueuePhase::kFallbackScan));

    EXPECT_TRUE(is_pop_subphase(QueuePhase::kBucketTraversal));
    EXPECT_TRUE(is_pop_subphase(QueuePhase::kWindowMaintenance));
    EXPECT_TRUE(is_pop_subphase(QueuePhase::kVerificationScan));
    EXPECT_TRUE(is_pop_subphase(QueuePhase::kRemoveFromBucket));
}

// -----------------------------------------------------------------------------
// Allocation counting
// -----------------------------------------------------------------------------

TEST_F(PhaseProfilerTest, AllocationCountingIsOffUntilItIsSwitchedOn) {
    // The counter is enabled only around the measured region, so that loading a
    // graph is not charged to the queue.
    AllocationCounter::instance().note_allocation(64);
    EXPECT_EQ(AllocationCounter::instance().allocations(), 0U);

    AllocationCounter::instance().set_enabled(true);
    AllocationCounter::instance().note_allocation(64);
    AllocationCounter::instance().note_allocation(32);
    AllocationCounter::instance().note_deallocation();

    EXPECT_EQ(AllocationCounter::instance().allocations(), 2U);
    EXPECT_EQ(AllocationCounter::instance().deallocations(), 1U);
    EXPECT_EQ(AllocationCounter::instance().bytes(), 96U);
}

TEST_F(PhaseProfilerTest, TheReportCarriesTheAllocationCounts) {
    AllocationCounter::instance().set_enabled(true);
    AllocationCounter::instance().note_allocation(128);
    AllocationCounter::instance().note_deallocation();

    const PhaseReport rep = build_phase_report();
    EXPECT_EQ(rep.allocations, 1U);
    EXPECT_EQ(rep.deallocations, 1U);
    EXPECT_EQ(rep.allocated_bytes, 128U);
}

}  // namespace
}  // namespace lpq
