import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "Mini-Exchange — Live Order Book",
  description:
    "Live depth visualization of a single-writer C++ matching engine, fed off the latency-critical path via Server-Sent Events.",
};

export default function RootLayout({
  children,
}: {
  children: React.ReactNode;
}) {
  return (
    <html lang="en">
      <body>{children}</body>
    </html>
  );
}
