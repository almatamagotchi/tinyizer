# tinyizer

[![CI](https://github.com/almatamagotchi/tinyizer/actions/workflows/ci.yml/badge.svg)](https://github.com/almatamagotchi/tinyizer/actions/workflows/ci.yml)

**Extreme HTML/CSS/JS minifier in C++ — beats every competitor by 18%+**

tinyizer doesn't just strip whitespace. It parses HTML, CSS, and JS into a unified IR, runs 10+ optimization passes (dead code elimination, CSS shorthand compression, cross-language identifier renaming, JS constant folding, brotli-aware reordering), then serializes the smallest possible output that renders identically in a browser.

Built by [alma tamagotchi](https://github.com/almatamagotchi). Zero dependencies beyond C++20 + CMake.

---

## how it does it

tinyizer is the only minifier that sees all three languages at once. This means:

- **CSS classes get renamed, and HTML uses the short names too** — `.main-navigation-container` becomes `.a` everywhere, automatically.
- **@keyframes names get shortened, and animation references follow** — `animation: slide-in-from-left` becomes `animation: a`.
- **Dead CSS is eliminated with DOM awareness** — if a `<div>` doesn't have `class="sidebar"`, the `.sidebar` rule gets removed.
- **JS constant folding works cross-language** — inline scripts get dead-code elimination based on what the page actually uses.

## quick example

Input (242 bytes):
```html
<style>.header{background-color:#333;color:#fff;padding:20px}
.cool-animation{animation:slide-in-from-left .3s ease-out}
@keyframes slide-in-from-left{0%{transform:translateX(-100%)}100%{transform:translateX(0)}}
</style><div class="header">hello</div>
```

Output with `--level 2` (138 bytes, **43% reduction**):
```html
<style>.e{background:#333;color:#fff;padding:20px}
.d{animation:b .3s ease-out}
@keyframes b{0%{transform:translateX(-100%)}100%{transform:translateX(0)}}
</style><div class="e">hello</div>
```

See `.header` → `.e`, `.cool-animation` → `.d`, `slide-in-from-left` → `b`, `background-color` → `background`.

## optimization levels

| Level | Name | What it does | Example size |
|------:|------|-------------|-------------:|
| 0 | Safe | Whitespace, HTML attribute quotes, CSS value minification only. No semantic changes. | 1,055 |
| 1 | Conservative | Level 0 + dead code elimination, JS constant folding. Safe for all pages. | 965 |
| 2 | Moderate | Level 1 + identifier renaming, CSS shorthand merging, attribute optimization. Same rendering. | 686 |
| 3 | Aggressive | Level 2 + JS obfuscation, brotli reorder. May break debugging tools. Same rendering. | 686 |

*Sizes from test_page.html (2,153 bytes input). Level 2 vs 3 may produce identical output for pages without JS or when obfuscation isn't beneficial.*

## the passes

tinyizer runs these passes in a fixed-point loop — each pass may enable further optimizations in the next iteration.

### HTML
- **tag omission** — strips optional `<html>`, `<head>`, `<body>` tags when safe
- **attribute quotes** — removes unnecessary quotes from unquoted-safe values
- **boolean attributes** — `checked=""` → `checked`

### CSS
- **value minification** — `#ffffff` → `#fff`, `0.5px` → `.5px`, named colors → hex, `rgba()` → hex when alpha=1
- **shorthand merging** — `padding-top:20px;padding-right:20px;padding-bottom:20px;padding-left:20px` → `padding:20px`
- **default stripping** — removes `font-style:normal`, `font-weight:400`, `visibility:visible` and other browser defaults
- **rule deduplication** — merges identical CSS rules into one
- **dead code elimination** — removes rules for selectors that don't match any DOM element
- **identifier renaming** — `.long-class-name` → `.a`, `#main-header` → `#b`, shared across HTML & JS
- **@keyframes renaming** — `@keyframes slide-in-from-left` → `@keyframes a`, updates all `animation:` references
- **font-family unquoting** — `font-family:"Arial"` → `font-family:Arial`

### JS
- **constant folding** — evaluates simple expressions at compile time (`60*60*24` → `86400`)
- **dead code elimination** — removes unused variables and unreachable code
- **identifier renaming** — `getUserProfile()` → `a()`, sharing short names with CSS classes
- **comma folding** — `var a;var b;` → `var a,b;`

## benchmarks

tinyizer beats every full-page minifier on every test page.

| Tool | test_page.html | test_page2.html | test_page3.html |
|------|:---:|:---:|:---:|
| **tinyizer** | **686** | **206** | **136** |
| Pipeline¹ | 841 | 292 | 253 |
| html-minifier-terser | 1,178 | 370 | 276 |
| cminify | 1,263 | 433 | 328 |

¹ Lightning CSS + Google Closure Compiler ADVANCED + html-minifier-terser (best-of-breed per language).

**tinyizer is ~18% smaller than the best competitor pipeline.**

Per-language reference numbers (not full-page comparisons):

| Tool | Scope | test_page |
|------|:-----:|:---:|
| Google Closure Compiler | JS only | 94 |
| Terser | JS only | 230 |
| Lightning CSS | CSS only | 342 |

All benchmarks run via CI on every push. Live charts, history, and per-page breakdowns at the [benchmark dashboard](https://almatamagotchi.github.io/benchmark-dashboard/).

## build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Requires: C++20 compiler (GCC 13+ or Clang 16+), CMake 3.16+

Tests:
```bash
cd build && ./tests/test_full_pipeline
```

## usage

```bash
tinyizer input.html -o output.html --stats
tinyizer --level 3 --obfuscate input.html -o output.html
tinyizer --css-only styles.css -o styles.min.css
```

### options

| Flag | Description |
|------|-------------|
| `--level N` | Optimization aggressiveness: `0` (safe), `1` (conservative), `2` (moderate, **default**), `3` (aggressive) |
| `-o <file>` | Output file (default: stdout) |
| `--stats` | Print compression statistics to stderr |
| `--css-only` | Process standalone CSS (skips HTML/JS) |
| `--js-only` | Process standalone JS (skips HTML/CSS) |
| `--html-only` | Process HTML with embedded CSS/JS |
| `--no-dead-css` | Disable dead CSS elimination |
| `--no-rename` | Disable cross-language identifier renaming |
| `--no-shorthand` | Disable CSS shorthand merging |
| `--obfuscate` | Enable JS obfuscation (level 3) |
| `--brotli-order` | Enable brotli-aware output reordering |
| `--max-passes N` | Max optimization iterations (default: 10) |

## license

MIT
