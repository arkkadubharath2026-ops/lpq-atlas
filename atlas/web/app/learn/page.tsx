import Link from "next/link";

const LAYERS: [string, string, string][] = [
  ["Graph", "cpp/include/lpq/graph/", "Directed weighted graphs loaded from DIMACS, SNAP, OpenStreetMap-derived and synthetic sources, with dataset checksums and query manifests."],
  ["CSR representation", "csr_graph.hpp", "Compressed sparse rows: one offsets array and one packed arc array, so a node's out-arcs are a contiguous span."],
  ["Dijkstra / A*", "algorithms/dijkstra.hpp, astar.hpp", "One templated implementation per algorithm, statically bound to its queue. A* adds g + h ordering with an admissible, consistent heuristic derived per graph."],
  ["Priority-queue abstraction", "pq/priority_queue.hpp", "Binary and 4-ary heaps and the radix heap use lazy deletion; pairing and Fibonacci heaps use decrease-key."],
  ["LPQ · bucket substrate", "lpq/bucket_substrate.hpp", "The authoritative store: 1024 key-range buckets, each keeping its best-scored entries at the front at push time."],
  ["LPQ · candidate manager", "lpq/candidate_manager.hpp", "Builds the candidate window from the front; the window size is bounded (default 2–32)."],
  ["LPQ · scorer", "learned_priority_queue.hpp (StaticNodeScorer)", "One float per node, precomputed from static features by a trained model before the search, or a constant in the control condition."],
  ["LPQ · verification", "verification/verification_engine.hpp", "Proves a candidate may be extracted by comparing it with a valid lower bound on everything not yet offered."],
  ["LPQ · correction", "verification/correction_engine.hpp", "Widens the window after a failed verification; if that fails too, falls back to the classical scan."],
  ["LPQ · adaptation", "adaptation/", "An optional controller that watches extraction outcomes and can retreat to the classical path; optionally an online policy learner."],
];

const CONCEPTS: [string, string][] = [
  ["Relaxation", "Examining arc u→v with weight w: if dist(u) + w beats the best known dist(v), the better value replaces it and v is (re)queued. In the trace this is EDGE_RELAXED."],
  ["Priority queue", "Holds tentative distances and returns the smallest. Which structure is used changes the cost of each operation, never the answer."],
  ["Dijkstra", "Repeatedly settles the queue's minimum. With non-negative weights a settled distance is final. It explores roughly in expanding circles of equal distance."],
  ["A*", "Orders by g + h, where h underestimates the remaining distance. The search leans toward the destination, so it usually settles fewer nodes than Dijkstra — compare the two on the same query in Compare mode."],
  ["Bucket substrate", "Keys are grouped into ranges. Extracting from a bucket classically means scanning it for the minimum; that scan is the cost the learned queue tries to avoid."],
  ["Learned scoring", "Each entry carries a score from its node. The substrate keeps high scorers at the front so the minimum is likely to be found by looking only there."],
  ["Exact verification", "Before extracting a candidate, the queue proves no smaller key exists anywhere. A wrong prediction therefore costs time, never correctness."],
  ["Approximation", "With ε > 0 the verifier may accept a candidate within (1+ε) of the bound. Every returned path is checked against the exact reference: ratio ≤ 1 + ε."],
  ["Fallback", "When verification cannot succeed within the window, the queue performs the classical scan. The fallback rate is one of the metrics that decides whether the learned queue pays for itself."],
];

export default function LearnPage() {
  return (
    <div className="page stack">
      <h1>Architecture &amp; concepts</h1>
      <p className="muted">
        This page explains the system in words. It contains no animated results: every visualisation in LPQ Atlas replays an event stream recorded
        from the real engine. To see these ideas in action, open the <Link href="/explore?dataset=osm_monaco">Route Explorer on Monaco</Link> and use
        <em> Step node</em>.
      </p>
      <div className="panel">
        <h2>Layers</h2>
        <ol className="stack" style={{ paddingLeft: 18, margin: 0 }}>
          {LAYERS.map(([name, file, text]) => (
            <li key={name}>
              <strong>{name}</strong> <span className="mono small dim">{file}</span>
              <div className="small muted">{text}</div>
            </li>
          ))}
        </ol>
      </div>
      <div className="grid cols-3">
        {CONCEPTS.map(([t, d]) => (
          <div key={t} className="panel"><h2>{t}</h2><p className="small muted" style={{ margin: 0 }}>{d}</p></div>
        ))}
      </div>
      <div className="panel">
        <h2>How a run reaches the screen</h2>
        <ol className="small muted" style={{ paddingLeft: 18 }}>
          <li>The UI posts a validated request; the API resolves the dataset and model from its registry — never from a client path.</li>
          <li>The API launches <code>lpq_run</code> (C++) with an argument list built from enums.</li>
          <li><code>lpq_run</code> runs the query untraced (the authoritative result), again with the trace recorder attached (the replay), then times the untraced search under the benchmark harness.</li>
          <li>It checks the traced run reproduced the untraced one and checks the answer against the reference Dijkstra, then writes JSON and JSONL.</li>
          <li>The UI streams the JSONL over server-sent events and replays it; every number in the telemetry panel is read from the JSON.</li>
        </ol>
      </div>
    </div>
  );
}
