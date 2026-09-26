"use client";
import { useEffect, useState } from "react";
import { Markdown } from "@/components/Markdown";
import { ResultView } from "@/components/ResultView";
import { Empty, ErrorBox, Loading } from "@/components/States";
import { api } from "@/lib/api";
import type { Note, ResultListing, ResultSummary } from "@/lib/types";

export default function ResearchPage() {
  const [list, setList] = useState<ResultListing[] | null>(null);
  const [notes, setNotes] = useState<Note[] | null>(null);
  const [file, setFile] = useState<string | null>(null);
  const [baseline, setBaseline] = useState<string>("");
  const [summary, setSummary] = useState<ResultSummary | null>(null);
  const [note, setNote] = useState<string | null>(null);
  const [error, setError] = useState<unknown>(null);

  useEffect(() => {
    api.researchResults().then((l) => { setList(l); setFile(l.find((x) => !x.error)?.file ?? null); }).catch(setError);
    api.researchNotes().then((n) => { setNotes(n); setNote(n.find((x) => x.id.endsWith("findings"))?.id ?? n[0]?.id ?? null); }).catch(setError);
  }, []);

  useEffect(() => {
    if (!file) return;
    setSummary(null);
    api.researchResult(file, baseline || undefined).then(setSummary).catch(setError);
  }, [file, baseline]);

  const current = notes?.find((n) => n.id === note);
  return (
    <div className="page stack">
      <h1>Research dashboard</h1>
      <p className="small muted">
        Every chart below is computed from a stored <code>lpq_benchmark</code> result file in <code>results/raw/</code>; the file name identifies it.
        Negative results are shown exactly as measured. Model predictive quality (see Models) is a separate question from end-to-end speed.
      </p>
      {error ? <ErrorBox error={error} /> : null}
      <div className="grid" style={{ gridTemplateColumns: "300px minmax(0,1fr)" }}>
        <div className="panel stack">
          <h3>Stored experiments</h3>
          {!list ? <Loading /> : list.length === 0 ? <Empty title="No stored results">Run the reproduction scripts or a benchmark.</Empty> : (
            <div className="stack" role="listbox" aria-label="Result files">
              {list.map((r) => (
                <button key={r.file} type="button" role="option" aria-selected={file === r.file} disabled={Boolean(r.error)}
                  onClick={() => setFile(r.file)}
                  style={{ textAlign: "left", borderColor: file === r.file ? "var(--primary)" : undefined }}>
                  <div className="mono small">{r.file}</div>
                  <div className="small dim">{r.error ?? `${r.records} records · ${r.configurations.length} configs`}</div>
                </button>
              ))}
            </div>
          )}
        </div>
        <div className="stack">
          {summary ? (
            <>
              <div className="row">
                <label className="field" style={{ width: 280 }}>Baseline for paired comparison
                  <select value={baseline} onChange={(e) => setBaseline(e.target.value)}>
                    <option value="">binary_heap (default)</option>
                    {[...new Set(summary.configurations.filter((c) => c.supported).map((c) => c.pq))].map((q) => <option key={q} value={q}>{q}</option>)}
                  </select>
                </label>
              </div>
              <ResultView s={summary} />
            </>
          ) : file ? <Loading label="Summarising stored records…" /> : null}
        </div>
      </div>
      <div className="panel">
        <h2>Research notes</h2>
        <p className="small muted">The project&apos;s own write-ups (docs/experiments, experiments/paper), rendered as written.</p>
        {!notes ? <Loading /> : (
          <div className="grid" style={{ gridTemplateColumns: "260px minmax(0,1fr)" }}>
            <div className="stack">
              {notes.map((n) => (
                <button key={n.id} type="button" onClick={() => setNote(n.id)} style={{ textAlign: "left", borderColor: note === n.id ? "var(--primary)" : undefined }}>
                  <span className="small">{n.title}</span>
                </button>
              ))}
            </div>
            <div>{current ? <Markdown text={current.markdown} /> : <Empty title="Choose a note" />}</div>
          </div>
        )}
      </div>
    </div>
  );
}
