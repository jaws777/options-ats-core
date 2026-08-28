#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

is_tag_build=0
current_tag=""
if [[ "${GITHUB_REF:-}" == refs/tags/* ]]; then
  is_tag_build=1
  current_tag="${GITHUB_REF#refs/tags/}"
  baseline_tag="$(git describe --tags --abbrev=0 "${current_tag}^" 2>/dev/null || true)"
else
  baseline_tag="$(git describe --tags --abbrev=0 2>/dev/null || true)"
fi

if [[ -z "$baseline_tag" ]]; then
  echo "No previous tag found; skipping API diff check."
  exit 0
fi

# This repo is header-only, so implementation changes and documentation fixes
# often touch headers. We still want visibility into surface changes, but the
# *release gate* should be: tags must not change public headers relative to the
# prior tag without an intentional version bump/review.
#
# Policy:
# - On tag builds (releases): require the tag to match the CMake project
#   version, and require a version change when headers changed.
# - On non-tag builds (PRs/branch pushes): report diffs but do not fail.
#
# This keeps CI green for iterative development while enforcing invariants
# at the release boundary.
if [[ "$is_tag_build" -eq 0 ]]; then
  echo "Non-tag build; API diff is informational only (baseline tag: $baseline_tag)."
  informational_only=1
else
  informational_only=0
  project_version="$(sed -nE 's/^[[:space:]]*VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' CMakeLists.txt | head -1)"
  if [[ -z "$project_version" || "$current_tag" != "v$project_version" ]]; then
    echo "Release tag $current_tag must match CMake project version v${project_version:-unknown}."
    exit 1
  fi
fi

TMP_DIR="$(mktemp -d)"
cleanup() {
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

normalize_file() {
  local in="$1"
  local out="$2"
  sed -E 's://.*$::' "$in" | awk '
    BEGIN { block=0 }
    {
      line=$0
      while (1) {
        if (block == 1) {
          if (match(line, /\*\//)) {
            line=substr(line, RSTART+RLENGTH)
            block=0
          } else {
            line=""
            break
          }
        } else {
          if (match(line, /\/\*/)) {
            pre=substr(line,1,RSTART-1)
            rest=substr(line,RSTART+RLENGTH)
            if (match(rest, /\*\//)) {
              post=substr(rest,RSTART+RLENGTH)
              line=pre post
            } else {
              line=pre
              block=1
            }
          } else {
            break
          }
        }
      }
      gsub(/[[:space:]]+/, " ", line)
      gsub(/^ +| +$/, "", line)
      if (line != "") print line
    }
  ' > "$out"
}

changed=0
while IFS= read -r file; do
  old="$TMP_DIR/old"
  new="$TMP_DIR/new"
  git show "$baseline_tag:$file" > "$TMP_DIR/old_raw" 2>/dev/null || : > "$TMP_DIR/old_raw"
  if [[ -f "$file" ]]; then
    cp "$file" "$TMP_DIR/new_raw"
  else
    : > "$TMP_DIR/new_raw"
  fi

  normalize_file "$TMP_DIR/old_raw" "$old"
  normalize_file "$TMP_DIR/new_raw" "$new"

  if ! diff -u "$old" "$new" >/dev/null; then
    echo "Public API surface changed: $file"
    changed=1
  fi
done < <(
  {
    git ls-tree -r --name-only "$baseline_tag" -- src | grep -E '\.hpp$' || true
    find src -name '*.hpp'
  } | sort -u
)

if [[ "$changed" -ne 0 ]]; then
  echo "Non-comment API/header diff detected relative to tag $baseline_tag"
  if [[ "$informational_only" -eq 1 ]]; then
    exit 0
  fi

  baseline_version="$(
    git show "$baseline_tag:CMakeLists.txt" 2>/dev/null |
      sed -nE 's/^[[:space:]]*VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' |
      head -1
  )"
  if [[ -z "$baseline_version" || "$project_version" == "$baseline_version" ]]; then
    echo "Public headers changed without a project version change from $baseline_version."
    exit 1
  fi
  echo "Header changes are release-gated by version bump $baseline_version -> $project_version."
  exit 0
fi

echo "No non-comment API/header diff against $baseline_tag"
