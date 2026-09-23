# Datasets: acquisition, conversion and query manifests

PRD.md §17 requires real SNAP graphs and OpenStreetMap-derived road networks,
and ESD.md §26 fixes the shape of the pipeline that produces them:

```text
download -> validate -> normalize -> convert to CSR -> compute metadata
         -> generate query manifest -> store checksum
```

This file documents how that pipeline is implemented, which component owns each
stage, and what the six currently prepared datasets actually contain. Every
figure in the table below was produced by running the pipeline; nothing here is
estimated.

## Where each stage lives, and why

Download, validate and the first half of normalize are Python
(`python/lpq_tools/data/`). Convert-to-CSR, metadata and the payload checksum
are the C++ tool `cpp/apps/lpq_convert.cpp`. Query-manifest generation is Python
again (`python/lpq_tools/data/queries.py`). `scripts/data/prepare_datasets.py`
drives the whole sequence through `lpq_tools.data.pipeline`.

The split is not a matter of taste. Two arguments put it exactly here.

The first is cost. Conversion has to touch every arc of a graph that may hold
tens of millions of them, and the checksum is a sequential hash over that whole
payload. Neither is work for Python. Acquisition and manifest generation are
I/O-bound orchestration with a few thousand pairs to emit, which is not work for
C++.

The second, and the deciding one, is that **the binary graph format has exactly
one implementation**. A second writer in Python would have to reproduce the
field order, the little-endian encoding, the canonical intra-adjacency order and
the FNV-1a checksum, and the two would drift. So Python decides *what* to
convert — which file, which normalization policy, which weight family and seed —
and `lpq_convert` decides *how*, and the format is defined in one place.

The one place Python does read the binary format is
`python/lpq_tools/data/binary_header.py`, and it reads only the fixed-size
header: node count, arc count, weight type, directedness and the stored
checksum. It never parses the arcs and never recomputes the checksum. It exists
so the pipeline can confirm that the file on disk is the one the metadata record
describes.

## Raw files are immutable

ESD.md §26's standing rule is *never modify raw datasets in place*.
`python/lpq_tools/data/acquire.py` enforces it rather than merely respecting it:

* A download lands in a temporary file in the destination directory and is
  renamed into `datasets/raw/` only after its digest has been checked. An
  interrupted transfer therefore cannot leave a short file that a later run
  mistakes for a complete one.
* The file is then made read-only. A later step that tries to decompress,
  rewrite or "fix" a raw file fails loudly instead of quietly editing the record
  of what was published. A filesystem that refuses the mode change is not
  treated as a failure; the guarantee weakens from enforced to conventional, and
  nothing downstream depends on it being enforced.
* Decompression writes to `datasets/processed/staging/`, never back into
  `datasets/raw/`.

TLS verification is never disabled. Python on Windows does not read the system
certificate store, which makes several university-hosted mirrors — including the
DIMACS challenge host — fail verification for reasons that have nothing to do
with the certificates being bad; the fix is certifi's CA bundle, not an
unverified context. For a pipeline whose entire purpose is knowing exactly which
bytes it received, accepting any server would be worse than not downloading.

Re-running is cheap and idempotent: a raw file already present with the right
digest is not fetched again, and the converter is re-run only when the recorded
command line differs from the one the current config produces. That last detail
matters — changing a normalization policy or a weight seed produces a different
graph from identical inputs, and a pipeline that only checked for an existing
output file would keep serving the old one.

## Checksums, and the difference between verified and trusted

Two different checksums are involved, for two different jobs.

**SHA-256 over downloaded files.** This answers "did I get the bytes I
expected?" and is checked at acquisition time.

**FNV-1a-64 over the arc payload of a converted graph.** This answers "is this
the same graph?" It is computed by `lpq_convert`, stored both in the binary
file's header and in the metadata JSON, and carried into the query manifest. The
pipeline cross-checks header against metadata after every conversion, because
the two are written by the same process and a disagreement means one of them was
replaced afterwards — precisely the case where trusting the metadata would
attribute one graph's results to another.

The SHA-256 pins deserve a plain statement, because the distinction is easy to
misread:

> **The `sha256` digests pinned in `configs/datasets/*.yaml` were observed by
> this pipeline on first acquisition. They are not checksums published by SNAP
> or by the DIMACS challenge.**

