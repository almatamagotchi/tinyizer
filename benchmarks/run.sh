#!/usr/bin/env bash
# tinyizer benchmark runner
# Runs tinyizer against test pages and optionally competitor tools.
# Outputs a JSON results file for the benchmark dashboard.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build2"
RESULTS_FILE="${SCRIPT_DIR}/results.json"

# Test pages
PAGES=(
  "${PROJECT_DIR}/test_page.html"
  "${PROJECT_DIR}/test_page2.html"
  "${PROJECT_DIR}/test_page3.html"
)

banner() { echo "=== $* ==="; }

run_tinyizer() {
  local input="$1"
  local basename
  basename=$(basename "$input")

  local stats
  stats=$("${BUILD_DIR}/tinyizer" "$input" --stats -o /dev/null 2>&1)

  local input_size output_size reduction time_ms
  input_size=$(echo "$stats" | grep "Input:" | awk '{print $2}')
  output_size=$(echo "$stats" | grep "Output:" | awk '{print $2}')
  time_ms=$(echo "$stats" | grep "Time:" | awk '{print $2}')

  if [ -n "$input_size" ] && [ "$input_size" -gt 0 ]; then
    reduction=$(echo "scale=2; (1 - $output_size / $input_size) * 100" | bc)
  else
    reduction="0"
  fi

  echo "  $basename: ${input_size} → ${output_size} bytes (${reduction}%, ${time_ms}ms)"
}

banner "tinyizer benchmark"
echo ""
for page in "${PAGES[@]}"; do
  run_tinyizer "$page"
done

echo ""
banner "done"
