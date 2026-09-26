import Link from "next/link";
export default function NotFound() {
  return (
    <div className="page">
      <div className="notice warn"><strong>Not found.</strong> <Link href="/">Back to LPQ Atlas</Link></div>
    </div>
  );
}