No published per-file digest was available at either download URL, and
inventing one was not an option. A plausible-looking hash for a file nobody has fetched makes
verification worse than useless, because it then fails identically for the
honest reason (the wrong file arrived) and the dishonest one (the expectation
was wrong), and the second failure is invisible. So `sources.py` allows a file
to be unpinned: it is downloaded, its observed digest is recorded in
`datasets/metadata/<dataset_id>.checksums.json`, and the run states in as many
words that the file was **accepted on trust**. Pinning it afterwards is a
one-line edit to the config.

What the current pins therefore buy is *change detection*, not provenance: a
later run that gets different bytes from the same URL fails. That is worth
having, and it is all that is being claimed. `PreparationResult.trusted_files`
propagates the distinction all the way to the run summary, because "verified"
and "accepted on trust" are different claims and the pipeline never conflates
them.

## `edges_listed`: a real bug, and the check that now prevents it

SNAP edge lists give no indication of whether each undirected edge is listed
once or in both directions, and **SNAP is not consistent about it**:

* `facebook_combined.txt` holds 88,234 records for the 88,234 edges SNAP
  publishes — each edge appears once.
* `roadNet-PA.txt` holds 3,083,796 records for the 1,541,898 edges SNAP
  publishes — each edge appears in both directions.

Reading the second file under the first file's assumption pairs every record a
second time and produces a graph with exactly twice the edges. Nothing about
that graph is malformed: it is a structurally valid undirected graph, it
converts, it loads, and shortest-path queries on it return answers. It is simply
a different graph, and every per-edge quantity measured on it — density, mean
degree, queue operations per edge, memory per arc — is wrong by a factor that
has nothing to do with the algorithm under test. This was found during
implementation, and it is the failure mode the whole validation stage exists for.

The fix is that the convention is *declared* in the config (`edges_listed: once`
or `both`) and then *checked against the file*. `validate.py` counts, over a
bounded prefix of at most 200,000 non-self-loop records, how many records repeat
an unordered endpoint pair already seen. A file listing both directions repeats
each pair exactly once, so the ratio lands near one half; a file listing each
edge once leaves it near zero. Ratios above 0.40 report `both`, below 0.01
report `once`, and anything in between reports *unknown* rather than rounding to
the nearer answer — a partially reciprocal file is a genuinely directed graph and
pairing it either way would be wrong.

Three details of that test are deliberate. The sample is bounded so that
validating a large road network does not need a set of tens of millions of
tuples, which is safe because the convention is a global property of the file
rather than a local one. It counts repeats of the *unordered pair* rather than
searching for each record's reverse: the reverse of an edge need not sit near it
in the file, so a "does its reverse appear nearby?" test would measure how well
sorted the file is as much as how it lists edges. And self-loops are excluded,
since a self-loop is its own reverse and would bias the ratio towards `both`.

When the detected convention contradicts the declared one, `pipeline.py` refuses
to convert and says by how much the edge count would have changed.

The validator also reports the whole shape of the file — node and edge counts,
weight presence and range, self-loops, and for DIMACS whether the `p sp` header
agrees with the records that follow. That summary is what catches the other
silent failure the converter cannot see: a download that succeeded, parsed and
converted perfectly but was the wrong graph. A DIMACS distance file and the
corresponding travel-time file have identical structure and different meanings,
and only the summary distinguishes them.

## Synthetic weights for the SNAP graphs

SNAP publishes all three of these graphs unweighted, so a travel cost has to
come from somewhere. `cpp/include/lpq/graph/weight_distributions.hpp`
synthesises one, from the family, bounds and seed the dataset config declares.

The weights are derived from a **hash of the unordered endpoint pair and the
seed**, not drawn from an RNG stream while walking the arcs. The obvious
stream-based implementation is wrong here for two independent reasons.

*Symmetry.* An undirected graph is stored as paired directed arcs, and `(u,v)`
and `(v,u)` are visited at different points in the walk. A stream would give
them different weights, and the graph would stop being undirected — quietly, and
in a way that changes shortest-path results rather than crashing.

*Order independence.* A stream ties each weight to its position in the traversal,
so reordering the input edge list — which a reconversion, a different reader or a
parallel-arc collapse can do — changes every weight downstream of the first
difference. A query manifest generated against one conversion would then be
describing a different graph.

Hashing the unordered pair fixes both by construction: `w(u,v) == w(v,u)`, and
the weight of an edge depends on nothing but the edge itself and the seed.

The cost is that **parallel arcs between the same pair receive identical
weights**. That is a documented consequence rather than an oversight:
distinguishing them would require an occurrence index, which is exactly the
order-dependent quantity this design exists to avoid.

