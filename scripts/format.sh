#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Run clang-format on all project source files.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

FILES=()
while IFS= read -r -d '' f; do
    FILES+=("$f")
done < <(find "$REPO_ROOT/include" "$REPO_ROOT/src" \
    -type f \( -name '*.hpp' -o -name '*.cpp' -o -name '*.h' -o -name '*.c' -o -name '*.inl' \) \
    -print0 | sort -z)

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "No source files found."
    exit 0
fi

CF="${CLANG_FORMAT:-clang-format-18}"
if ! command -v "$CF" &>/dev/null; then
    echo "ERROR: $CF not found. Install clang-format-18 or set CLANG_FORMAT=<path>." >&2
    exit 1
fi

echo "clang-format: formatting ${#FILES[@]} files with $CF …"
"$CF" -i --style=file "${FILES[@]}"
echo "Done."
