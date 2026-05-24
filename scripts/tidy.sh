#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Run clang-tidy on the translation units present in compile_commands.json.
# CI uses a Linux-hosted build database, so that remains the default analysis slice there.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
if [[ "$BUILD_DIR" != /* ]]; then
    BUILD_DIR="$REPO_ROOT/$BUILD_DIR"
fi
CT="${CLANG_TIDY:-clang-tidy}"

if [[ ! -f "$BUILD_DIR/compile_commands.json" ]]; then
    echo "Error: $BUILD_DIR/compile_commands.json not found."
    echo "Generate it with: cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
    exit 1
fi

if ! command -v "$CT" &>/dev/null; then
    echo "Error: $CT not found. Install clang-tidy or set CLANG_TIDY=<path>." >&2
    exit 1
fi

FILES=()
while IFS= read -r f; do
    FILES+=("$f")
done < <(
    sed -n 's/^[[:space:]]*"file": "\(.*\)",$/\1/p' "$BUILD_DIR/compile_commands.json" \
        | awk -v repo_root="$REPO_ROOT/" -v build_root="$BUILD_DIR/" '
            index($0, repo_root) == 1 && index($0, build_root) != 1 && $0 ~ /\.(cpp|c)$/ {
                print
            }
        ' \
        | sort -u
)

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "No translation units from compile_commands.json matched the repository source tree."
    exit 0
fi

echo "clang-tidy: checking ${#FILES[@]} files with $CT …"
"$CT" -p "$BUILD_DIR" "${FILES[@]}" "$@"
echo "Done."
