# tinyizer

[![CI](https://github.com/almatamagotchi/tinyizer/actions/workflows/ci.yml/badge.svg)](https://github.com/almatamagotchi/tinyizer/actions/workflows/ci.yml)
[![changelog](https://img.shields.io/badge/changelog-autonomous-58a6ff)](https://almatamagotchi.github.io/autonomous-changelog/)

**Extreme HTML/CSS/JS minifier and obfuscator in C++**

Zero dependencies beyond C++20 + CMake. Single-pass streaming parser, unified IR, multi-stage optimization — producing the smallest possible output while preserving identical browser rendering.

Built by [Alma Tamagotchi](https://github.com/almatamagotchi) with algorithmic depth over ease of implementation.

## Quick numbers

| Test page | Input | Output | Reduction | Time |
|-----------|------:|-------:|----------:|-----:|
| test_page.html | 2,153 | 785 | 63.5% | 5ms |
| test_page2.html | 521 | 279 | 46.4% | 4ms |
| test_page3.html | 381 | 191 | 49.9% | 6ms |

## Design

tinyizer doesn't just strip whitespace. It:

- **Parses HTML, CSS, and JS into a unified IR** — cross-language optimizations (e.g., sharing identifiers between CSS classes and JS variables)
- **Runs 10+ optimization passes**: dead code elimination, CSS shorthand expansion/compression, JS constant folding, Brotli-aware content reordering, single-longhand-to-shorthand promotion (`background-color`→`background`, etc.)
- **Streaming parser** — single-pass tokenizer feeds incremental IR construction
- **Obfuscation**: rename all identifiers to minimal single-char names, shared across languages

### Architecture

```
HTML/CSS/JS input
    → Tokenizer (streaming)
    → Parser (HTML, CSS, JS)
    → Unified IR (DOM tree + CSS rules + JS scopes)
    → 10 optimization passes
    → Serializer
    → Minified output
```

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Requires: C++20 compiler (GCC 13+ or Clang 16+), CMake 3.16+

To run tests:
```bash
cd build && ./tests/test_html_parser && ./tests/test_css_parser && ./tests/test_full_pipeline
```

## Usage

```bash
./build/tinyizer input.html -o output.html --stats
```

### Options

| Flag | Description |
|------|-------------|
| `-o <file>` | Output file |
| `--stats` | Print compression statistics |
| `--obfuscate` | Enable identifier obfuscation |
| `--no-brotli-reorder` | Skip Brotli-aware reordering |

## Benchmarks

### Full-page minifiers (apples-to-apples)

These tools produce a complete, browser-ready HTML page — CSS, JS, and structure all minified together.

| Tool | test_page.html | test_page2.html | test_page3.html |
|------|:---:|:---:|:---:|
| **tinyizer** | **785** | **279** | **191** |
| Pipeline¹ | 839 | 291 | 251 |
| html-minifier-terser | 1,178 | 370 | 276 |
| cminify | 1,263 | 433 | 328 |

¹ Lightning CSS (CSS) + Google Closure Compiler ADVANCED (JS) + html-minifier-terser (HTML). Best-of-breed combo for each language.

**tinyizer beats every full-page minifier on every test page**, including a best-of-breed pipeline.

### Per-language tools (for reference)

These minify only one language. The numbers shown are **CSS-only or JS-only** — not a full page. Listed for transparency.

| Tool | Scope | test_page | test_page2 | test_page3 |
|------|:-----:|:---:|:---:|:---:|
| Google Closure Compiler | JS only | 94 | 28 | 45 |
| Terser | JS only | 230 | 108 | 71 |
| Lightning CSS | CSS only | 343 | 143 | 115 |

*Full-page comparison = combining these tools with an HTML minifier (see Pipeline above).*

*All benchmarks are run via CI on every push. See [`benchmarks/`](benchmarks/).*

## License

MIT
