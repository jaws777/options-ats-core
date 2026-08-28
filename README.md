# options-ats-core

[![CI](https://github.com/jaws777/options-ats-core/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/jaws777/options-ats-core/actions/workflows/ci.yml)
[![CodeQL](https://github.com/jaws777/options-ats-core/actions/workflows/codeql.yml/badge.svg?branch=main)](https://github.com/jaws777/options-ats-core/actions/workflows/codeql.yml)
[![Scorecard](https://github.com/jaws777/options-ats-core/actions/workflows/scorecard.yml/badge.svg?branch=main)](https://github.com/jaws777/options-ats-core/actions/workflows/scorecard.yml)

Low-latency C++20 building blocks for an options ATS: core numeric types,
market data handling, OMS state machines, and pre-trade risk controls.

Author: Matthew Jaworski (`jaws777`).

Keywords: C++20, low latency, lock-free, fixed-point, options, market data, OMS, FIX, pre-trade risk, deterministic replay.

## Overview

`options-ats-core` is a header-only C++20 library focused on correctness and
latency-sensitive paths for listed options systems. It provides:

- fixed-point pricing and checked arithmetic for deterministic numeric behavior
- lock-free infrastructure primitives for producer/consumer and replay flows
- market data parsers and books for options quote handling
- order/spread lifecycle state machines and FIX parsing
- pre-trade risk checks and execution-quality scoring primitives

This repository is software infrastructure, not trading advice.

## Security and public collaboration posture

- Security disclosure: see [SECURITY.md](SECURITY.md).
- Community behavior and reporting standards: see
  [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).
- Maintainer incident process (SLA, recusal, appeal): see
  [MAINTAINER_RESPONSE.md](MAINTAINER_RESPONSE.md).
- Contribution workflow: see [CONTRIBUTING.md](CONTRIBUTING.md).

## Build

Requires CMake 3.25+ and a C++20 compiler (Clang 17+, GCC 13+, or AppleClang
with equivalent C++20 support).

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Install and consume with CMake

Install the header-only package without fetching its test dependencies:

```bash
cmake -S . -B build-package \
  -DOPTIONS_ATS_CORE_BUILD_CATCH2=OFF \
  -DOPTIONS_ATS_CORE_BUILD_GTESTS=OFF \
  -DOPTIONS_ATS_CORE_BUILD_BENCHMARKS=OFF \
  -DOPTIONS_ATS_CORE_BUILD_EXAMPLES=OFF
cmake --install build-package --prefix /path/to/prefix
```

Downstream projects can then use the exported target:

```cmake
find_package(options_ats_core 0.1 REQUIRED CONFIG)
target_link_libraries(your_target PRIVATE options_ats_core::options_ats_core)
```

### Useful CMake options

| Option | Default | Description |
| --- | --- | --- |
| `OPTIONS_ATS_CORE_BUILD_CATCH2` | `ON` at top level | Build Catch2 tests |
| `OPTIONS_ATS_CORE_BUILD_GTESTS` | `ON` at top level | Build GoogleTest tests |
| `OPTIONS_ATS_CORE_BUILD_BENCHMARKS` | `ON` at top level | Build benchmark binaries |
| `OPTIONS_ATS_CORE_BUILD_EXAMPLES` | `ON` at top level | Build sample programs |

All four options default to `OFF` when the project is included with
`add_subdirectory`, so downstream builds do not fetch test dependencies.

## Implementation guide

See [docs/IMPLEMENTATION_GUIDE.md](docs/IMPLEMENTATION_GUIDE.md) for a structured walkthrough of the public methods, module responsibilities, and current cleanup decisions.

## Wiki

Repo-local wiki pages live under [docs/wiki/](docs/wiki/). These are written to be directly readable in the repo and easy to mirror into GitHub Wiki if enabled.

## Test layout

```text
tests/
├── unit/          # Catch2 unit tests
├── gtest/         # GoogleTest suite
├── integration/   # End-to-end order-flow tests
├── benchmarks/    # latency and throughput benches
└── package/       # installed-package consumer smoke test
```

## Examples

- `examples/lmax_disruptor_example.cpp`
- `examples/opra_consumer_example.cpp`

## Repository layout

```text
.
├── .github/
│   ├── CODEOWNERS
│   ├── ISSUE_TEMPLATE/
│   ├── PULL_REQUEST_TEMPLATE.md
│   └── workflows/ci.yml
├── src/
│   ├── core/
│   ├── infra/
│   ├── market_data/
│   ├── oms/
│   └── risk/
├── tests/
├── examples/
├── CHANGELOG.md
├── CODE_OF_CONDUCT.md
├── CONTRIBUTING.md
├── LICENSE
├── MAINTAINER_RESPONSE.md
├── NOTICE
├── README.md
├── SECURITY.md
└── THIRD_PARTY_LICENSES.md
```

## Release and compliance artifacts

- Change history: [CHANGELOG.md](CHANGELOG.md)
- Third-party license inventory: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)
- Release gates: [RELEASE_GATES.md](RELEASE_GATES.md)
- Security operations notes: [SECURITY_TRIAGE.md](SECURITY_TRIAGE.md)

## Scope and safety

This is an educational/reference core, not a complete production ATS. It does
not provide exchange certification, broker-dealer controls, regulatory
reporting, operational supervision, or deployment configuration. Integrators
remain responsible for independent security, correctness, legal, and
regulatory review before any trading use.

## License

Licensed under Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
