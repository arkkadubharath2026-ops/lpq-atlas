"""Comparing the ladder across datasets (PRD.md section 25, ESD.md section 33).

A finding measured on one graph is a fact about that graph. This assembles the
same ladder across every dataset measured, so a claim can be checked for whether
it is a property of the DESIGN or a property of `dimacs_ny`.

That distinction is not hypothetical here. "The learned queue performs 12.9%
fewer comparisons than a binary heap" was reported for most of this project's
life as though it described the design; measured across four road networks it
holds on one of them and reverses on the others.

-----------------------------------------------------------------------------
Why bucket width is reported beside every row
-----------------------------------------------------------------------------

The substrate divides the weight range into a fixed number of buckets, so the
bucket width is the graph's maximum weight divided by that count. It is the
variable the learned machinery's cost tracks -- not node count, which two of
these datasets hold nearly constant while differing by an order of magnitude in
overhead. A table that omitted it would invite the reader to attribute the
spread to size, which is what happened to the author of this module.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from lpq_tools.evaluation.results import ResultSet, read_results

#: The fixed bucket count in BucketSubstrate. Mirrored here so a table can be
#: built from a results file alone, and asserted against the C++ constant by a
#: test rather than trusted.
BUCKET_COUNT = 1024


@dataclass(frozen=True)
class DatasetLadder:
    """One dataset's rung comparisons, plus what explains them."""

    dataset: str
    nodes: int
    max_weight: int

    binary_comparisons: int
    radix_comparisons: int
    lpq_comparisons: int

    binary_median_ns: float
    radix_median_ns: float
    lpq_median_ns: float

    mean_weight: float = 0.0

    @property
    def bucket_width(self) -> int:
        """What the runner derives, reproduced so the table is self-contained."""
        span = max(self.max_weight, 1)
        return max(1, (span + BUCKET_COUNT - 1) // BUCKET_COUNT)

    @property
    def width_over_mean_weight(self) -> float:
        """Bucket width measured against a TYPICAL edge, not the largest one.

        The variable that survived a discriminating test where bucket width
        alone failed twice. A bucket is wide or narrow only relative to the
        distance between consecutive keys, and that spacing is set by the
        typical edge weight. Sizing buckets from the maximum weight means a
        heavy-tailed graph gets buckets far wider than its own key spacing, and
        every fallback scan then walks a bucket holding many distinct keys.
        """
        return self.bucket_width / self.mean_weight if self.mean_weight else 0.0

    @property
    def bucketing_gain(self) -> float:
        """radix / binary. Below one means bucketing removed comparisons."""
        return self.radix_comparisons / self.binary_comparisons if self.binary_comparisons else 0.0

    @property
    def machinery_cost(self) -> float:
        """lpq / radix. The candidate window and verification, in isolation.

        The number that matters. Comparing the learned queue against a binary
        heap conflates this with the bucketing gain, and the two move in
        opposite directions.
        """
        return self.lpq_comparisons / self.radix_comparisons if self.radix_comparisons else 0.0

    @property
    def net_vs_binary(self) -> float:
        return self.lpq_comparisons / self.binary_comparisons if self.binary_comparisons else 0.0


#: Datasets whose ladder lives in a file named for another experiment. dimacs_ny
#: was measured by the ablation before the sweep existed, over the identical
#: query manifest and protocol, so re-running it would produce the same numbers
#: from a second file rather than new information.
RESULTS_FILENAME = {"dimacs_ny": "dimacs_ny_ablation_v1.json"}


def ladder_from_results(results: ResultSet, *, dataset: str, nodes: int,
                        max_weight: int, mean_weight: float = 0.0) -> DatasetLadder:
    """Extracts one dataset's ladder.

    Raises:
        ValueError: if a rung is missing, because a ladder with a hole in it
            would produce ratios against zero and print as though complete.
    """
    missing = [
        name
        for name in ("binary_heap", "radix_heap", "lpq")
        if not [r for r in results.for_configuration(name) if r.supported]
    ]
    if missing:
        raise ValueError(f"{dataset} is missing {', '.join(missing)}; the ladder is incomplete")

    def median(name: str) -> float:
        values = sorted(r.median_ns for r in results.for_configuration(name) if r.supported)
        middle = len(values) // 2
        if len(values) % 2:
            return values[middle]
        return 0.5 * (values[middle - 1] + values[middle])

    return DatasetLadder(
        dataset=dataset,
        nodes=nodes,
        max_weight=max_weight,
        mean_weight=mean_weight,
        binary_comparisons=results.total_comparisons("binary_heap"),
        radix_comparisons=results.total_comparisons("radix_heap"),
        lpq_comparisons=results.total_comparisons("lpq"),
        binary_median_ns=median("binary_heap"),
        radix_median_ns=median("radix_heap"),
        lpq_median_ns=median("lpq"),
    )


def format_generalisation(ladders: list[DatasetLadder]) -> str:
    """A table ordered by bucket width OVER MEAN WEIGHT, because that explains it.

    Ordering by plain bucket width put `snap_roadnet_ca` between `dimacs_ny` and
    `dimacs_col` and made the table look non-monotonic by three orders of
    magnitude. Ordering by the ratio makes it monotonic, which is the whole
    content of the finding, so the ordering is not cosmetic.
    """
    ordered = sorted(ladders, key=lambda x: x.width_over_mean_weight)

    lines = [
        "the ladder across datasets, ordered by BUCKET WIDTH / MEAN EDGE WEIGHT",
        "",
        f"{'dataset':<20} {'nodes':>10} {'max w':>9} {'mean w':>9} {'bkt w':>7} "
        f"{'bw/mean':>9} {'radix/bin':>10} {'lpq/radix':>12} {'lpq/bin':>10}",
        f"{'-' * 20} {'-' * 10} {'-' * 9} {'-' * 9} {'-' * 7} {'-' * 9} "
        f"{'-' * 10} {'-' * 12} {'-' * 10}",
    ]
    for rung in ordered:
        lines.append(
            f"{rung.dataset:<20} {rung.nodes:>10,} {rung.max_weight:>9,} "
            f"{rung.mean_weight:>9.2f} {rung.bucket_width:>7,} "
            f"{rung.width_over_mean_weight:>9.3f} {rung.bucketing_gain:>9.3f}x "
            f"{rung.machinery_cost:>11.3f}x {rung.net_vs_binary:>9.3f}x"
        )

    lines += [
        "",
        "  bw/mean     bucket width against a TYPICAL edge. The explanatory",
        "              variable: it predicted synth_ratio1_ny's 215.5x to within",
        "              1.28x, where bucket width alone was wrong by 216x.",
        "  radix/bin   what bucketing alone achieves. Never above 1.0x.",
        "  lpq/radix   what the candidate window and verification cost, isolated.",
        "  lpq/bin     the two combined -- which is what a pairwise comparison",
        "              against a binary heap reports, and why it misleads: the",
        "              two components move in opposite directions.",
        "",
        "  All comparison counts are EXACT. Ordering by the ratio rather than by",
        "  bucket width or node count is the finding: see the module docstring.",
    ]
    return "\n".join(lines)


def load_generalisation(
    results_dir: str | Path,
    metadata_dir: str | Path,
    datasets: list[str],
) -> list[DatasetLadder]:
    """Loads every dataset's ladder, skipping those not yet measured.

    A dataset with no results file is skipped rather than raising: the sweep is
    long and a partial table is useful, provided the caller can see which rows
    are absent. A dataset whose file exists but lacks a rung still raises,
    because that is a broken measurement rather than a missing one.
    """
    import json

    results_dir = Path(results_dir)
    metadata_dir = Path(metadata_dir)

    ladders = []
    for dataset in datasets:
        path = results_dir / RESULTS_FILENAME.get(
            dataset, f"{dataset}_generalisation_v1.json"
        )
        if not path.exists():
            continue
        meta = json.loads((metadata_dir / f"{dataset}.json").read_text(encoding="utf-8"))
        ladders.append(
            ladder_from_results(
                read_results(path),
                dataset=dataset,
                nodes=int(meta["node_count"]),
                max_weight=int(meta["max_weight"]),
                mean_weight=float(meta["mean_weight"]),
            )
        )
    return ladders
