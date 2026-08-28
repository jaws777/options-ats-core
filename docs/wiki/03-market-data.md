# Market Data

Primary entry points:
- OPRA/OCC parsing: `src/market_data/opra.hpp`
- Quote book per series: `src/market_data/series_book.hpp`
- Simple limit book (reference): `src/market_data/order_book.hpp`

Operational flow:
1. Parse an exchange quote line into typed fields (`opra.hpp`).
2. Update NBBO snapshot in a lock-free latest-value store (often `infra::Seqlock`).
3. Update the per-series `SeriesBook` with the latest MM quotes.
4. Match incoming retail orders against MM quotes with NBBO compliance.

The `SeriesBook` is intentionally modeled as a flat array of MM quotes (<= 16).
At that scale, contiguous memory scans are typically faster than tree/heap data
structures due to cache locality.

