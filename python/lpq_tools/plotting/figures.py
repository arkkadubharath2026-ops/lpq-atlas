"""The paper's figures (ESD.md sections 32 and 36).

Two figures, each answering one question the text asserts.

**The ablation waterfall** answers "where does the work go", using comparison
counts, which are exact. It is a bar chart rather than a true floating waterfall
because the quantity is a level at each rung, not a signed delta, and floating
bars would imply an additive decomposition the measurement does not support.

**The paired scatter** answers "is the model faster", per query, against the
line where the two are equal. A scatter is right because the claim is about the
*distribution* of paired outcomes, not about a summary: 74% of points on one
side is the finding, and a bar of two medians would hide it.

-----------------------------------------------------------------------------
Two rules these figures follow that are easy to get wrong
-----------------------------------------------------------------------------

**Never two y-scales.** Comparison counts are exact and timings are noisy
measurements; putting them on one pair of axes would invite exactly the
inference this project has measured three times to be invalid, that fewer
comparisons means less time. They get separate figures.

**Identity is never colour alone.** Every figure using both hues ships a legend,
and the bar figure labels each bar directly.
"""

from __future__ import annotations

from pathlib import Path

from lpq_tools.plotting.style import (
    CLASSICAL,
    GRID,
    LEARNED,
    REFERENCE,
    TEXT_PRIMARY,
    TEXT_SECONDARY,
    apply_style,
)

#: Which rungs of the ladder are classical and which carry learned machinery.
#: Colour follows the entity, so this mapping is fixed rather than positional.
LEARNED_CONFIGURATIONS = frozenset({"lpq", "lpq_model"})


def _rounded_bar(axis, y, width, height, color, radius_fraction=0.35):
    """Draws a horizontal bar with its far end rounded and its base square.

    The data end is rounded and the baseline end is not, so the mark reads as
    anchored to the axis rather than floating. matplotlib has no such bar, and a
    fully rounded rectangle would round the baseline too, detaching the bar from
    the zero it is measured from.
    """
    from matplotlib.path import Path as MplPath
    from matplotlib.patches import PathPatch

    radius = min(height * radius_fraction, width)
    if radius <= 0:
        axis.barh(y, width, height=height, color=color)
        return

    bottom, top = y - height / 2, y + height / 2
    end = width
    vertices = [
        (0, bottom),
        (end - radius, bottom),
        (end, bottom),
        (end, bottom + radius),
        (end, top - radius),
        (end, top),
        (end - radius, top),
        (0, top),
        (0, bottom),
    ]
    codes = [
        MplPath.MOVETO,
        MplPath.LINETO,
        MplPath.CURVE3,
        MplPath.CURVE3,
        MplPath.LINETO,
        MplPath.CURVE3,
        MplPath.CURVE3,
        MplPath.LINETO,
        MplPath.CLOSEPOLY,
    ]
    axis.add_patch(PathPatch(MplPath(vertices, codes), facecolor=color, edgecolor="none"))


def plot_ablation_waterfall(steps, destination: str | Path, *, dataset: str) -> Path:
    """Comparison counts per rung of the ablation ladder.

    Args:
        steps: WaterfallStep values, in ladder order.
        destination: where to write the figure.
        dataset: named in the title, because a figure separated from its caption
            that does not say which graph it describes is unusable.

    Raises:
        ValueError: if no rung was measured, where the figure would be empty.
    """
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    apply_style(plt)

    measured = [s for s in steps if s.measured]
    if not measured:
        raise ValueError("no rung of the ladder was measured, so there is nothing to plot")

    # Tall enough that four bars are thin marks with room around them rather
    # than a stack of heavy blocks.
    figure, axis = plt.subplots(figsize=(6.8, 3.4))

    # Top rung at the top: the ladder reads downward, as the text describes it.
    positions = list(range(len(measured)))[::-1]
    # A 2px surface gap between adjacent bars, achieved by leaving the bar
    # height below the category pitch rather than by drawing a border.
    height = 0.46

    largest = max(s.comparisons for s in measured)
    for position, step in zip(positions, measured):
        colour = LEARNED if step.configuration in LEARNED_CONFIGURATIONS else CLASSICAL
        _rounded_bar(axis, position, step.comparisons, height, colour)

        # Direct label outside the bar end. Inside would clip on the short bars,
        # and a clipped number is worse than an absent one.
        #
        # Value and ratio go on ONE line. Stacking the ratio underneath put it
        # into the x-axis tick band on the bottom row, where the two overlapped
        # and both became unreadable.
        label = f"{step.comparisons / 1e6:,.1f}M"
        if step.comparison_ratio and step is not measured[0]:
            label += f"   {step.comparison_ratio:.2f}x vs prev"
        axis.text(
            step.comparisons + largest * 0.015,
            position,
            label,
            va="center",
            ha="left",
            fontsize=8,
            color=TEXT_PRIMARY,
        )

    axis.set_yticks(positions)
    axis.set_yticklabels([s.configuration for s in measured], fontsize=9)
    axis.set_xlabel("queue comparisons (exact)")
    axis.set_xlim(0, largest * 1.42)
    axis.xaxis.set_major_formatter(lambda value, _: f"{value / 1e6:,.0f}M")
    axis.grid(axis="y", visible=False)

    axis.set_title(
        f"Bucketing wins, the learned machinery gives it back  ·  {dataset}",
        fontsize=10.5,
        loc="left",
        pad=26,
    )

    # A legend for two classes, because colour alone must not carry identity.
    from matplotlib.patches import Patch

    # Above the plot, not inside it. In the lower-right corner the legend box
    # sat on top of a bar's direct label.
    axis.legend(
        handles=[
            Patch(facecolor=CLASSICAL, label="classical"),
            Patch(facecolor=LEARNED, label="learned machinery"),
        ],
        loc="lower left",
        bbox_to_anchor=(0.0, 1.02),
        ncol=2,
        fontsize=8,
    )

    path = Path(destination)
    path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(path)
    plt.close(figure)
    return path


