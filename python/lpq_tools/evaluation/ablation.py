"""The ablation waterfall (ESD.md section 32 item 16, PRD.md section 25).

A waterfall attributes a total to its parts by adding them one at a time. This
project has a specific total to attribute: the learned priority queue is several
times slower than a binary heap while performing fewer comparisons, and three
separate measurements have failed to find the learned component contributing
anything. The waterfall asks which structural step costs what.

The ladder, in order, each step adding exactly one thing to the one above it:

    binary_heap   the classical baseline: a comparison-ordered heap
    radix_heap    + bucketing by key, still entirely classical
    lpq           + the candidate window and the verification engine,
                    with a CONSTANT scorer, so no prediction is involved
    lpq_model     + a trained model in place of the constant scorer

Reading it as a waterfall is the point. `radix_heap` isolates what bucketing
alone does, because the learned queue's substrate is a bucket queue; `lpq`
against `radix_heap` isolates the cost of the window and verification machinery
with prediction held constant; and `lpq_model` against `lpq` isolates the
contribution of prediction itself with the machinery held constant. Each step
changes one thing.

-----------------------------------------------------------------------------
What this reports and what it refuses to
-----------------------------------------------------------------------------

Comparison counts are exact, machine-independent and reproducible to the unit,
so they are reported as facts. Timings are measurements: they are reported with
their dispersion and compared with a paired test, never quoted as a bare ratio.

The two are kept apart deliberately. This project has already measured a queue
performing 12.9% fewer comparisons while running 4.57x slower, so a waterfall
that mixed them would invite exactly the inference that measurement refutes.
"""

from __future__ import annotations

from dataclasses import dataclass

from lpq_tools.evaluation.results import ResultSet, ResultsError

#: The ladder, in order. Each entry is (configuration, what it adds).
WATERFALL = (
    ("binary_heap", "classical comparison-ordered heap"),
    ("radix_heap", "+ bucketing by key (still classical)"),
    ("lpq", "+ candidate window and verification, constant scorer"),
    ("lpq_model", "+ a trained model in place of the constant scorer"),
)


@dataclass(frozen=True)
class WaterfallStep:
    """One rung, and what it changed relative to the rung above."""

    configuration: str
    adds: str

    measured: bool
    reason: str

    comparisons: int
    pops: int

    #: Ratio of this step's comparisons to the previous measured step's. Above
    #: one means this step made the queue do more work.
    comparison_ratio: float

    #: Ratio to the first measured step, so a reader can see cumulative drift
    #: without multiplying the column above.
    comparison_ratio_to_base: float

    median_ns: float
    median_ratio_to_base: float

    #: True when any query in this configuration was flagged noisy or ran
    #: unpinned. Carried per step so a caveat attaches to the rows it applies
    #: to rather than to the table as a whole.
    noisy: bool
    unpinned: bool


def _median(values: list[float]) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return 0.5 * (ordered[middle - 1] + ordered[middle])


def build_waterfall(results: ResultSet, ladder=WATERFALL) -> list[WaterfallStep]:
    """Assembles the waterfall from one results file.

    Configurations absent from the file become steps marked not measured, with a
    reason, rather than being dropped. A missing rung is information -- most
    often that `lpq_model` was not run because no model was supplied -- and a
    silently shorter table would read as a complete one.
    """
    steps: list[WaterfallStep] = []

    base_comparisons = 0
    base_median = 0.0
    previous_comparisons = 0

    for configuration, adds in ladder:
        records = [r for r in results.for_configuration(configuration) if r.supported]

        if not records:
            unsupported = results.for_configuration(configuration)
            reason = (
                unsupported[0].unsupported_reason
                if unsupported and unsupported[0].unsupported_reason
                else "not present in this results file"
            )
            steps.append(
                WaterfallStep(
                    configuration=configuration,
                    adds=adds,
                    measured=False,
                    reason=reason,
                    comparisons=0,
                    pops=0,
                    comparison_ratio=0.0,
                    comparison_ratio_to_base=0.0,
                    median_ns=0.0,
                    median_ratio_to_base=0.0,
                    noisy=False,
                    unpinned=False,
                )
            )
            continue

        comparisons = sum(r.comparisons for r in records)
        pops = sum(r.pops for r in records)
        median = _median([r.median_ns for r in records])

        if base_comparisons == 0:
            base_comparisons = comparisons
            base_median = median
            previous_comparisons = comparisons

        steps.append(
            WaterfallStep(
                configuration=configuration,
                adds=adds,
                measured=True,
                reason="",
                comparisons=comparisons,
                pops=pops,
                comparison_ratio=(
                    comparisons / previous_comparisons if previous_comparisons else 0.0
                ),
                comparison_ratio_to_base=(
                    comparisons / base_comparisons if base_comparisons else 0.0
                ),
                median_ns=median,
                median_ratio_to_base=(median / base_median if base_median else 0.0),
                noisy=any(r.noisy for r in records),
                unpinned=any(not r.cpu_pinned for r in records),
            )
        )
        previous_comparisons = comparisons

    if not any(step.measured for step in steps):
        raise ResultsError(
            "no configuration in the waterfall was measured in this results file"
        )
    return steps


def format_waterfall(steps: list[WaterfallStep]) -> str:
    """A table that cannot be read as a timing claim it does not support."""
    lines = [
        "ablation waterfall -- each step adds one thing to the step above it",
        "",
        f"{'configuration':<14} {'comparisons':>16} {'vs prev':>9} {'vs base':>9} "
        f"{'median ns':>14} {'vs base':>9}",
        f"{'-' * 14} {'-' * 16} {'-' * 9} {'-' * 9} {'-' * 14} {'-' * 9}",
    ]

    for step in steps:
        if not step.measured:
            lines.append(f"{step.configuration:<14} {'not measured':>16}   {step.reason}")
            continue
        flags = "".join(
            [" [NOISY]" if step.noisy else "", " [UNPINNED]" if step.unpinned else ""]
        )
        lines.append(
            f"{step.configuration:<14} {step.comparisons:>16,} "
            f"{step.comparison_ratio:>8.3f}x {step.comparison_ratio_to_base:>8.3f}x "
            f"{step.median_ns:>14,.0f} {step.median_ratio_to_base:>8.3f}x{flags}"
        )

    lines += [
        "",
        "  comparisons are EXACT and reproduce to the unit.",
        "  median ns is a measurement; a row flagged NOISY had queries whose",
        "  within-query dispersion exceeded the runner's threshold, and the",
        "  ratio beside it is not a speedup claim. Use a paired test for that.",
        "",
        "  what each step adds:",
    ]
    for step in steps:
        lines.append(f"    {step.configuration:<14} {step.adds}")
    return "\n".join(lines)
