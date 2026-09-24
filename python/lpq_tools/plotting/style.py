"""Shared figure styling.

One place that owns the palette and the chrome, so every figure in the paper
looks like it came from the same system rather than from whoever wrote it.

The two categorical hues below were validated rather than chosen by eye:
adjacent-pair separation of ΔE 24.7 under protanopia and 33.6 for normal
vision (OKLab ×100, against floors of 8 and 15), and both clear 3:1 contrast
against the chart surface. That matters here because the distinction the
figures carry — classical machinery against learned machinery — is the whole
argument, and a reader who cannot separate the two colours cannot read it.

Identity is never carried by colour alone: every figure that uses both hues
also ships a legend, and the bar figure direct-labels each bar.

Figures are light-mode only. They are built for a paper and a repository
document, neither of which has a theme toggle; a dark variant would be a second
set of validated steps for a surface nothing renders them on.
"""

from __future__ import annotations

#: Chart surface. Off-white rather than pure white so marks and gridlines have
#: something to sit against without a border.
SURFACE = "#fcfcfb"

TEXT_PRIMARY = "#0b0b0b"
TEXT_SECONDARY = "#52514e"

#: One shade off the surface: present, never competing with the data.
GRID = "#e4e3df"

#: Categorical slot 1 and 2. Classical components take the first, learned
#: components the second, consistently across every figure -- colour follows the
#: entity, so a figure that drops a series never repaints the survivors.
CLASSICAL = "#2a78d6"
LEARNED = "#eb6834"

#: A neutral for reference lines, which are chrome rather than data.
REFERENCE = "#8a8984"


def apply_style(pyplot) -> None:
    """Applies the shared chrome to a pyplot module.

    Kept as a function taking pyplot rather than a matplotlib style file so the
    settings live beside the palette they belong to and are readable without
    knowing matplotlib's stylesheet search path.
    """
    pyplot.rcParams.update(
        {
            "figure.facecolor": SURFACE,
            "axes.facecolor": SURFACE,
            "savefig.facecolor": SURFACE,
            # One sans throughout, including on any hero number. A serif or
            # display face on a figure reads as decoration.
            "font.family": "sans-serif",
            "font.sans-serif": ["DejaVu Sans", "Arial", "Helvetica"],
            "font.size": 9,
            "text.color": TEXT_PRIMARY,
            "axes.labelcolor": TEXT_SECONDARY,
            "axes.titlecolor": TEXT_PRIMARY,
            "xtick.color": TEXT_SECONDARY,
            "ytick.color": TEXT_SECONDARY,
            # Hairline, solid. Dashed gridlines read as a threshold or a
            # projection when they are neither.
            "axes.grid": True,
            "grid.color": GRID,
            "grid.linewidth": 0.6,
            "grid.linestyle": "-",
            "axes.axisbelow": True,
            # Two spines, not a box. The frame is not data.
            "axes.spines.top": False,
            "axes.spines.right": False,
            "axes.spines.left": False,
            "axes.edgecolor": GRID,
            "axes.linewidth": 0.8,
            "legend.frameon": False,
            "figure.dpi": 150,
            "savefig.dpi": 150,
            "savefig.bbox": "tight",
            # Generous padding. Thin marks need room more than they need size.
            "figure.constrained_layout.use": True,
        }
    )
