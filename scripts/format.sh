#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Run clang-format on all project source files.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: ./scripts/format.sh [--check]

Options:
  --check   Validate formatting with clang-format --dry-run --Werror
  -h, --help  Show this help
EOF
}

MODE="format"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --check)
            MODE="check"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "ERROR: unknown option '$1'." >&2
            usage >&2
            exit 1
            ;;
    esac
done

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

SEARCH_ROOTS=()
for dir in include src tests examples; do
    if [[ -d "$REPO_ROOT/$dir" ]]; then
        SEARCH_ROOTS+=("$REPO_ROOT/$dir")
    fi
done

FILES=()
while IFS= read -r -d '' f; do
    FILES+=("$f")
done < <(find "${SEARCH_ROOTS[@]}" \
    -type f \( -name '*.hpp' -o -name '*.cpp' -o -name '*.h' -o -name '*.c' -o -name '*.inl' \) \
    ! -path '*/tests/nuttx/vendor/*' \
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

if [[ "$MODE" == "check" ]]; then
    echo "clang-format: checking ${#FILES[@]} files with $CF …"
    "$CF" --dry-run --Werror --style=file "${FILES[@]}"
else
    echo "clang-format: formatting ${#FILES[@]} files with $CF …"
    "$CF" -i --style=file "${FILES[@]}"
fi

echo "Done."
