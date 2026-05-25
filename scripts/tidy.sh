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
TIDY_INCLUDE_REGEX="${TIDY_INCLUDE_REGEX:-}"
TIDY_JOBS="${TIDY_JOBS:-}"

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
        | awk -v repo_root="$REPO_ROOT/" -v build_root="$BUILD_DIR/" -v include_regex="$TIDY_INCLUDE_REGEX" '
            index($0, repo_root) == 1 && index($0, build_root) != 1 && $0 ~ /\.(cpp|c)$/ && (include_regex == "" || $0 ~ include_regex) {
                print
            }
        ' \
        | sort -u
)

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "No translation units from compile_commands.json matched the repository source tree."
    exit 0
fi

if [[ -z "$TIDY_JOBS" ]]; then
    if command -v nproc &>/dev/null; then
        TIDY_JOBS="$(nproc)"
    else
        TIDY_JOBS=1
    fi
fi

if ! [[ "$TIDY_JOBS" =~ ^[0-9]+$ ]] || [[ "$TIDY_JOBS" -eq 0 ]]; then
    echo "Error: TIDY_JOBS must be a positive integer." >&2
    exit 1
fi

if (( TIDY_JOBS > ${#FILES[@]} )); then
    TIDY_JOBS=${#FILES[@]}
fi

EXTRA_ARGS=("$@")
TOTAL_FILES=${#FILES[@]}
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

run_tidy_one()
{
    local index="$1"
    local file="$2"
    local log_file="$TMP_DIR/$index.log"
    local status_file="$TMP_DIR/$index.status"
    local status=0

    set +e
    {
        printf '[%d/%d] Processing file %s.\n' "$index" "$TOTAL_FILES" "$file"
        "$CT" -p "$BUILD_DIR" "$file" "${EXTRA_ARGS[@]}"
    } >"$log_file" 2>&1
    status=$?
    printf '%d\n' "$status" >"$status_file"
}

echo "clang-tidy: checking ${TOTAL_FILES} files with $CT (${TIDY_JOBS} job(s)) …"

active_jobs=0
for index in "${!FILES[@]}"; do
    run_tidy_one "$((index + 1))" "${FILES[index]}" &
    active_jobs=$((active_jobs + 1))
    if (( active_jobs >= TIDY_JOBS )); then
        wait -n || true
        active_jobs=$((active_jobs - 1))
    fi
done

while (( active_jobs > 0 )); do
    wait -n || true
    active_jobs=$((active_jobs - 1))
done

status=0
for ((index = 1; index <= TOTAL_FILES; ++index)); do
    cat "$TMP_DIR/$index.log"
    if [[ ! -f "$TMP_DIR/$index.status" ]] || [[ "$(<"$TMP_DIR/$index.status")" != "0" ]]; then
        status=1
    fi
done

if (( status != 0 )); then
    exit "$status"
fi

echo "Done."
