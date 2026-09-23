// =============================================================================
// The profiler in its DEFAULT state: compiled out.
//
// A separate translation unit from test_phase_profiler.cpp because the two need
// opposite values of the same macro. This one is built the way every ordinary
// target is built -- without LPQ_ENABLE_PHASE_PROFILING -- and asserts that the
// instrumentation then costs nothing at all.
//
// This matters more than it looks. The probes sit in the hot path of the queue
// the whole project is measuring. If a release build were to retain even the
// clock reads, every timing the project has published would include an overhead
// that no one had accounted for, and the profiler would have corrupted the
// measurements it exists to explain. ESD.md section 4.4 requires instrumentation
// to be zero-cost when disabled; these tests are that requirement made
// executable.
// =============================================================================
#include <gtest/gtest.h>

#include "lpq/metrics/phase_profiler.hpp"

namespace lpq {
namespace {

static_assert(LPQ_ENABLE_PHASE_PROFILING == 0,
              "this target must be built WITHOUT profiling; it exists to prove the "
              "disabled path is inert, which it cannot do if profiling is on");

TEST(PhaseProfilerDisabled, TheScopeMacroCompilesToNothing) {
    // No object is constructed, so nothing is recorded and there is no local
    // for the optimiser to have to remove.
    {
        LPQ_PROFILE_SCOPE(QueuePhase::kPop);
    }
    EXPECT_EQ(PhaseProfile::instance().record(QueuePhase::kPop).calls, 0U);
    EXPECT_EQ(PhaseProfile::instance().record(QueuePhase::kPop).ticks, 0U);
}

TEST(PhaseProfilerDisabled, TheBeginEndMacrosCompileToNothing) {
    LPQ_PROFILE_BEGIN(region);
    LPQ_PROFILE_END(region, QueuePhase::kWindowMaintenance);

    EXPECT_EQ(PhaseProfile::instance().record(QueuePhase::kWindowMaintenance).calls, 0U);
}

TEST(PhaseProfilerDisabled, TheClockIsNotReadAtAll) {
    // read_ticks() returns a constant zero rather than a cheap clock, so a
    // disabled build cannot accidentally pay for a timestamp.
    EXPECT_EQ(detail::read_ticks(), 0U);
}

TEST(PhaseProfilerDisabled, TheReportIsWellFormedRatherThanAbsent) {
    // The reporting types stay available when profiling is off, so code that
    // prints a profile still compiles in every build. It simply has nothing to
    // print -- which the profiling app says out loud rather than presenting
    // zeroes as a result.
    const PhaseReport rep = build_phase_report();

    EXPECT_EQ(rep.total_ticks, 0U);
    EXPECT_EQ(rep.probe_pairs, 0U);
    EXPECT_EQ(rep.shares.size(), static_cast<std::size_t>(QueuePhase::kCount));
}

}  // namespace
}  // namespace lpq
