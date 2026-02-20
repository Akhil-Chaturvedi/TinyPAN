#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <build_dir>" >&2
  exit 2
fi

BUILD_DIR="$1"

SIZE_TOOL="${SIZE_TOOL:-size}"
TEXT_BUDGET="${TINYPAN_TEXT_BUDGET:-20000}"
BSS_BUDGET="${TINYPAN_BSS_BUDGET:-3000}"

OBJECTS=(
  "$BUILD_DIR/CMakeFiles/tinypan.dir/src/tinypan.c.o"
  "$BUILD_DIR/CMakeFiles/tinypan.dir/src/tinypan_bnep.c.o"
  "$BUILD_DIR/CMakeFiles/tinypan.dir/src/tinypan_supervisor.c.o"
)

for obj in "${OBJECTS[@]}"; do
  if [[ ! -f "$obj" ]]; then
    echo "Missing object file: $obj" >&2
    exit 2
  fi
done

size_output="$($SIZE_TOOL "${OBJECTS[@]}")"
printf '%s\n' "$size_output"

totals_line="$(printf '%s\n' "$size_output" | awk 'NR>1 {text+=$1; bss+=$3} END {printf "%d %d", text, bss}')"
read -r total_text total_bss <<<"$totals_line"

printf 'TOTAL text=%d bytes (budget=%d)\n' "$total_text" "$TEXT_BUDGET"
printf 'TOTAL bss=%d bytes (budget=%d)\n' "$total_bss" "$BSS_BUDGET"

if (( total_text > TEXT_BUDGET )); then
  echo "FAIL: text budget exceeded" >&2
  exit 1
fi

if (( total_bss > BSS_BUDGET )); then
  echo "FAIL: bss budget exceeded" >&2
  exit 1
fi

echo "PASS: size budgets satisfied"
