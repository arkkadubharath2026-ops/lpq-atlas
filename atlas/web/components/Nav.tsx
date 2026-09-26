"use client";
import Link from "next/link";
import { usePathname } from "next/navigation";

const LINKS = [
  ["/explore", "Route Explorer"],
  ["/compare", "Compare"],
  ["/benchmarks", "Benchmarks"],
  ["/research", "Research"],
  ["/models", "Models"],
  ["/datasets", "Datasets"],
  ["/learn", "Architecture / Learn"],
] as const;

export function Nav() {
  const path = usePathname();
  return (
    <header className="topbar">
      <Link href="/" className="brand" aria-label="LPQ Atlas home">
        LPQ <span>ATLAS</span>
      </Link>
      <nav className="nav" aria-label="Primary">
        {LINKS.map(([href, label]) => (
          <Link key={href} href={href} aria-current={path?.startsWith(href) ? "page" : undefined}>
            {label}
          </Link>
        ))}
      </nav>
      <div className="spacer" />
    </header>
  );
}
