# Live Order-Book Visualizer (Milestone 6)

A React/Next.js front end that renders the matching engine's order book in real
time. It consumes the **Server-Sent Events** feed published by the C++
`feed_server` and draws a depth ladder + a live ticker (last trade, trades/sec,
spread).

> **This is a presentation layer, not a feature of a real HFT system** (Section
> 10.3). Real venues are headless. The engineering point is the *isolation*
> (Section 10.4): snapshots are produced on a separate thread and the network
> never touches the matching hot path — if the browser is slow or disconnects,
> the engine keeps running at full speed.

## Run it

Two processes: the C++ backend (publishes the feed) and this front end.

```bash
# 1) backend — the engine + SSE feed (from the repo root)
make feed_server          # or: .\build.ps1 feed_server   (Windows)
./build/feed_server --port 8080

# 2) front end
cd web
npm install
npm run dev               # http://localhost:3000
```

The page connects to `http://127.0.0.1:8080/events` by default. Point it
elsewhere with an env var:

```bash
NEXT_PUBLIC_FEED_URL="http://127.0.0.1:9000/events" npm run dev
```

## Zero-dependency alternative

`feed_server` also serves a self-contained HTML version of this UI at
`http://127.0.0.1:8080/` — open that directly in a browser if you don't want to
run the Next.js toolchain. The Next.js app here is the polished, skills-reuse
version of the same thing.
