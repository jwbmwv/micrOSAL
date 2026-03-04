#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: ./generate_diagrams.sh [OPTIONS]

Generate PNG and SVG files from all PlantUML (.puml) files.

Options:
  -i, --input-dir DIR   Directory to scan for .puml files (default: docs/diagrams)
  -h, --help            Show this help

Examples:
  ./generate_diagrams.sh
  ./generate_diagrams.sh --input-dir docs/diagrams
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
INPUT_DIR="$ROOT_DIR/docs/diagrams"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -i|--input-dir)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value" >&2
        usage
        exit 1
      fi
      if [[ "$2" = /* ]]; then
        INPUT_DIR="$2"
      else
        INPUT_DIR="$ROOT_DIR/$2"
      fi
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Error: unknown option '$1'" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ ! -d "$INPUT_DIR" ]]; then
  echo "Error: input directory does not exist: $INPUT_DIR" >&2
  exit 1
fi

if ! command -v plantuml >/dev/null 2>&1; then
  echo "Error: 'plantuml' is not installed or not on PATH." >&2
  echo "Install it (and Java) first, then re-run this script." >&2
  exit 1
fi

mapfile -t PUML_FILES < <(find "$INPUT_DIR" -type f -name '*.puml' ! -name '_*.puml' | sort)

if [[ ${#PUML_FILES[@]} -eq 0 ]]; then
  echo "No .puml files found under: $INPUT_DIR"
  exit 0
fi

echo "Found ${#PUML_FILES[@]} .puml files under: $INPUT_DIR"
echo "Generating PNG and SVG files..."

for file in "${PUML_FILES[@]}"; do
  echo "  - $(basename "$file")"
  plantuml -failfast2 -tpng "$file"
  plantuml -failfast2 -tsvg "$file"
done

echo "Done. Generated .png and .svg files next to each .puml source."
