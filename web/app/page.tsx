"use client";

import { useEffect, useRef, useState } from "react";

// The C++ feed_server's SSE endpoint. Override with NEXT_PUBLIC_FEED_URL.
const FEED_URL =
  process.env.NEXT_PUBLIC_FEED_URL ?? "http://127.0.0.1:8080/events";

type Level = [number, number]; // [price, qty]

interface Snapshot {
  bids: Level[];
  asks: Level[];
  last: { price: number; qty: number };
  spread: number;
  tps: number;
  trades: number;
  processed: number;
}

export default function Page() {
  const [snap, setSnap] = useState<Snapshot | null>(null);
  const [live, setLive] = useState(false);
  const prevPrice = useRef<number | null>(null);
  const [dir, setDir] = useState<"up" | "down" | "">("");

  useEffect(() => {
    const es = new EventSource(FEED_URL);
    es.onopen = () => setLive(true);
    es.onerror = () => setLive(false);
    es.onmessage = (e) => {
      const s: Snapshot = JSON.parse(e.data);
      if (prevPrice.current != null && s.last.price !== prevPrice.current) {
        setDir(s.last.price > prevPrice.current ? "up" : "down");
      }
      prevPrice.current = s.last.price;
      setSnap(s);
    };
    return () => es.close();
  }, []);

  const maxQty = snap
    ? Math.max(1, ...snap.bids.map((b) => b[1]), ...snap.asks.map((a) => a[1]))
    : 1;

  const Bar = ({ side, lvl }: { side: "bid" | "ask"; lvl: Level }) => {
    const width = Math.max(2, (100 * lvl[1]) / maxQty) * 0.48;
    return (
      <div className={`row ${side}`}>
        <div className="bar" style={{ width: `${width}%` }} />
        <div className="px">{lvl[0]}</div>
        <div className="qty">{lvl[1]}</div>
      </div>
    );
  };

  return (
    <main className="wrap">
      <h1>Mini-Exchange — Live Order Book</h1>
      <div className="sub">
        single-writer C++ matching engine · depth snapshots pushed off the
        latency-critical path via Server-Sent Events
      </div>
      <div className={`status ${live ? "live" : "down"}`}>
        <span className="dot" />
        {live ? "connected" : `disconnected — start feed_server (${FEED_URL})`}
      </div>

      <div className="book">
        {snap ? (
          <>
            {[...snap.asks].reverse().map((a, i) => (
              <Bar key={`a${i}`} side="ask" lvl={a} />
            ))}
            <div className="mid">— spread {snap.spread} —</div>
            {snap.bids.map((b, i) => (
              <Bar key={`b${i}`} side="bid" lvl={b} />
            ))}
          </>
        ) : (
          <div style={{ color: "var(--muted)", padding: "20px 0" }}>
            waiting for the feed…
          </div>
        )}
      </div>

      {snap && (
        <div className="ticker">
          <div className="cell">
            <b className={dir}>
              {snap.last.qty} @ {snap.last.price}
            </b>
            <span>last trade</span>
          </div>
          <div className="cell">
            <b>{snap.spread}</b>
            <span>spread</span>
          </div>
          <div className="cell">
            <b>{snap.tps.toLocaleString()}</b>
            <span>trades / sec</span>
          </div>
          <div className="cell">
            <b>{snap.trades.toLocaleString()}</b>
            <span>total trades</span>
          </div>
          <div className="cell">
            <b>{snap.processed.toLocaleString()}</b>
            <span>orders processed</span>
          </div>
        </div>
      )}

      <div className="note">
        Bids (green) grow left, asks (red) grow right; bar width ∝ resting
        quantity. This UI is a <b>presentation layer only</b> — a real exchange
        is headless. The engineering point is the <em>isolation</em>: if this
        page is slow or disconnects, the matching thread keeps running at full
        speed because snapshots are produced on a separate thread and the network
        never touches the book. Start the backend with{" "}
        <code>./build/feed_server --port 8080</code>.
      </div>
    </main>
  );
}
