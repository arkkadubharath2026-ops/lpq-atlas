"""Python mirror of the C++ queue policy (``queue_policy.hpp``).

Everything that must agree with the engine lives here so that agreement can be
tested in one place:

* the ``.lpqprofile`` cache format and :func:`features` (``policy_features()``),
* :func:`pooled_ring_for` and the 2^18-slot feasibility rule,
* :meth:`QueuePolicy.choose` (``QueueSelectionPolicy::choose``),
* the ``lpqpolicy 1`` reader.

The feature list is checked against the C++ header by a test, not by trust.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from pathlib import Path

#: Must equal ``kPolicyFeatureNames`` in queue_policy.hpp (tested).
FEATURE_NAMES: tuple[str, ...] = (
    "log2_max_w",
    "log2_mean_w",
    "p50_over_max",
    "p99_over_max",
    "cv_w",
    "log2_nodes",
    "mean_degree",
)

#: ``QueueSelectionPolicy::kMaxPolicyRing``.
MAX_POLICY_RING: int = 1 << 18

_PROFILE_FLOAT_KEYS = (
    "min_weight",
    "max_weight",
    "mean_weight",
    "stddev_weight",
    "p50",
    "p90",
    "p99",
    "mean_degree",
)
_PROFILE_INT_KEYS = ("checksum", "nodes", "arcs", "quantiles_sampled", "compute_ns")


class PolicyFormatError(ValueError):
    """A profile or policy file is missing, malformed or inconsistent."""


@dataclass(frozen=True)
class WeightProfile:
    """The graph-level statistics the policy reads (``WeightProfile`` in C++)."""

    nodes: int
    arcs: int
    min_weight: float
    max_weight: float
    mean_weight: float
    stddev_weight: float
    p50: float
    p90: float
    p99: float
    mean_degree: float
    quantiles_sampled: bool = False
    checksum: int = 0
    compute_ns: int = 0


def parse_profile_text(text: str, source: str = "<text>") -> WeightProfile:
    """Parses the ``lpqprofile 1`` text format, as the C++ cache reader does.

    Like the C++ reader, an unknown key is an error (a foreign file must not be
    half-understood), and the version must be 1.
    """
    tokens = text.split()
    values: dict[str, str] = {}
    version = None
    i = 0
    while i < len(tokens):
        key = tokens[i]
        if i + 1 >= len(tokens):
            raise PolicyFormatError(f"{source}: key '{key}' has no value")
        val = tokens[i + 1]
        i += 2
        if key == "lpqprofile":
            version = val
        elif key in _PROFILE_FLOAT_KEYS or key in _PROFILE_INT_KEYS:
            values[key] = val
        else:
            raise PolicyFormatError(f"{source}: unknown profile key '{key}'")
    if version != "1":
        raise PolicyFormatError(f"{source}: unsupported or missing profile version {version!r}")
    missing = [k for k in (*_PROFILE_FLOAT_KEYS, "nodes", "arcs") if k not in values]
    if missing:
        raise PolicyFormatError(f"{source}: profile is missing {', '.join(missing)}")
    try:
        floats = {k: float(values[k]) for k in _PROFILE_FLOAT_KEYS}
        ints = {k: int(values.get(k, "0")) for k in _PROFILE_INT_KEYS}
    except ValueError as exc:
        raise PolicyFormatError(f"{source}: unparsable number ({exc})") from exc
    for k, v in floats.items():
        if not math.isfinite(v):
            raise PolicyFormatError(f"{source}: {k} is not finite")
    return WeightProfile(
        nodes=ints["nodes"],
        arcs=ints["arcs"],
        quantiles_sampled=ints["quantiles_sampled"] != 0,
        checksum=ints["checksum"],
        compute_ns=ints["compute_ns"],
        **floats,
    )


def load_profile(path: str | Path) -> WeightProfile:
    """Reads a ``.lpqprofile``; raises a clear error naming the file if absent."""
    p = Path(path)
    if not p.is_file():
        raise PolicyFormatError(
            f"graph profile not found: {p} (write it with the engine, e.g. by running "
            f"lpq_run --pq lpq_optimized on that dataset, or lpq_profile)"
        )
    return parse_profile_text(p.read_text(encoding="ascii"), str(p))


def format_profile(profile: WeightProfile) -> str:
    """Writes the same text the C++ cache writer writes (17 significant digits)."""

    def g(x: float) -> str:
        return format(x, ".17g")

    return (
        "lpqprofile 1\n"
        f"checksum {profile.checksum}\n"
        f"nodes {profile.nodes}\n"
        f"arcs {profile.arcs}\n"
        f"min_weight {g(profile.min_weight)}\n"
        f"max_weight {g(profile.max_weight)}\n"
        f"mean_weight {g(profile.mean_weight)}\n"
        f"stddev_weight {g(profile.stddev_weight)}\n"
        f"p50 {g(profile.p50)}\n"
        f"p90 {g(profile.p90)}\n"
        f"p99 {g(profile.p99)}\n"
        f"mean_degree {g(profile.mean_degree)}\n"
        f"quantiles_sampled {1 if profile.quantiles_sampled else 0}\n"
        f"compute_ns {profile.compute_ns}\n"
    )


def features(profile: WeightProfile) -> list[float]:
    """``policy_features()``: same formulas, same order, double precision.

    ``math.log2`` is used (libm, as ``std::log2``), not numpy's, so the values
    follow the same code path as the engine's.
    """
    mx = max(1.0, profile.max_weight)
    mean = max(1e-9, profile.mean_weight)
    return [
        math.log2(mx + 1.0),
        math.log2(mean + 1.0),
        profile.p50 / mx,
        profile.p99 / mx,
        profile.stddev_weight / mean,
        math.log2(float(max(1, profile.nodes))),
        profile.mean_degree,
    ]


def pooled_ring_for(max_step: int, width: int) -> int:
    """``pooled_ring_for``: ``std::bit_ceil(max_step / width + 3)`` (integer division)."""
    if width < 1:
        raise ValueError("bucket width must be >= 1")
    need = max_step // width + 3
    return 1 << (need - 1).bit_length()


def policy_max_step(profile: WeightProfile, algorithm: str) -> int:
    """The ``step`` the engine passes to ``choose``.

    Mirrors lpq_run / lpq_benchmark exactly: ``algorithm == "astar"`` doubles the
    maximum weight, any other algorithm string uses it as is, and the double is
    truncated to an integer like ``static_cast<std::uint64_t>``.
    """
    return int(2.0 * profile.max_weight if algorithm == "astar" else profile.max_weight)


def is_astar(algorithm: str) -> bool:
    """Dataset-table key rule: every algorithm whose name starts with 'astar'."""
    return algorithm.startswith("astar")


@dataclass(frozen=True)
class Candidate:
    width: int
    intercept: float
    coef: tuple[float, ...]

    def __post_init__(self) -> None:
        if self.width < 1:
            raise PolicyFormatError("candidate width must be >= 1")
        if len(self.coef) != len(FEATURE_NAMES):
            raise PolicyFormatError(f"candidate needs {len(FEATURE_NAMES)} coefficients")

    def log_ratio(self, x: list[float]) -> float:
        z = self.intercept
        for c, xi in zip(self.coef, x):
            z += c * xi
        return z


@dataclass(frozen=True)
class Choice:
    """``QueueChoice``: ``width == 0`` means the radix heap."""

    width: int
    ring: int
    predicted_ratio: float

    @property
    def is_radix(self) -> bool:
        return self.width == 0

    def describe(self) -> str:
        if self.is_radix:
            return "radix_heap"
        return f"pooled_bucket(width={self.width},ring={self.ring})"


def _exp(z: float) -> float:
    try:
        return math.exp(z)
    except OverflowError:
        return math.inf


def choose_from_log_ratios(
    log_ratios: list[tuple[int, float]], max_step: int, margin: float
) -> Choice:
    """The decision rule of ``QueueSelectionPolicy::choose`` on predicted log-ratios.

    ``log_ratios`` is the candidate list in policy-file order as ``(width, z)``.
    A candidate whose ring would exceed 2^18 slots is skipped; the best ratio
    must be strictly below ``1 - margin``; the first of equal ratios wins.
    """
    best = Choice(width=0, ring=0, predicted_ratio=1.0)
    best_ratio = 1.0 - margin
    for width, z in log_ratios:
        ring = pooled_ring_for(max_step, width)
        if ring > MAX_POLICY_RING:
            continue
        ratio = _exp(z)
        if ratio < best_ratio:
            best_ratio = ratio
            best = Choice(width=width, ring=ring, predicted_ratio=ratio)
    return best


@dataclass
class QueuePolicy:
    """In-memory ``QueueSelectionPolicy``."""

    margin: float = 0.03
    trained_on: str = ""
    candidates: list[Candidate] = field(default_factory=list)

    def choose(self, profile: WeightProfile, algorithm: str = "dijkstra") -> Choice:
        if not self.candidates:
            return Choice(0, 0, 1.0)
        x = features(profile)
        return choose_from_log_ratios(
            [(c.width, c.log_ratio(x)) for c in self.candidates],
            policy_max_step(profile, algorithm),
            self.margin,
        )


def parse_policy_text(text: str, source: str = "<text>") -> QueuePolicy:
    """Mirror of ``QueueSelectionPolicy::load``, including its strictness."""
    pol = QueuePolicy()
    header = False
    features_checked = False
    for raw in text.splitlines():
        if not raw or raw[0] == "#":
            continue
        parts = raw.split()
        if not parts:
            continue
        key = parts[0]
        if key == "lpqpolicy":
            if len(parts) < 2 or parts[1] != "1":
                raise PolicyFormatError(f"unsupported queue policy version in {source}")
            header = True
        elif key == "margin":
            pol.margin = float(parts[1])
        elif key == "trained_on":
            pol.trained_on = raw[len("trained_on") :]
        elif key == "features":
            if tuple(parts[1:]) != FEATURE_NAMES:
                raise PolicyFormatError(
                    f"queue policy {source} was trained on a different feature list"
                )
            features_checked = True
        elif key == "candidate":
            # candidate pooled w=<W> intercept <b> coef <7 numbers>
            if len(parts) < 6 or parts[1] != "pooled" or not parts[2].startswith("w="):
                raise PolicyFormatError(f"malformed candidate in {source}")
            if parts[3] != "intercept" or parts[5] != "coef":
                raise PolicyFormatError(f"malformed candidate in {source}")
            nums = parts[6:]
            if len(nums) < len(FEATURE_NAMES):
                raise PolicyFormatError(f"candidate has too few coefficients in {source}")
            pol.candidates.append(
                Candidate(
                    width=int(parts[2][2:]),
                    intercept=float(parts[4]),
                    coef=tuple(float(v) for v in nums[: len(FEATURE_NAMES)]),
                )
            )
        else:
            raise PolicyFormatError(f"unknown queue policy key '{key}' in {source}")
    if not header or not features_checked:
        raise PolicyFormatError(f"queue policy {source} is missing its header or features")
    return pol


def load_policy(path: str | Path) -> QueuePolicy:
    p = Path(path)
    if not p.is_file():
        raise PolicyFormatError(f"queue policy not found: {p}")
    return parse_policy_text(p.read_text(encoding="ascii"), str(p))
