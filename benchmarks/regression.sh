#!/usr/bin/env bash
# tinyizer regression checker
# Runs tinyizer on all test pages and shows a per-page diff table
# against the stored baseline.  Use --update to refresh the baseline.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

BUILD_DIR=$(ls -dt "${PROJECT_DIR}"/build*/tinyizer 2>/dev/null | head -1 | xargs dirname)
if [ -z "$BUILD_DIR" ]; then
  echo "ERROR: no tinyizer build found" >&2
  exit 1
fi

BASELINE_FILE="${SCRIPT_DIR}/baseline.json"
TINYIZER="${BUILD_DIR}/tinyizer"

PAGES=(
  test_page.html
  test_page2.html
  test_page3.html
)

MODE="${1:-check}"

# Get current sizes
> "${SCRIPT_DIR}/.current.json"
for page in "${PAGES[@]}"; do
  bytes=$("${TINYIZER}" "${PROJECT_DIR}/${page}" | wc -c | tr -d ' ')
  echo "$page $bytes" >> "${SCRIPT_DIR}/.current.json"
done

if [ "$MODE" = "--update" ]; then
  cp "${SCRIPT_DIR}/.current.json" "$BASELINE_FILE"
  echo "baseline updated:"
  cat "$BASELINE_FILE" | while read page bytes; do
    echo "  $page: $bytes bytes"
  done
  rm -f "${SCRIPT_DIR}/.current.json"
  exit 0
fi

if [ ! -f "$BASELINE_FILE" ]; then
  echo "No baseline file. Run '$0 --update' first."
  rm -f "${SCRIPT_DIR}/.current.json"
  exit 1
fi

# Diff
total_change=0
regressions=0
improvements=0

printf "%-22s %8s %8s %8s\n" "Page" "Before" "After" "Delta"
printf "%-22s %8s %8s %8s\n" "----------------------" "--------" "--------" "--------"

for page in "${PAGES[@]}"; do
  after=$(grep "^$page " "${SCRIPT_DIR}/.current.json" | awk '{print $2}')
  before=$(grep "^$page " "$BASELINE_FILE" | awk '{print $2}')
  if [ -z "$before" ]; then
    printf "%-22s %8s %8s %8s\n" "$page" "???" "$after" "N/A (new)"
    continue
  fi
  delta=$((after - before))
  if [ $delta -gt 0 ]; then
    sign="+"
    regressions=$((regressions + 1))
  elif [ $delta -lt 0 ]; then
    sign=""
    improvements=$((improvements + 1))
  else
    sign=" "
  fi
  total_change=$((total_change + delta))
  printf "%-22s %8d %8d %s%8d\n" "$page" "$before" "$after" "$sign" "$delta"
done

rm -f "${SCRIPT_DIR}/.current.json"

echo ""
printf "%-22s %8s %8s %s%8d\n" "TOTAL" "" "" "" "$total_change"

if [ $regressions -gt 0 ]; then
  echo ""
  echo "WARNING: $regressions page(s) grew (regression)"
  exit 1
fi

if [ $total_change -lt 0 ]; then
  echo "All good: $improvements page(s) improved, ${total_change} bytes saved."
elif [ $total_change -eq 0 ]; then
  echo "No changes from baseline."
fi
