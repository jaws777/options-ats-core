#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

missing=0

while IFS= read -r md; do
  while IFS= read -r link; do
    target="${link%%#*}"
    if [[ -z "$target" ]]; then
      continue
    fi
    if [[ "$target" =~ ^https?:// ]]; then
      continue
    fi
    if [[ "$target" =~ ^mailto: ]]; then
      continue
    fi

    resolved="$(python3 - <<'PY' "$md" "$target"
import os
import sys
md = sys.argv[1]
target = sys.argv[2]
base = os.path.dirname(os.path.abspath(md))
print(os.path.normpath(os.path.join(base, target)))
PY
)"
    if [[ ! -e "$resolved" ]]; then
      echo "Missing link target in $md: $link"
      missing=1
    fi
  done < <(grep -oE '\[[^]]+\]\(([^)]+)\)' "$md" | sed -E 's/^.*\(([^)]+)\)$/\1/')
done < <(find . -name '*.md' \
  -not -path './.git/*' \
  -not -path './build/*' \
  -not -path './build-*/*' \
  -print)

if [[ "$missing" -ne 0 ]]; then
  echo "Markdown link check failed"
  exit 1
fi

echo "Markdown link check passed"
