## Block A: dimacs_ny, Dijkstra, exact, 100 queries
records 2700, exact_match true 2700, cpu_pinned values [True], (commit, dirty) [('450804238ca8786fb45bdce9f70991fc1765d55f', False)]
noisy-flagged records per invocation: [284, 300, 304] of [900, 900, 900]
anchor (radix_heap median of per-query medians) ms per invocation: ['18.69', '18.94', '20.51']; deviation from their median: ['1.3%', '0.0%', '8.3%']; rule (a) anomalous: [False, False, False]; rule (b) anomalous: [False, False, False]

### Absolute per-row figures (host-specific; median / MAD across the 100 queries of each query's median, ms)
| row | inv1 median (MAD) | inv2 median (MAD) | inv3 median (MAD) | comparisons (inv1) | noisy records (all inv) |
|---|---|---|---|---|---|
| radix_heap | 18.69 (13.25) | 18.94 (13.84) | 20.51 (14.16) | 71,839,857 | 90 |
| binary_heap | 26.89 (17.86) | 25.98 (19.92) | 25.11 (19.08) | 203,801,439 | 110 |
| four_ary_heap | 23.97 (18.16) | 25.59 (18.39) | 24.92 (17.22) | 221,402,601 | 83 |
| bucket_classical | 24.73 (15.17) | 26.03 (17.47) | 27.41 (17.18) | 116,552,995 | 86 |
| lpq | 64.42 (48.02) | 69.56 (53.16) | 72.98 (53.74) | 177,580,603 | 133 |
| lpq_model | 62.59 (48.12) | 69.99 (50.38) | 68.48 (50.80) | 177,732,303 | 133 |
| lpq_direct_nonml | 29.96 (17.22) | 29.01 (20.48) | 29.76 (21.02) | 116,238,499 | 99 |
| lpq_optimized | 19.85 (12.96) | 20.54 (14.19) | 19.50 (13.77) | 71,839,857 | 79 |
| replay_floor | 10.61 (7.25) | 11.00 (8.07) | 10.49 (7.80) | 0 | 75 |

### Paired ratios, row A / row B, per query (A slower when above 1)
Per invocation: median ratio [95% bootstrap CI over queries], fraction of queries where A is faster, Holm-adjusted permutation p within the invocation's family of 10 comparisons. Pooled: cluster bootstrap over queries across all invocations. 'established' needs the same sign in every invocation AND a pooled CI that excludes 1.
| A / B | inv1 | inv2 | inv3 | pooled median [95% CI] | A faster on (pooled) | verdict |
|---|---|---|---|---|---|---|
| binary_heap / radix_heap | 1.311 [1.251, 1.384] f=0.09 p_adj=0.001 | 1.339 [1.293, 1.378] f=0.04 p_adj=0.001 | 1.266 [1.224, 1.304] f=0.11 p_adj=0.001 | 1.302 [1.278, 1.346] | 0.08 | established slower |
| four_ary_heap / radix_heap | 1.261 [1.216, 1.285] f=0.10 p_adj=0.001 | 1.295 [1.247, 1.357] f=0.07 p_adj=0.001 | 1.234 [1.199, 1.297] f=0.12 p_adj=0.001 | 1.265 [1.231, 1.290] | 0.10 | established slower |
| bucket_classical / radix_heap | 1.178 [1.128, 1.230] f=0.24 p_adj=0.001 | 1.239 [1.209, 1.284] f=0.11 p_adj=0.001 | 1.211 [1.151, 1.251] f=0.21 p_adj=0.001 | 1.211 [1.183, 1.238] | 0.19 | established slower |
| lpq / radix_heap | 3.084 [2.963, 3.226] f=0.00 p_adj=0.001 | 3.243 [3.057, 3.451] f=0.00 p_adj=0.001 | 3.269 [3.073, 3.478] f=0.00 p_adj=0.001 | 3.203 [3.066, 3.290] | 0.00 | established slower |
| lpq_model / radix_heap | 3.148 [2.998, 3.286] f=0.00 p_adj=0.001 | 3.393 [3.197, 3.594] f=0.01 p_adj=0.001 | 3.047 [2.904, 3.254] f=0.00 p_adj=0.001 | 3.199 [3.066, 3.302] | 0.00 | established slower |
| lpq_direct_nonml / radix_heap | 1.405 [1.335, 1.477] f=0.04 p_adj=0.001 | 1.405 [1.325, 1.459] f=0.04 p_adj=0.001 | 1.396 [1.319, 1.453] f=0.07 p_adj=0.001 | 1.400 [1.353, 1.434] | 0.05 | established slower |
| lpq_optimized / radix_heap | 1.002 [0.970, 1.016] f=0.49 p_adj=1 | 1.015 [0.976, 1.047] f=0.46 p_adj=1 | 0.983 [0.959, 0.998] f=0.61 p_adj=0.342 | 0.998 [0.979, 1.010] | 0.52 | not distinguishable |
| replay_floor / radix_heap | 0.519 [0.482, 0.554] f=1.00 p_adj=0.001 | 0.547 [0.520, 0.565] f=1.00 p_adj=0.001 | 0.542 [0.518, 0.566] f=1.00 p_adj=0.001 | 0.537 [0.520, 0.553] | 1.00 | established faster |
| lpq_model / lpq | 0.978 [0.956, 1.012] f=0.55 p_adj=1 | 1.003 [0.958, 1.049] f=0.49 p_adj=1 | 0.978 [0.943, 1.005] f=0.59 p_adj=0.101 | 0.984 [0.967, 1.005] | 0.54 | not distinguishable |
| lpq_direct_nonml / lpq | 0.442 [0.419, 0.473] f=1.00 p_adj=0.001 | 0.437 [0.414, 0.468] f=1.00 p_adj=0.001 | 0.439 [0.422, 0.470] f=1.00 p_adj=0.001 | 0.439 [0.427, 0.465] | 1.00 | established faster |

