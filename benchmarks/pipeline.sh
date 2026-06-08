#!/usr/bin/env bash
# Unified competitor pipeline: extracts CSS/JS from HTML, runs best-in-class
# minifiers on each, and reassembles. Produces full-page byte counts for the
# Lightning CSS + Google Closure Compiler + html-minifier-terser combo.
#
# Dependencies (must be on PATH):
#   lightningcss-cli    (npm)
#   google-closure-compiler  (Java JAR at /tmp/closure-compiler.jar)
#   html-minifier-terser (npm)
#
# Usage: ./pipeline.sh <input.html>

set -euo pipefail

INPUT="$1"
if [ ! -f "$INPUT" ]; then
    echo "ERROR: $INPUT not found" >&2
    exit 1
fi

JAVA="${JAVA:-java}"
CC_JAR="${CC_JAR:-/tmp/closure-compiler.jar}"

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# -----------------------------------------------------------------------
# 1. Extract <style> blocks, concatenate them
# -----------------------------------------------------------------------
CSS_BLOCK="$TMPDIR/css_raw.css"
> "$CSS_BLOCK"
perl -0777 -ne 'while(/<style[^>]*>(.*?)<\/style>/gs){print "$1\n"}' "$INPUT" > "$CSS_BLOCK"

# 2. Extract <script> blocks (inline only, no src attrs)
# -----------------------------------------------------------------------
JS_BLOCK="$TMPDIR/js_raw.js"
> "$JS_BLOCK"
perl -0777 -ne 'while(/<script[^>]*>(.*?)<\/script>/gs){print "$1\n"}' "$INPUT" > "$JS_BLOCK"

# 3. Minify CSS with Lightning CSS
# -----------------------------------------------------------------------
CSS_MIN="$TMPDIR/css_min.css"
css_orig=0 css_min=0
if [ -s "$CSS_BLOCK" ]; then
    css_orig=$(wc -c < "$CSS_BLOCK")
    lightningcss --minify "$CSS_BLOCK" > "$CSS_MIN" 2>/dev/null
    css_min=$(wc -c < "$CSS_MIN")
fi

# 4. Minify JS with Closure Compiler (ADVANCED)
# -----------------------------------------------------------------------
JS_MIN="$TMPDIR/js_min.js"
js_orig=0 js_min=0
if [ -s "$JS_BLOCK" ]; then
    js_orig=$(wc -c < "$JS_BLOCK")
    $JAVA -jar "$CC_JAR" --js "$JS_BLOCK" --compilation_level ADVANCED --warning_level QUIET 2>/dev/null > "$JS_MIN"
    js_min=$(wc -c < "$JS_MIN")
fi

# 5. Strip CSS and JS blocks from HTML, minify remainder
# -----------------------------------------------------------------------
HTML_BARE="$TMPDIR/html_bare.html"
perl -0777 -pe 's/<style[^>]*>.*?<\/style>/<style><\/style>/gs; s/<script[^>]*>.*?<\/script>/<script><\/script>/gs' "$INPUT" > "$HTML_BARE"

HTML_MIN="$TMPDIR/html_min.html"
html_bare=$(wc -c < "$HTML_BARE")
html-minifier-terser --collapse-whitespace --remove-comments --remove-optional-tags \
    --remove-redundant-attributes --remove-script-type-attributes \
    --remove-tag-whitespace --use-short-doctype --minify-css false --minify-js false \
    < "$HTML_BARE" > "$HTML_MIN" 2>/dev/null
html_min=$(wc -c < "$HTML_MIN")

# 6. Reassemble: inject minified CSS/JS back into minified HTML
# -----------------------------------------------------------------------
REBUILT="$TMPDIR/rebuilt.html"
cp "$HTML_MIN" "$REBUILT"

if [ "$css_min" -gt 0 ]; then
    perl -0777 -pi -e "s|<style></style>|<style>$(cat "$CSS_MIN")</style>|" "$REBUILT" 2>/dev/null || true
fi
if [ "$js_min" -gt 0 ]; then
    perl -0777 -pi -e "s|<script></script>|<script>$(cat "$JS_MIN")</script>|" "$REBUILT" 2>/dev/null || true
fi

total=$(wc -c < "$REBUILT")

# 7. Print table row
# -----------------------------------------------------------------------
base=$(basename "$INPUT")
echo "| pipeline (lc+cc+hmt) | ${total} |"
echo ""
echo "  Breakdown for ${base}:"
echo "    Original:  $(wc -c < "$INPUT") bytes"
echo "    CSS:       ${css_orig} → ${css_min} (lightningcss)"
echo "    JS:        ${js_orig} → ${js_min} (closure-compiler ADVANCED)"
echo "    HTML bare: ${html_bare} → ${html_min} (html-minifier-terser)"
echo "    Reassembled: ${total} bytes"
