// Loading / empty / error states. Every page renders one of these instead of a
// blank panel (spec section 35).
import { ApiError } from "@/lib/api";

export function Loading({ label = "Loading…", lines = 3 }: { label?: string; lines?: number }) {
  return (
    <div role="status" aria-live="polite" className="stack">
      <span className="small muted">{label}</span>
      {Array.from({ length: lines }, (_, i) => (
        <div key={i} className="skeleton" style={{ width: `${90 - i * 15}%` }} />
      ))}
    </div>
  );
}

export function Empty({ title, children }: { title: string; children?: React.ReactNode }) {
  return (
    <div className="notice info">
      <strong>{title}</strong>
      {children ? <div className="small muted" style={{ marginTop: 4 }}>{children}</div> : null}
    </div>
  );
}

export function ErrorBox({ error, title }: { error: unknown; title?: string }) {
  const message =
    error instanceof ApiError || error instanceof Error ? error.message : String(error);
  const unavailable = error instanceof ApiError && error.status === 0;
  return (
    <div className="notice bad" role="alert">
      <strong>{title ?? (unavailable ? "API unavailable" : "Something went wrong")}</strong>
      <div className="small" style={{ marginTop: 4 }}>
        {message}
      </div>
      {unavailable ? (
        <div className="small muted" style={{ marginTop: 4 }}>
          Start it with <code>python -m uvicorn atlas_api.app:app --app-dir atlas/api --port 8000</code>.
        </div>
      ) : null}
    </div>
  );
}

export function StateBadge({ state }: { state: string }) {
  const cls =
    state === "COMPLETED" || state === "READY"
      ? "ok"
      : state === "FAILED" || state === "CANCELLED"
        ? "bad"
        : state === "NOT_DOWNLOADED" || state === "NOT_CONVERTED"
          ? "warn"
          : "info";
  return <span className={`badge ${cls}`}>{state.replace(/_/g, " ")}</span>;
}
