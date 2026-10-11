// A small "?" that explains a term in plain language. The text is in
// aria-label (screen readers) and data-tip (the CSS tooltip, shown on hover
// and on keyboard focus).
export function Tip({ text }: { text: string }) {
  return <span className="tip" tabIndex={0} role="note" aria-label={text} data-tip={text}>?</span>;
}
