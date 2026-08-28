# Infrastructure Primitives

Primary entry points:
- SPSC queue: `src/infra/spsc_queue.hpp`
- LMAX-style disruptor: `src/infra/lmax_disruptor.hpp`
- Seqlock: `src/infra/seqlock.hpp`
- WAL: `src/infra/wal.hpp`
- Rate limiter: `src/infra/rate_limiter.hpp`

What these are for:
- decouple ingestion, matching, and publishing without locks
- support deterministic replay and deployment parity checks
- keep latency stable by avoiding allocation and contention in hot paths

Notes:
- These primitives have strict concurrency assumptions. Do not widen them to
  multi-producer or multi-writer use without redesign and new tests.

