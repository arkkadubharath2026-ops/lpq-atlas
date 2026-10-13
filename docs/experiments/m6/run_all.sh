#!/usr/bin/env bash
# M6 master run (scratchpad, not committed). Serial on purpose: nothing else may run during timing.
S=/tmp/claude-0/-home-user-lpq-atlas/0f1e2280-ae12-576b-942d-73b1cac79693/scratchpad
W=$S/m6; E=$S/m6_evidence; cd $W || exit 99
R=results/raw/m6; mkdir -p $R
REL=build/release/bin/lpq_benchmark; NI=build/release-noinstr/bin/lpq_benchmark
CORE=2
CMP="radix_heap;binary_heap;four_ary_heap;bucket_queue@bucket_classical;lpq;lpq_model;lpq_direct:front=0@lpq_direct_nonml;lpq_optimized;replay_floor"
stamp(){ echo "$(date -u +%FT%TZ) $*" >> $E/progress.txt; }
gate(){ # abort unless the tree is clean and at the candidate commit
  [ "$(git rev-parse HEAD)" = 450804238ca8786fb45bdce9f70991fc1765d55f ] && [ -z "$(git status --porcelain)" ] || { stamp "GATE FAILED $1"; exit 98; }; }
gate start
# ---- Block A / B: three invocations each ------------------------------------------------------
i=0
for seed in 20260923 20260924 20260925; do
  i=$((i+1))
  stamp "A inv$i start (dimacs_ny, seed $seed)"
  $REL --dataset datasets/processed/dimacs_ny.lpqgraph --queries-file experiments/manifests/dimacs_ny_queries_100.yaml \
     --algorithm dijkstra --repetitions 7 --warmup 3 --cpu-core $CORE --require-pinning --seed $seed --interleave \
     --model models/exported/dimacs_ny_static.lpqmodel --experiment-id m6-A-ny-inv$i --output $R/A_ny_inv$i.json \
     --compare "$CMP" > $E/A_ny_inv$i.log 2>&1; echo $? > $E/A_ny_inv$i.exit
  stamp "A inv$i end exit $(cat $E/A_ny_inv$i.exit)"
done
i=0
for seed in 20260923 20260924 20260925; do
  i=$((i+1))
  stamp "B inv$i start (snap_facebook, seed $seed)"
  $REL --dataset datasets/processed/snap_facebook.lpqgraph --queries-file experiments/manifests/snap_facebook_queries_first100.yaml \
     --algorithm dijkstra --repetitions 7 --warmup 3 --cpu-core $CORE --require-pinning --seed $seed --interleave \
     --model models/exported/snap_facebook_static.lpqmodel --experiment-id m6-B-fb-inv$i --output $R/B_fb_inv$i.json \
     --compare "$CMP" > $E/B_fb_inv$i.log 2>&1; echo $? > $E/B_fb_inv$i.exit
  stamp "B inv$i end exit $(cat $E/B_fb_inv$i.exit)"
done
# ---- Block C: counters overhead, binaries alternated REL NI REL NI -----------------------------
for pair in 1 2; do
  seed=$((20260930+pair))
  for kind in rel ni; do
    bin=$REL; [ $kind = ni ] && bin=$NI
    stamp "C pair$pair $kind start"
    $bin --dataset datasets/processed/dimacs_ny.lpqgraph --queries-file experiments/manifests/dimacs_ny_queries_100.yaml \
       --algorithm dijkstra --repetitions 7 --warmup 3 --cpu-core $CORE --require-pinning --seed $seed --interleave \
       --experiment-id m6-C-$kind-$pair --output $R/C_${kind}_$pair.json --compare "radix_heap;binary_heap;lpq" \
       > $E/C_${kind}_$pair.log 2>&1; echo $? > $E/C_${kind}_$pair.exit
    stamp "C pair$pair $kind end exit $(cat $E/C_${kind}_$pair.exit)"
  done
done
# ---- Block D: phase profile vs probes-compiled-out, two rounds -----------------------------------
for round in 1 2; do
  for spec in "lpq_profile_noinstr lpq" "lpq_profile lpq" "lpq_profile_noinstr binary_heap" "lpq_profile binary_heap"; do
    set -- $spec; tool=$1; q=$2
    stamp "D round$round $tool $q start"
    t0=$(date +%s.%N)
    build/release/bin/$tool --dataset datasets/processed/dimacs_ny.lpqgraph --queries-file experiments/manifests/dimacs_ny_queries_100.yaml \
       --pq $q --repetitions 3 --core $CORE > $E/D_${tool}_${q}_r$round.log 2>&1; code=$?
    t1=$(date +%s.%N)
    echo "$code $t0 $t1" > $E/D_${tool}_${q}_r$round.time
    stamp "D round$round $tool $q end exit $code"
  done
done
# ---- Block E: online, two invocations ------------------------------------------------------------
for mode in disabled periodic; do
  stamp "E $mode start"
  $REL --dataset datasets/processed/snap_facebook.lpqgraph --queries-file experiments/manifests/snap_facebook_queries_first100.yaml \
     --algorithm dijkstra --repetitions 7 --warmup 3 --cpu-core $CORE --require-pinning --seed 20260923 --interleave \
     --evaluation online_session --online-update-mode $mode \
     --model models/exported/snap_facebook_static.lpqmodel --online-model-report models/exported/snap_facebook_static.report.json \
     --experiment-id m6-E-$mode --output $R/E_fb_$mode.json --compare "radix_heap;lpq_model;lpq_online@online_$mode" \
     > $E/E_fb_$mode.log 2>&1; echo $? > $E/E_fb_$mode.exit
  stamp "E $mode end exit $(cat $E/E_fb_$mode.exit)"
done
gate end
stamp "ALL DONE"