def plot_paired_scatter(
    baseline_ns,
    treatment_ns,
    destination: str | Path,
    *,
    baseline_name: str,
    treatment_name: str,
    dataset: str,
) -> Path:
    """Per-query medians of two configurations against the line of equality.

    Every point is one query measured under both configurations, which is what
    makes the comparison paired. The diagonal is where the two are equal, so the
    share of points below it IS the result and needs no annotation to be read.

    Raises:
        ValueError: if the two sequences differ in length, which would mean the
            points are not paired and the figure would be a fiction.
    """
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    apply_style(plt)

    if len(baseline_ns) != len(treatment_ns):
        raise ValueError(
            f"{len(baseline_ns)} baseline points against {len(treatment_ns)} treatment "
            "points: these are not paired, and a scatter of them would be a fiction"
        )
    if not baseline_ns:
        raise ValueError("no paired queries to plot")

    figure, axis = plt.subplots(figsize=(4.6, 4.4))

    low = min(min(baseline_ns), min(treatment_ns)) * 0.8
    high = max(max(baseline_ns), max(treatment_ns)) * 1.2

    # Equality first, so the data sits on top of the reference rather than under
    # it. Solid hairline: a dashed rule would read as a threshold.
    axis.plot([low, high], [low, high], color=REFERENCE, linewidth=1.0, zorder=1)

    # A 2px surface ring on each marker, which is how overlapping points stay
    # separable without drawing a border around every mark.
    axis.scatter(
        baseline_ns,
        treatment_ns,
        s=26,
        color=LEARNED,
        edgecolors="#fcfcfb",
        linewidths=1.0,
        alpha=0.85,
        zorder=2,
    )

    # Log-log: per-query difficulty on a road network spans orders of magnitude,
    # and on linear axes every easy query would collapse into the corner.
    axis.set_xscale("log")
    axis.set_yscale("log")
    # Minor ticks off. On a log axis matplotlib draws eight of them per decade,
    # which on a small figure reads as a fringe of noise along the axis and
    # collided with the orientation note.
    axis.minorticks_off()
    axis.set_xlim(low, high)
    axis.set_ylim(low, high)
    axis.set_aspect("equal")

    axis.set_xlabel(f"{baseline_name} median (ns, log)")
    axis.set_ylabel(f"{treatment_name} median (ns, log)")

    faster = sum(1 for b, t in zip(baseline_ns, treatment_ns) if t < b)
    share = faster / len(baseline_ns)

    axis.set_title(
        f"{treatment_name} faster on {share:.0%} of queries  ·  {dataset}",
        fontsize=10,
        loc="left",
        pad=10,
    )
    # One annotation naming which side of the line means what. Without it the
    # reader has to derive the orientation, and half of them will derive it
    # wrong.
    axis.text(
        0.04,
        0.96,
        f"below the line:\n{treatment_name} faster",
        transform=axis.transAxes,
        va="top",
        ha="left",
        fontsize=7.5,
        color=TEXT_SECONDARY,
    )

    path = Path(destination)
    path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(path)
    plt.close(figure)
    return path
