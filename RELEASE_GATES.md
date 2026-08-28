# Release Gates

This checklist defines required controls before a public release.

## Required CI checks

- `build-test`
- `asan`
- `docs-integrity`
- `secret-scan`
- `compliance`
- `api-diff`
- `package-consumer`

## Maintainer gates

- Signed release tag (`git tag -s`).
- Changelog updated for user-visible changes.
- Security policy links verified.
- Two-person sign-off for release candidate (maintainer + security reviewer).
- Release archive checksum generated and attached to the GitHub release.
- All third-party GitHub Actions and FetchContent inputs pinned to immutable
  commit SHAs; Dependabot review completed for available updates.

## Branch protection

Configure repository branch protection (GitHub settings) to require all checks
listed above before merge into the release branch.
