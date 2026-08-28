# Third-Party Licenses

This project is distributed under Apache-2.0. Third-party components used in
build/test flows are listed below.

## Direct dependencies

| Component | Version | Usage | License |
| --- | --- | --- | --- |
| Catch2 | v3.7.1 | Unit/integration tests and benchmarks | BSL-1.0 |
| GoogleTest | v1.15.2 | Unit tests | BSD-3-Clause |

## Notes

- Runtime library is header-only and has no required runtime third-party
  dependency.
- Test dependencies are fetched via CMake `FetchContent` when test options are
  enabled.
- CI runs automated license scanning to detect unexpected transitive additions.