Finally, the metadata record for every converted graph carries a `weights` block
with `synthesized: true` or `synthesized: false`. Where it is true it also
records the distribution, the bounds and the seed (plus `sigma`, `alpha` or
`narrow_fraction` where the family uses one); where it is false it records
`source: "dataset"`. Without that field a results table could attribute a
synthetic weight distribution to the source dataset, which is a claim this
project does not make. The DIMACS configs carry no `weights` block at all,
because those graphs ship real weights and synthesising over them would discard
the only real metric in the collection.

## Query manifests

PRD.md §18 requires that source-target pairs be generated deterministically from
fixed seeds and stored as experiment manifests, and in the same section forbids
cherry-picking favourable queries. Both are properties of
`python/lpq_tools/data/queries.py`.

The generator is **SplitMix64, written out in the module** rather than taken
from `random` or `numpy`. Both of those are reproducible in practice, but their
guarantees are about the underlying stream, not about how a particular helper
consumes it; `random.sample` and `Generator.choice` have changed their internal
consumption patterns across versions before. A manifest that silently
regenerated to different pairs after an interpreter upgrade would invalidate
every paired comparison in the results without failing anything. SplitMix64 is
twelve lines, is fully specified by those lines, and produces the same stream on
every Python that has integers.

Pairs are drawn uniformly over the node range, with **distinct endpoints** — a
query from a node to itself is answered without touching the priority queue, so
including such pairs would dilute the measurement with non-queries. The target
is mapped into "anything but the source" arithmetically rather than by a
rejection loop, so the number of draws per pair is fixed and the stream stays
aligned regardless of which sources come up. Uniformity within the range uses
Lemire's rejection rather than a plain modulo: the modulo bias is tiny for these
ranges, but it is direction-dependent, which means it would correlate with node
id, and node id correlates with position in a road network.

**Duplicate pairs are allowed.** Rejecting them would make the draw non-uniform,
and on a small graph with a large query count it might not terminate at all.

**Pairs are explicitly not filtered for reachability.** Filtering would require
running the very search the manifest exists to measure, and discarding the hard
pairs is exactly the cherry-picking PRD.md §18 forbids. A real graph has a giant
component and a scattering of small ones; an unreachable pair is a legitimate
query whose answer is "no path". The manifest states
`reachability_filtered: false` in its own body so a reader never has to infer
it.

Note that `lpq_benchmark` does not yet read these manifests: it still generates
its own queries from a seed, and loading a converted graph and its manifest is
outstanding work. The manifests are produced and checked in now because they
are an input to that step, not because anything consumes them today.

Each manifest carries the graph's FNV-1a-64 checksum, because **node ids are
dense indices assigned at conversion time**. The same numeric pair means a
different query after the graph is rebuilt from a different source file or under
a different normalization policy, so a manifest is only meaningful for one
conversion and says which one.

## The six prepared datasets

Measured during conversion and recorded in
`datasets/metadata/<dataset_id>.json`. "Observed" weight ranges are the minimum
and maximum weight actually present in the converted graph.

| dataset_id | source | nodes | edges | arcs | weights | coords |
|---|---|---|---|---|---|---|
| `snap_facebook` | SNAP ego-Facebook | 4,039 | 88,234 | 176,468 | synthetic lognormal, seed 1001, range [1, 10000], observed [2, 7911] | no |
| `snap_roadnet_pa` | SNAP roadNet-PA | 1,088,092 | 1,541,898 | 3,083,796 | synthetic uniform, seed 1002, range [1, 1000], observed [1, 1000] | no |
| `snap_roadnet_ca` | SNAP roadNet-CA | 1,965,206 | 2,766,607 | 5,533,214 | synthetic heavy_tailed, alpha 1.5, seed 1003, range [1, 100000], observed [1, 48438] | no |
| `dimacs_ny` | DIMACS USA-road-d.NY | 264,346 | 733,846 | 733,846 | dataset (distance), observed [1, 36946] | yes |
| `dimacs_col` | DIMACS USA-road-d.COL | 435,666 | 1,057,066 | 1,057,066 | dataset (distance), observed [1, 137384] | yes |
| `dimacs_fla` | DIMACS USA-road-d.FLA | 1,070,376 | 2,712,798 | 2,712,798 | dataset (distance), observed [1, 214013] | yes |

**Every node and edge count above matches the figures published by SNAP and by
the 9th DIMACS Implementation Challenge respectively.** This was checked against
the published figures rather than assumed.

