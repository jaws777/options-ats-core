# Contributing

Thanks for contributing to `options-ats-core`.

## Collaboration standards

All participation must follow [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

Expected behavior in issues/PRs:

- Keep feedback code-focused and specific.
- No harassment, insults, intimidation, or pile-on behavior.
- Assume good faith while still being technically rigorous.
- Be async-friendly across time zones; avoid pressure for immediate replies.

Security-sensitive details must go through private channels in
[SECURITY.md](SECURITY.md), not public comments.

## Local development

```bash
git clone https://github.com/<your-fork>/options-ats-core.git
cd options-ats-core

cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Sanitizer runs:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Asan
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Tsan
cmake --build build-tsan -j
```

## Code style and static analysis

- Run `clang-format -i` on touched `*.hpp` and `*.cpp` files.
- Use `clang-tidy -p build ...` on changed files when feasible.
- Keep changes focused; avoid mixing behavior changes with broad reformatting.

## Commit messages

Use Conventional Commits:

```text
<type>(<scope>): <short summary>
```

Common types: `feat`, `fix`, `perf`, `refactor`, `test`, `docs`, `build`,
`ci`, `chore`.

## DCO sign-off

All commits must include DCO sign-off via `git commit -s`.

## Pull requests

- Branch from `main` and open a PR to `main`.
- Add/update tests for behavior changes.
- Update [CHANGELOG.md](CHANGELOG.md) when user-visible behavior changes.
- Ensure CI is green.

## Reporting vulnerabilities

Use the private channels in [SECURITY.md](SECURITY.md).
