#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: ./scripts/generate_diagrams_print.sh [OPTIONS]

Generate PRINT-optimized PNG, SVG, and PDF files from all PlantUML (.puml) files.
Uses docs/diagrams/_render_profile_print.puml for rendering.

Options:
  -i, --input-dir DIR    Directory to scan for .puml files (default: docs/diagrams)
  -o, --output-dir DIR   Output directory for generated files (default: docs/diagrams/print)
  -h, --help             Show this help

Examples:
  ./scripts/generate_diagrams_print.sh
  ./scripts/generate_diagrams_print.sh --output-dir docs/diagrams/print
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
INPUT_DIR="$ROOT_DIR/docs/diagrams"
OUTPUT_DIR="$ROOT_DIR/docs/diagrams/print"
PRINT_PROFILE="$ROOT_DIR/docs/diagrams/_render_profile_print.puml"
SVG_TO_PDF="$ROOT_DIR/scripts/svg_to_pdf.py"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -i|--input-dir)
      [[ $# -ge 2 ]] || { echo "Error: $1 requires a value" >&2; usage; exit 1; }
      if [[ "$2" = /* ]]; then INPUT_DIR="$2"; else INPUT_DIR="$ROOT_DIR/$2"; fi
      shift 2
      ;;
    -o|--output-dir)
      [[ $# -ge 2 ]] || { echo "Error: $1 requires a value" >&2; usage; exit 1; }
      if [[ "$2" = /* ]]; then OUTPUT_DIR="$2"; else OUTPUT_DIR="$ROOT_DIR/$2"; fi
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

if [[ ! -f "$PRINT_PROFILE" ]]; then
  echo "Error: print profile not found: $PRINT_PROFILE" >&2
  exit 1
fi

if ! command -v plantuml >/dev/null 2>&1; then
  echo "Error: 'plantuml' is not installed or not on PATH." >&2
  echo "Install it (and Java) first, then re-run this script." >&2
  exit 1
fi

mkdir -p "$OUTPUT_DIR"

mapfile -t PUML_FILES < <(find "$INPUT_DIR" -type f -name '*.puml' ! -name '_*.puml' | sort)
if [[ ${#PUML_FILES[@]} -eq 0 ]]; then
  echo "No .puml files found under: $INPUT_DIR"
  exit 0
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

echo "Found ${#PUML_FILES[@]} .puml files under: $INPUT_DIR"
echo "Generating PRINT PNG, SVG, and PDF files into: $OUTPUT_DIR"

for file in "${PUML_FILES[@]}"; do
  base="$(basename "$file")"
  tmp_file="$TMP_DIR/$base"

  if grep -qE '^!include[[:space:]]+_render_profile\.puml' "$file"; then
    sed "s|^!include[[:space:]]\+_render_profile\.puml.*$|!include $PRINT_PROFILE|" "$file" > "$tmp_file"
  elif grep -qE '^!theme[[:space:]]+' "$file"; then
    awk -v profile="$PRINT_PROFILE" '
      BEGIN { inserted=0 }
      {
        print
        if (!inserted && $0 ~ /^!theme[[:space:]]+/) {
          print "!include " profile
          inserted=1
        }
      }
    ' "$file" > "$tmp_file"
  else
    awk -v profile="$PRINT_PROFILE" '
      BEGIN { inserted=0 }
      {
        print
        if (!inserted && $0 ~ /^@startuml/) {
          print "!include " profile
          inserted=1
        }
      }
    ' "$file" > "$tmp_file"
  fi

  echo "  - $base"
  plantuml -failfast2 --format png --output-dir "$OUTPUT_DIR" "$tmp_file"
  plantuml -failfast2 --format svg --output-dir "$OUTPUT_DIR" "$tmp_file"
  if command -v python3 >/dev/null 2>&1; then
    svg_file="$OUTPUT_DIR/${base%.puml}.svg"
    pdf_file="$OUTPUT_DIR/${base%.puml}.pdf"
    if ! python3 "$SVG_TO_PDF" "$svg_file" "$pdf_file" >/dev/null 2>&1; then
      echo "    warning: PDF conversion unavailable for $base" >&2
    fi
  else
    echo "    warning: python3 not found; PDF conversion skipped for $base" >&2
  fi
done

echo "Done. Print-optimized .png/.svg files are in: $OUTPUT_DIR; .pdf files are generated when conversion support is available."
