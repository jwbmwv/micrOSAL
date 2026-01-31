#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Run clang-tidy on all project source files.
# Requires a compile_commands.json (pass BUILD_DIR or default to build/).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"

if [[ ! -f "$BUILD_DIR/compile_commands.json" ]]; then
    echo "Error: $BUILD_DIR/compile_commands.json not found."
    echo "Generate it with: cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
    exit 1
fi

FILES=()
while IFS= read -r -d '' f; do
    FILES+=("$f")
done < <(find "$REPO_ROOT/include" "$REPO_ROOT/src" \
    -type f \( -name '*.hpp' -o -name '*.cpp' -o -name '*.h' -o -name '*.c' \) \
    -print0 | sort -z)

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "No source files found."
    exit 0
fi

echo "clang-tidy: checking ${#FILES[@]} files …"
clang-tidy -p "$BUILD_DIR" "${FILES[@]}" "$@"
echo "Done."
