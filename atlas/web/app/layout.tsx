import type { Metadata } from "next";
import "leaflet/dist/leaflet.css";
import "./globals.css";
import { Nav } from "@/components/Nav";

export const metadata: Metadata = {
  title: "LPQ Atlas",
  description:
    "Dynamic Approximate Shortest Path Using a Learned Priority Queue — an observatory for classical and learned shortest-path search.",
};

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en">
      <body>
        <a href="#main" className="sr-only">
          Skip to content
        </a>
        <Nav />
        <div id="main">{children}</div>
      </body>
    </html>
  );
}
