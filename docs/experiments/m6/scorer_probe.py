import subprocess, time, statistics, json, sys
W="/tmp/claude-0/-home-user-lpq-atlas/0f1e2280-ae12-576b-942d-73b1cac79693/scratchpad/m6"
E="/tmp/claude-0/-home-user-lpq-atlas/0f1e2280-ae12-576b-942d-73b1cac79693/scratchpad/m6_evidence"
base=[W+"/build/release/bin/lpq_benchmark","--dataset",W+"/datasets/processed/dimacs_ny.lpqgraph","--queries-file",E+"/trivial_q1.yaml",
      "--repetitions","3","--warmup","0","--cpu-core","2","--require-pinning","--seed","1","--no-validate"]
with_model=base+["--pq","lpq_model","--model",W+"/models/exported/dimacs_ny_static.lpqmodel"]
without=base+["--pq","lpq"]
def t(cmd):
    t0=time.perf_counter_ns(); p=subprocess.run(cmd,capture_output=True,text=True,cwd=W); dt=time.perf_counter_ns()-t0
    assert p.returncode==0,(p.stdout[-300:],p.stderr[-300:]); return dt
t(without); t(with_model)  # warm the page cache; discarded
rows=[]
for i in range(20):
    a=t(without); b=t(with_model); c=t(without)   # a,c: same command (null pair); b: with scorer build
    rows.append((a,b,c))
json.dump(rows,open(E+"/S_scorer_probe.json","w"))
d_model=[ (b-(a+c)/2)/1e6 for a,b,c in rows]; d_null=[(c-a)/1e6 for a,b,c in rows]
mad=lambda x: statistics.median(abs(v-statistics.median(x)) for v in x)
print("lpq (no model) process wall ms: median %.1f  MAD %.1f"%(statistics.median([a for a,_,_ in rows])/1e6, mad([a/1e6 for a,_,_ in rows])))
print("lpq_model - mean(lpq before, lpq after) ms: median %.1f MAD %.1f  min %.1f max %.1f"%(statistics.median(d_model),mad(d_model),min(d_model),max(d_model)))
print("null (lpq after - lpq before) ms: median %.1f MAD %.1f  min %.1f max %.1f"%(statistics.median(d_null),mad(d_null),min(d_null),max(d_null)))