### lpq_optimized resolution and one-time costs (per invocation)
inv1: resolved_queue=radix_heap, method=autotune, profile_source=computed_and_cached, profile_ns=13943834, calibration_ns=3950920184; reason: no pooled geometry beat the radix heap by the required margin (0.970000x on at least 60% of sampled queries): radix heap
inv2: resolved_queue=radix_heap, method=autotune_cached, profile_source=cached, profile_ns=13943834, calibration_ns=0; reason: no pooled geometry beat the radix heap by the required margin (0.970000x on at least 60% of sampled queries): radix heap
inv3: resolved_queue=radix_heap, method=autotune_cached, profile_source=cached, profile_ns=13943834, calibration_ns=0; reason: no pooled geometry beat the radix heap by the required margin (0.970000x on at least 60% of sampled queries): radix heap

fastest classical row by pooled median of per-query medians: radix_heap

## Block B: snap_facebook, Dijkstra, exact, 100 queries
records 2700, exact_match true 2700, cpu_pinned values [True], (commit, dirty) [('450804238ca8786fb45bdce9f70991fc1765d55f', False)]
noisy-flagged records per invocation: [285, 297, 308] of [900, 900, 900]
anchor (radix_heap median of per-query medians) ms per invocation: ['0.47', '0.47', '0.47']; deviation from their median: ['0.0%', '0.1%', '0.0%']; rule (a) anomalous: [False, False, False]; rule (b) anomalous: [False, False, False]

### Absolute per-row figures (host-specific; median / MAD across the 100 queries of each query's median, ms)
| row | inv1 median (MAD) | inv2 median (MAD) | inv3 median (MAD) | comparisons (inv1) | noisy records (all inv) |
|---|---|---|---|---|---|
| radix_heap | 0.47 (0.21) | 0.47 (0.18) | 0.47 (0.18) | 1,412,953 | 110 |
| binary_heap | 0.65 (0.24) | 0.66 (0.23) | 0.66 (0.21) | 5,326,649 | 109 |
| four_ary_heap | 0.63 (0.23) | 0.63 (0.23) | 0.61 (0.21) | 5,356,600 | 112 |
| bucket_classical | 1.58 (0.94) | 1.65 (1.03) | 1.67 (0.91) | 76,325,043 | 73 |
| lpq | 4.07 (2.32) | 4.19 (2.38) | 3.89 (2.11) | 82,595,639 | 77 |
| lpq_model | 4.49 (2.42) | 4.63 (2.70) | 3.99 (2.08) | 83,240,154 | 74 |
| lpq_direct_nonml | 1.10 (0.59) | 1.08 (0.54) | 0.97 (0.43) | 35,655,911 | 91 |
| lpq_optimized | 0.51 (0.22) | 0.49 (0.20) | 0.46 (0.20) | 1,412,953 | 128 |
| replay_floor | 0.41 (0.16) | 0.39 (0.14) | 0.36 (0.14) | 0 | 116 |

