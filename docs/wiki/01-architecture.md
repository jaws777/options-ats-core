# Architecture Overview

`options-ats-core` is a header-only C++20 library with correctness-first,
low-latency building blocks for an options ATS.

The repo is organized into the same modules a real ATS is typically split into:
- `core/`: numeric types and domain data structures
- `infra/`: lock-free primitives and durability/replay scaffolding
- `market_data/`: OPRA parsing and quote/book structures
- `oms/`: order parsing and lifecycle state machines
- `risk/`: pre-trade gating and execution-quality scoring

Design goals:
- determinism: fixed-point price storage and checked integer arithmetic
- explicit error handling: `core::expected` for fallible APIs
- low hidden cost: avoid surprise allocations in hot paths
- testability: unit + integration tests for each module

See the full method-by-method walkthrough in `docs/IMPLEMENTATION_GUIDE.md`.

