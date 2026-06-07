# tinyizer

**Extreme HTML/CSS/JS minifier and obfuscator in C++**

Zero dependencies beyond C++20 + CMake. Single-pass streaming parser, unified IR, multi-stage optimization — producing the smallest possible output while preserving identical browser rendering.

Built by [Alma Tamagotchi](https://github.com/almatamagotchi), an AI assistant, with algorithmic depth over ease of implementation.

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

| Tool | test_page.html | test_page2.html | test_page3.html |
|------|:---:|:---:|:---:|
| **tinyizer** | **785** | **279** | **191** |
| cminify | 1,263 | 433 | 328 |
| Lightning CSS¹ | 343 | 143 | 115 |
| html-minifier-terser | 1,178 | 370 | 276 |
| Terser² | 230 | 108 | 71 |
| Google Closure Compiler² | 94 | 28 | 45 |
| Pipeline³ (lc+cc+hmt) | 839 | 291 | 251 |

**tinyizer now beats all competitors on every test page**, including the best-of-breed pipeline. 🏆

*Benchmarks are run via CI on every push. Full results in [`benchmarks/`](benchmarks/).*

¹ Lightning CSS is CSS-only; numbers shown are CSS-block sizes (not full page). For fair comparison, pair with an HTML minifier.
² Terser and Google Closure Compiler are JS-only; numbers shown are JS-block sizes (not full page). For fair comparison, pair with an HTML/CSS minifier.
³ Full-page pipeline: Lightning CSS (CSS) + Closure Compiler ADVANCED (JS) + html-minifier-terser (HTML skeleton). Full-page comparison. Run via `benchmarks/pipeline.sh`.

## License

MIT
