"""Request contracts (spec sections 20 and 36).

Every field that reaches the engine is an enum, a bounded integer or a bounded
float. Free-form strings (dataset and model ids, manifest names) are resolved
against the registry before use and never passed through as paths.
"""

from __future__ import annotations

from typing import Annotated, Literal

from pydantic import BaseModel, Field, model_validator

Algorithm = Literal["dijkstra", "astar"]
Heuristic = Literal["auto", "zero", "haversine", "euclidean"]
Queue = Literal[
    "binary_heap",
    "four_ary_heap",
    "pairing_heap",
    "fibonacci_heap",
    "radix_heap",
    "lpq",
    "lpq_model",
    "lpq_adaptive",
    "lpq_optimized",
]
Policy = Literal["exact", "approximate"]
TraceLevel = Literal["none", "summary", "visual", "full"]
_ID = r"^[a-z0-9_]{1,64}$"

LEARNED = {"lpq", "lpq_model", "lpq_adaptive"}
# The engine-configured, exact, ML-assisted queue. It is deliberately not in
# LEARNED: LEARNED gates the research knobs (approximate mode, window, bucket
# width), and this queue accepts none of them -- the engine chooses its own
# configuration, and it has no approximation mechanism.
OPTIMIZED = "lpq_optimized"


class EngineConfig(BaseModel):
    """What to run: shared by single runs, comparisons and benchmarks."""

    algorithm: Algorithm = "dijkstra"
    heuristic: Heuristic = "auto"
    queue: Queue = "binary_heap"
    correctness_policy: Policy = "exact"
    epsilon: float = Field(0.0, ge=0.0, le=1.0)
    model_id: str | None = Field(None, pattern=_ID)
    adaptive_learning: bool = False
    lpq_window: tuple[int, int] | None = None
    lpq_bucket_width: int | None = Field(None, ge=1, le=10_000_000)

    @model_validator(mode="after")
    def _combinations(self) -> EngineConfig:
        learned = self.queue in LEARNED
        if self.queue == OPTIMIZED:
            if (
                self.correctness_policy != "exact"
                or self.epsilon != 0.0
                or self.model_id
                or self.adaptive_learning
                or self.lpq_window is not None
                or self.lpq_bucket_width is not None
            ):
                raise ValueError(
                    "lpq_optimized is exact-only and chooses its own configuration: it takes no "
                    "model, approximate mode, adaptive learning, window or bucket width"
                )
            if self.algorithm == "dijkstra" and self.heuristic != "auto":
                raise ValueError("a heuristic only applies to A*")
            return self
        if self.algorithm == "dijkstra" and self.heuristic != "auto":
            raise ValueError("a heuristic only applies to A*")
        if self.queue == "lpq_model" and not self.model_id:
            raise ValueError("lpq_model requires model_id")
        if self.model_id and self.queue != "lpq_model":
            raise ValueError("model_id only applies to the lpq_model queue")
        if self.adaptive_learning and self.queue != "lpq_adaptive":
            raise ValueError("adaptive_learning only applies to the lpq_adaptive queue")
        if not learned and (self.lpq_window is not None or self.lpq_bucket_width is not None):
            raise ValueError("LPQ-specific options were given for a classical queue")
        if self.lpq_window is not None:
            lo, hi = self.lpq_window
            if not 0 < lo <= hi <= 4096:
                raise ValueError("lpq_window must satisfy 0 < lo <= hi <= 4096")
        if self.correctness_policy == "approximate":
            if not learned:
                raise ValueError(
                    f"{self.queue} has no approximation mechanism; approximate mode would "
                    "return exact results under an approximate label"
                )
            if not self.epsilon > 0.0:
                raise ValueError("approximate mode requires epsilon > 0")
        elif self.epsilon != 0.0:
            raise ValueError("epsilon only applies to approximate mode")
        return self


class RunRequest(EngineConfig):
    dataset_id: str = Field(pattern=_ID)
    source_node: int = Field(ge=0)
    target_node: int = Field(ge=0)
    trace_level: TraceLevel = "visual"
    max_events: int | None = Field(None, ge=1, le=5_000_000)
    repetitions: int = Field(5, ge=0, le=50)
    warmup: int = Field(1, ge=0, le=10)


class ComparisonSide(EngineConfig):
    label: str | None = Field(None, max_length=60)


