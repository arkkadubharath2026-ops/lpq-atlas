# Does it generalise? The headline finding was a property of one graph

Every measurement in this project up to this point was made on `dimacs_ny`. This
repeats the ladder on five road networks, and the result is that **the claim
this project had been making since its first measurements does not hold.**

It also records a second failure: the explanation offered for *why* it did not
hold was itself tested against a new dataset, and **that explanation was wrong
too.** Both failures are kept here in the order they happened, because the
sequence is the evidence.

## The claim that failed

From the baseline measurements onward, this project reported:

> The learned queue performs **12.9% fewer comparisons** than a binary heap and
> is nevertheless 4.6× slower.

That was measured correctly. It is also a fact about `dimacs_ny` and not about
the design. Measured across five road networks, with the identical protocol and
100-query manifests generated the same way:

| dataset | nodes | max weight | bucket width | radix/binary | **lpq/radix** | lpq/binary |
|---|---:|---:|---:|---:|---:|---:|
| `snap_roadnet_pa` | 1,088,092 | 1,000 | **1** | 0.266× | **1.001×** | 0.266× |
| `dimacs_ny` | 264,346 | 36,946 | 37 | 0.352× | 2.472× | **0.871×** |
| `snap_roadnet_ca` | 1,965,206 | 48,438 | 48 | 0.069× | **4282.402×** | 294.884× |
| `dimacs_col` | 435,666 | 137,384 | 135 | 0.324× | 4.304× | **1.395×** |
| `dimacs_fla` | 1,070,376 | 214,013 | 209 | 0.331× | **11.141×** | 3.687× |

The `lpq/binary` column — the one the original claim reported — ranges from
0.266× to 294.884×. On `dimacs_ny` the learned queue does 12.9% fewer
comparisons; on `snap_roadnet_ca` it does roughly **295 times more**.

All counts are exact.

## The explanation that also failed

An earlier version of this document, written when only four of these datasets
had been measured, concluded that the explanatory variable was **bucket width**
(`max_weight / 1024`). Ordered by bucket width the four rows then available were
monotonic, and the mechanism was plausible: wider buckets hold entries with
genuinely different keys, so the candidate window covers less of a bucket,
verification fails more often, and each failure costs a fallback scan.

That document recorded a prediction **before** the fifth dataset finished, so
that the explanation could be falsified rather than fitted:

> `snap_roadnet_ca` (1,965,206 nodes, bucket width 48) is measuring as this is
> written. The bucket-width hypothesis predicts a machinery cost between
> `dimacs_ny`'s 2.472× and `dimacs_col`'s 4.304×.

**The measured value is 4282.402×.** The prediction was wrong by roughly three
orders of magnitude. Bucket width does not explain the machinery's cost: at
width 48, between `dimacs_ny`'s 37 and `dimacs_col`'s 135, the cost is not
between theirs. It is a thousand times larger than either.

The pre-registration did its job. Had the prediction been written after seeing
the number, the obvious move would have been to add a term and keep the theory.

## What the five datasets actually have in common

`snap_roadnet_ca` differs from every other dataset in its **weight
distribution**, not its size or its weight range:

| dataset | max weight | mean weight | sd weight | bucket width | **bucket width ÷ mean weight** | lpq/radix |
|---|---:|---:|---:|---:|---:|---:|
| `snap_roadnet_pa` | 1,000 | 500.20 | 288.42 | 1 | **0.002** | 1.001× |
| `dimacs_ny` | 36,946 | 1,293.30 | 1,130.08 | 37 | **0.029** | 2.472× |
| `dimacs_col` | 137,384 | 3,499.81 | 5,376.33 | 135 | **0.039** | 4.304× |
| `dimacs_fla` | 214,013 | 2,043.46 | 2,918.78 | 209 | **0.102** | 11.141× |
| `snap_roadnet_ca` | 48,438 | **2.95** | 36.31 | 48 | **16.281** | 4282.402× |

`snap_roadnet_ca` has a mean edge weight of **2.95** and a maximum of **48,438**
— a heavy tail, where almost every edge is tiny and a few are enormous. The
substrate sizes its buckets from the *maximum*, so its buckets are about 16
times wider than a *typical* edge. Every frontier node therefore lands in a
handful of buckets, each bucket holds an enormous number of entries with
distinct keys, and a fallback scan traverses all of them.

Ordered by that ratio, all five datasets are monotonic across four orders of
magnitude in the variable and four in the cost. The mechanism is the one the
earlier draft described; what it got wrong was the scale the bucket should be
compared against. A bucket is wide or narrow only *relative to the distance
between consecutive keys*, which is set by the typical edge weight, not by the
largest one.

