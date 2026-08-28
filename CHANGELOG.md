# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Public collaboration hardening artifacts:
  - `CODE_OF_CONDUCT.md`
  - `MAINTAINER_RESPONSE.md`
  - `CODEOWNERS`
  - issue and pull request templates
  - security/compliance CI checks and helper scripts
  - installable CMake package exports and a downstream consumer smoke test

### Changed

- `README.md`, `CONTRIBUTING.md`, and `SECURITY.md` for public governance,
  security reporting, and compliance clarity.
- Pinned GitHub Actions and FetchContent dependencies to immutable commits.
- Made release archives deterministic and added SHA-256 checksums.
- Removed host-specific release compiler flags from global CMake settings.

### Security

- Added dual secret scan policy (working tree and git history) and private-first
  remediation workflow documentation.