class ComparisonRequest(BaseModel):
    dataset_id: str = Field(pattern=_ID)
    source_node: int = Field(ge=0)
    target_node: int = Field(ge=0)
    a: ComparisonSide
    b: ComparisonSide
    trace_level: TraceLevel = "visual"
    max_events: int | None = Field(None, ge=1, le=5_000_000)
    repetitions: int = Field(5, ge=1, le=50)
    warmup: int = Field(1, ge=0, le=10)


LaneId = Annotated[str, Field(pattern=_ID)]


class LaneSpec(BaseModel):
    """One lane of a fair comparison: an algorithm paired with a priority queue."""

    id: LaneId
    algorithm: Algorithm
    queue: Queue
    label: str | None = Field(None, max_length=80)

    @model_validator(mode="after")
    def _lane(self) -> LaneSpec:
        if self.queue == "lpq_model":
            raise ValueError("a comparison lane cannot use lpq_model: it needs a user-chosen model")
        return self


class LaneComparisonRequest(BaseModel):
    """Several (algorithm, queue) lanes timed on the SAME graph, source and target.

    Fairness is enforced here rather than left to the client: one repetition and
    warm-up count for every lane, tracing off, an exact policy, and a single
    heuristic shared by every A* lane.
    """

    dataset_id: str = Field(pattern=_ID)
    source_node: int = Field(ge=0)
    target_node: int = Field(ge=0)
    lanes: list[LaneSpec] = Field(min_length=2, max_length=8)
    heuristic: Heuristic = "auto"
    # Measured in `rounds` rounds; each round runs EVERY lane once (a fresh process, a fresh
    # shuffled order), each process doing `warmup` + `repetitions` searches. Separate
    # processes drift against one another on a shared machine, so one process's own spread
    # understates the real uncertainty; rounds expose it.
    rounds: int = Field(3, ge=1, le=7)
    repetitions: int = Field(5, ge=3, le=51)
    warmup: int = Field(2, ge=2, le=10)
    order_seed: int | None = Field(None, ge=0, le=2**63 - 1)

    @model_validator(mode="after")
    def _lanes(self) -> LaneComparisonRequest:
        ids = [lane.id for lane in self.lanes]
        if len(set(ids)) != len(ids):
            raise ValueError("lane ids must be unique")
        pairs = [(lane.algorithm, lane.queue) for lane in self.lanes]
        if len(set(pairs)) != len(pairs):
            raise ValueError("each (algorithm, queue) pair may appear in one lane only")
        if self.heuristic != "auto" and not any(lane.algorithm == "astar" for lane in self.lanes):
            raise ValueError("a heuristic only applies when some lane uses A*")
        return self


class BenchmarkRequest(BaseModel):
    dataset_id: str = Field(pattern=_ID)
    algorithm: Algorithm = "dijkstra"
    heuristic: Literal["euclidean", "haversine", "zero"] = "haversine"
    queues: list[Queue] = Field(min_length=1, max_length=8)
    correctness_policy: Policy = "exact"
    epsilon: float = Field(0.0, ge=0.0, le=1.0)
    model_id: str | None = Field(None, pattern=_ID)
    query_manifest: str | None = Field(None, pattern=r"^[a-z0-9_]{1,80}\.yaml$")
    query_count: int = Field(20, ge=1, le=1000)
    seed: int = Field(42, ge=0, le=2**63 - 1)
    repetitions: int = Field(5, ge=1, le=51)
    warmup: int = Field(1, ge=0, le=10)
    baseline: Queue | None = None

    @model_validator(mode="after")
    def _combinations(self) -> BenchmarkRequest:
        if len(set(self.queues)) != len(self.queues):
            raise ValueError("queues must not repeat")
        if ("lpq_model" in self.queues) != bool(self.model_id):
            raise ValueError("model_id is required with, and only with, the lpq_model queue")
        if self.correctness_policy == "approximate":
            if not self.epsilon > 0.0:
                raise ValueError("approximate mode requires epsilon > 0")
        elif self.epsilon != 0.0:
            raise ValueError("epsilon only applies to approximate mode")
        if self.baseline is not None and self.baseline not in self.queues:
            raise ValueError("baseline must be one of the selected queues")
        return self


class SnapRequest(BaseModel):
    lon: float = Field(ge=-180.0, le=180.0)
    lat: float = Field(ge=-90.0, le=90.0)