This also explains the `radix/binary` column's one outlier: `snap_roadnet_ca` is
0.069× where every other dataset sits between 0.266× and 0.352×. The same
concentration of weights that ruins the learned machinery is ideal for a radix
heap.

## This explanation is fitted, and here is its test

**The ratio above was chosen after seeing the number it explains.** That makes
it a fit, not a prediction, and the previous section is what a fit looks like
when it fails. It is recorded as provisional until it survives a dataset chosen
to break it.

That dataset now exists. `synth_ratio1_ny` takes the `dimacs_ny` topology
unchanged (264,346 nodes, 733,846 arcs) and replaces its weights with a
heavy-tailed distribution (`--weights heavy_tailed --weight-alpha 2.0
--weight-min 1 --weight-max 100000 --weight-seed 4242`), giving mean weight
1.943, max weight 1,317, bucket width 2, and a **ratio of 1.029** — between
`dimacs_fla`'s 0.102 and `snap_roadnet_ca`'s 16.281, in the gap where the two
hypotheses disagree most.

The two hypotheses make incompatible predictions:

- **Bucket width** (already falsified, stated for contrast): bucket width 2 is
  the second-narrowest of any dataset here, just above `snap_roadnet_pa`'s 1, so
  the cost should be near **1.0×** — essentially free.
- **Bucket width ÷ mean weight**: a log-log fit through `dimacs_fla` and
  `snap_roadnet_ca` has slope 1.173 and predicts **167.8×**, with a
  factor-of-three band of **55.9× to 503.3×**.

These differ by more than two orders of magnitude, so the measurement
discriminates. Holding the topology fixed and changing only the weights also
removes graph structure as a confound, which no pair of the five natural
datasets does.

**Recorded before the run. The result appears in the next section.**

### Result

Measured `lpq/radix` on `synth_ratio1_ny`: **pending**.

## What does generalise

**Bucketing's advantage.** The `radix/binary` column sits between 0.266× and
0.352× on the four datasets with well-spread weights, and improves to 0.069× on
the concentrated one. Bucketing never loses. That is a property of the design.

**The direction of the machinery's cost.** It is never below 1.0×. The candidate
window and verification engine never *reduce* comparisons relative to the bucket
queue they are built on; at best, when a bucket is far narrower than a typical
edge weight, they are free.

## What this does to the project's conclusions

The structural finding survives and is strengthened. "Bucketing wins and the
learned machinery gives it back" was measured on one graph; it now holds on
five, with the give-back ranging from nothing to more than four thousandfold.

The specific number 12.9% should not be quoted again. Where earlier documents in
this directory use it, they are describing `dimacs_ny`.

The `lpq/binary` framing should not be used at all. It is the product of two
effects that move in opposite directions and vary independently, which is why a
single dataset made it look like a small favourable constant.

## What this does not show

- **Five road networks and no other graph class.** `snap_facebook` is a social
  graph and was not part of this sweep; its degree distribution is unlike a road
  network's.
- **The ratio is a five-point fit with one validation point.** It is monotonic
  and spans four orders of magnitude, but it is a summary of five natural
  datasets plus one constructed one, not a model derived from the substrate.
- **Bucket width was not varied within a natural dataset.** Each graph
  contributes one width. `synth_ratio1_ny` varies the weights on a fixed
  topology, which is the beginning of that sweep and not the whole of it.
- **No timings are compared across datasets.** The wall-clock columns differ by
  machine state across a multi-hour sweep; only the exact comparison counts are
  compared here. The approximate-mode wall-clock measurements quantify how large
  that drift is, and it is large.
- **The learned model was not involved.** Every row is the constant-scorer
  control. This measures the machinery, not prediction.

## Reproducing

```bash
for ds in dimacs_col dimacs_fla snap_roadnet_pa snap_roadnet_ca; do
  lpq_benchmark --dataset datasets/processed/$ds.lpqgraph \
    --queries-file experiments/manifests/${ds}_queries_100.yaml \
    --pq all --repetitions 11 --warmup 3 --require-pinning \
    --output results/raw/${ds}_generalisation_v1.json
done

python -c "from lpq_tools.evaluation.generalisation import *; \
print(format_generalisation(load_generalisation('results/raw','datasets/metadata', \
  ['dimacs_ny','dimacs_col','dimacs_fla','snap_roadnet_pa','snap_roadnet_ca'])))"
```

The validation dataset is built from `dimacs_ny` rather than downloaded:

```bash
lpq_convert --input datasets/processed/dimacs_ny.lpqgraph --format binary \
  --output datasets/processed/synth_ratio1_ny.lpqgraph \
  --metadata datasets/metadata/synth_ratio1_ny.json \
  --dataset-id synth_ratio1_ny \
  --weights heavy_tailed --weight-min 1 --weight-max 100000 \
  --weight-alpha 2.0 --weight-seed 4242
```
