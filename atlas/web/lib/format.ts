export const fmtInt = (v: number | null | undefined): string =>
  v === null || v === undefined ? "—" : Math.round(v).toLocaleString("en-US");

export const fmtNum = (v: number | null | undefined, digits = 3): string =>
  v === null || v === undefined || Number.isNaN(v)
    ? "—"
    : v.toLocaleString("en-US", { maximumFractionDigits: digits });

/** Nanoseconds to a readable duration. */
export function fmtNs(ns: number | null | undefined): string {
  if (ns === null || ns === undefined) return "—";
  if (ns < 1e3) return `${ns.toFixed(0)} ns`;
  if (ns < 1e6) return `${(ns / 1e3).toFixed(1)} µs`;
  if (ns < 1e9) return `${(ns / 1e6).toFixed(2)} ms`;
  return `${(ns / 1e9).toFixed(2)} s`;
}

export const fmtPct = (v: number | null | undefined, digits = 1): string =>
  v === null || v === undefined ? "—" : `${(v * 100).toFixed(digits)}%`;

/** A distance with its unit where the dataset states one. */
export function fmtDistance(d: number | null | undefined, unit?: string): string {
  if (d === null || d === undefined) return "unreachable";
  if (unit === "decimetre") return `${fmtInt(d)} dm (${(d / 10000).toFixed(3)} km)`;
  return `${fmtInt(d)}`;
}

export const queueLabel = (q: string): string =>
  ({
    binary_heap: "Binary heap",
    four_ary_heap: "4-ary heap",
    pairing_heap: "Pairing heap",
    fibonacci_heap: "Fibonacci heap",
    radix_heap: "Radix heap",
    lpq: "LPQ (control scorer)",
    lpq_model: "LPQ + static model",
    lpq_adaptive: "LPQ + adaptive",
  })[q] ?? q;

export const isLearned = (q: string): boolean => q.startsWith("lpq");