### Paired ratios, row A / row B, per query (A slower when above 1)
Per invocation: median ratio [95% bootstrap CI over queries], fraction of queries where A is faster, Holm-adjusted permutation p within the invocation's family of 10 comparisons. Pooled: cluster bootstrap over queries across all invocations. 'established' needs the same sign in every invocation AND a pooled CI that excludes 1.
| A / B | inv1 | inv2 | inv3 | pooled median [95% CI] | A faster on (pooled) | verdict |
|---|---|---|---|---|---|---|
| binary_heap / radix_heap | 1.435 [1.377, 1.474] f=0.08 p_adj=0.001 | 1.374 [1.341, 1.428] f=0.06 p_adj=0.001 | 1.369 [1.340, 1.445] f=0.04 p_adj=0.001 | 1.390 [1.358, 1.438] | 0.06 | established slower |
| four_ary_heap / radix_heap | 1.319 [1.292, 1.361] f=0.04 p_adj=0.001 | 1.291 [1.255, 1.327] f=0.04 p_adj=0.001 | 1.270 [1.247, 1.351] f=0.09 p_adj=0.001 | 1.303 [1.274, 1.333] | 0.06 | established slower |
| bucket_classical / radix_heap | 3.276 [3.020, 3.559] f=0.00 p_adj=0.001 | 3.301 [2.956, 3.771] f=0.01 p_adj=0.001 | 3.362 [2.979, 3.643] f=0.00 p_adj=0.001 | 3.303 [3.025, 3.635] | 0.00 | established slower |
| lpq / radix_heap | 8.691 [7.757, 9.308] f=0.00 p_adj=0.001 | 8.005 [7.253, 8.816] f=0.00 p_adj=0.001 | 7.769 [7.254, 8.580] f=0.00 p_adj=0.001 | 8.058 [7.495, 8.818] | 0.00 | established slower |
| lpq_model / radix_heap | 8.791 [7.927, 9.293] f=0.00 p_adj=0.001 | 9.253 [8.463, 9.618] f=0.00 p_adj=0.001 | 8.464 [7.867, 9.170] f=0.00 p_adj=0.001 | 8.723 [8.102, 9.338] | 0.00 | established slower |
| lpq_direct_nonml / radix_heap | 2.231 [2.054, 2.420] f=0.00 p_adj=0.001 | 2.186 [2.078, 2.334] f=0.01 p_adj=0.001 | 2.184 [2.042, 2.262] f=0.00 p_adj=0.001 | 2.196 [2.101, 2.288] | 0.00 | established slower |
| lpq_optimized / radix_heap | 1.008 [0.998, 1.021] f=0.43 p_adj=0.356 | 0.998 [0.983, 1.007] f=0.53 p_adj=0.581 | 0.995 [0.981, 1.005] f=0.53 p_adj=0.296 | 1.001 [0.993, 1.005] | 0.50 | not distinguishable |
| replay_floor / radix_heap | 0.811 [0.785, 0.839] f=0.83 p_adj=0.001 | 0.779 [0.761, 0.796] f=0.95 p_adj=0.001 | 0.779 [0.767, 0.794] f=0.84 p_adj=0.001 | 0.786 [0.776, 0.799] | 0.87 | established faster |
| lpq_model / lpq | 1.079 [1.051, 1.100] f=0.26 p_adj=0.356 | 1.108 [1.092, 1.127] f=0.10 p_adj=0.001 | 1.089 [1.068, 1.138] f=0.23 p_adj=0.0208 | 1.095 [1.079, 1.112] | 0.20 | established slower |
| lpq_direct_nonml / lpq | 0.268 [0.259, 0.283] f=1.00 p_adj=0.001 | 0.275 [0.267, 0.290] f=1.00 p_adj=0.001 | 0.278 [0.256, 0.294] f=1.00 p_adj=0.001 | 0.274 [0.264, 0.284] | 1.00 | established faster |