The `arcs` column differs between the two collections for a structural reason,
not an inconsistency. The DIMACS graphs are stored **directed**, because the
source file lists both directions as separate arc records carrying their own
weights; pairing them again would be wrong, so `arcs = edges` there. The SNAP
graphs are stored **undirected**, materialised as paired directed arcs, so
`arcs = 2 x edges`. This is why `lpq_convert` defaults to directed for the
DIMACS and binary formats and undirected only for SNAP: reporting a DIMACS graph
as undirected would halve its edge count in the metadata.

The observed heavy-tailed maximum for `snap_roadnet_ca` (48,438) sits below the
configured bound of 100,000 because no sampled edge reached the clamp; the
Pareto family is unbounded in principle and clamped to the configured maximum in
practice.

The three SNAP graphs were also chosen to vary independently. `snap_facebook` is
dense and highly clustered — the structural opposite of a road network, and small
enough to check the pipeline end to end before spending bandwidth on the large
graphs. `snap_roadnet_pa` is road topology stripped of any real metric, which is
what makes comparing it against the similarly sized `dimacs_col` informative:
the difference isolates how much of a queue's behaviour comes from road-network
topology and how much from real travel costs. And `snap_roadnet_pa` on uniform
weights against `snap_roadnet_ca` on a heavy tail separates the effect of graph
*size* from the effect of weight *spread*, which a radix heap is sensitive to
through its bucket count.

## Why the DIMACS distance variant rather than travel time

The challenge publishes both a distance variant (`USA-road-d.*`) and a
travel-time variant (`USA-road-t.*`) of each region. The distance variant is
used here.

Distances are a pure function of geometry, so the coordinate-based A* heuristic
is admissible against them without any further assumption. A travel-time run
would need a maximum-speed scaling factor to keep the heuristic admissible, and
that factor is an *experiment parameter* — something chosen, tunable, and capable
of changing results — rather than a property of the dataset. Keeping it out means
the A* baseline on these graphs is a baseline and not a tuning knob.

The two variants are structurally identical and differ only in the weight
column, which is another reason the validator reports weight ranges: that
summary is the only thing distinguishing a correctly downloaded `-d` file from a
correctly downloaded `-t` file.

## Adding a dataset

A dataset is a YAML file in `configs/datasets/`, not a Python constant and not a
compiled-in table. Nothing about any particular graph appears in
`prepare_datasets.py`, in `pipeline.py`, or in `lpq_convert.cpp`. Adding one
means writing a new config with its URLs, source format, normalization policy,
optional weight-synthesis block and query seed, and running the pipeline. There
is no code change and no rebuild.

The larger DIMACS regions drop in exactly this way, up to the full USA graph at
roughly 24 million nodes. They have **deliberately not been downloaded yet**:
the six datasets above are what the current experiments need, and the larger
graphs are a bandwidth and disk commitment to make when a stress-test tier
actually calls for them, not before.

Two config-level guards are worth knowing about when adding one. Two configs
declaring the same `dataset_id` are rejected outright, because processed graphs
and metadata are keyed by it and one would silently overwrite the other. And a
weight-synthesis block with no `seed` is rejected rather than defaulted, because
an unseeded synthesis is not reproducible and a default would hide that the
config forgot to say which graph it meant.

## Usage

```bash
python scripts/data/prepare_datasets.py                  # every declared dataset
python scripts/data/prepare_datasets.py dimacs_ny        # one, or several, by id
python scripts/data/prepare_datasets.py --list           # what is declared, and how
python scripts/data/prepare_datasets.py --force          # re-download and re-convert
python scripts/data/prepare_datasets.py --skip-download  # convert what is already here
```

`--skip-download` never touches the network and fails if a declared raw file is
absent. A run that prepares some datasets and fails on others exits non-zero: an
experiment that silently ran on four of six datasets is worse than one that did
not run at all.

The converter is located by searching `build/release/bin`,
`build/relwithdebinfo/bin` and `build/debug/bin` for `lpq_convert` or
`lpq_convert.exe` — searched rather than configured, because the build directory
differs between presets and platforms and an absolute path in a config file
would be wrong on every machine but one. **The `LPQ_CONVERT` environment
variable overrides that discovery** with an explicit path, and a path that does
not exist is reported rather than skipped.

## A note for Windows

No PATH setup is needed to run `lpq_convert` from the Python pipeline. The MinGW
builds are statically linked (`-static-libgcc -static-libstdc++ -static`), so the
executable carries its runtime with it and can be invoked as a subprocess from
anywhere without a compiler installation on the path.

That linkage is not only a convenience. A dynamically linked libstdc++ can be
resolved to a different version on a different machine, which means a benchmark
would not necessarily be running the same standard-library code it was validated
against.