### lpq_optimized resolution and one-time costs (per invocation)
inv1: resolved_queue=radix_heap, method=autotune, profile_source=cached, profile_ns=4566888, calibration_ns=123240990; reason: no pooled geometry beat the radix heap by the required margin (0.970000x on at least 60% of sampled queries): radix heap
inv2: resolved_queue=radix_heap, method=autotune_cached, profile_source=cached, profile_ns=4566888, calibration_ns=0; reason: no pooled geometry beat the radix heap by the required margin (0.970000x on at least 60% of sampled queries): radix heap
inv3: resolved_queue=radix_heap, method=autotune_cached, profile_source=cached, profile_ns=4566888, calibration_ns=0; reason: no pooled geometry beat the radix heap by the required margin (0.970000x on at least 60% of sampled queries): radix heap

fastest classical row by pooled median of per-query medians: radix_heap

## Block C: cost of the pq-operation counters (release vs release-noinstr), dimacs_ny
instrumentation_enabled: release [True], release-noinstr [False]
| row | pair | counters-on / counters-off, median [95% CI] | queries where counters-on slower |
|---|---|---|---|
| radix_heap | 1 | 1.089 [1.034, 1.143] | 0.66 |
| radix_heap | 2 | 1.078 [1.040, 1.136] | 0.63 |
| radix_heap | pooled | 1.085 [1.050, 1.131] | 0.65 |
| binary_heap | 1 | 1.073 [1.041, 1.127] | 0.67 |
| binary_heap | 2 | 1.029 [0.982, 1.069] | 0.56 |
| binary_heap | pooled | 1.057 [1.033, 1.077] | 0.61 |
| lpq | 1 | 1.104 [1.067, 1.140] | 0.68 |
| lpq | 2 | 1.042 [0.990, 1.119] | 0.57 |
| lpq | pooled | 1.073 [1.046, 1.120] | 0.62 |
comparisons recorded: release 906,443,798, release-noinstr 0 (counters off = 0 by construction)

## Block D: phase profile (dimacs_ny, 3 passes over 100 queries)
| queue | round | uninstrumented wall (s) | with probes wall (s) | inflation | probe pair (ticks) |
|---|---|---|---|---|---|
| lpq | 1 | 30.826 | 36.255 | 1.176x | 52.7 |
| lpq | 2 | 27.261 | 34.398 | 1.262x | 63.7 |
| binary_heap | 1 | 10.188 | 12.932 | 1.269x | 45.8 |
| binary_heap | 2 | 10.487 | 12.660 | 1.207x | 46.3 |

Phase shares, LPQ (% of pop ticks), both rounds:
round 1: bucket traversal 6.34%, window maintenance 36.86%, verification scan 37.16%, fallback scan 0.02%, remove from bucket 6.93%, pop (unattributed) 12.69%; window+verification+fallback = 74.04% of pop; push share of push+pop = 13.48%
round 2: bucket traversal 5.76%, window maintenance 37.51%, verification scan 37.19%, fallback scan 0.01%, remove from bucket 6.83%, pop (unattributed) 12.70%; window+verification+fallback = 74.71% of pop; push share of push+pop = 13.29%

## Block E: lpq_online vs lpq_model on snap_facebook (100 queries, one invocation per mode)
mode disabled: exact_match all True; search time online / model per query: median 1.288 [1.258, 1.307], online faster on 0.10 of queries; median T_search model 3.86 ms, online 5.00 ms
   online_update_ns per query: median 0.0 us, MAD 0.0 us, max 0.1 us, sum 0.00 ms; queries whose weights hash changed: 0; total updates (last record): 0; timing.samples_ns has exactly 7 entries on every record (update cost not inside it): True
   labelled estimate, search + update per query (update counted once per query): 5.00 ms (median T_search 5.00 ms + median update 0.0000 ms)
mode periodic: exact_match all True; search time online / model per query: median 1.309 [1.288, 1.335], online faster on 0.05 of queries; median T_search model 3.70 ms, online 5.00 ms
   online_update_ns per query: median 803.2 us, MAD 176.5 us, max 1711.8 us, sum 67.72 ms; queries whose weights hash changed: 100; total updates (last record): 0; timing.samples_ns has exactly 7 entries on every record (update cost not inside it): True
   labelled estimate, search + update per query (update counted once per query): 5.81 ms (median T_search 5.00 ms + median update 0.8032 ms)

## Scorer-build estimate (process-level, coarse)
lpq_model minus mean of the surrounding lpq runs, ms: median 34.8, MAD 11.6, 95% bootstrap CI of the median [23.8, 43.6], positive in 17 of 20
null (lpq after minus lpq before), ms: median -1.3, MAD 20.3, CI of the median [-16.4, 14.7], range [-62.1, 67.8]

