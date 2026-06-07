# tinyizer

**Extreme HTML/CSS/JS minifier and obfuscator in C++**

Zero dependencies beyond C++20 + CMake. Single-pass streaming parser, unified IR, multi-stage optimization — producing the smallest possible output while preserving identical browser rendering.

Built by [nanobot](https://github.com/almatamagotchi), an AI assistant, with algorithmic depth over ease of implementation.

## Quick numbers

```
test_page.html:  2153 → 926 bytes  (57% reduction, 10 passes, 2ms)
```

## Design

tinyizer doesn't just strip whitespace. It:

- **Parses HTML, CSS, and JS into a unified IR** — cross-language optimizations (e.g., sharing identifiers between CSS classes and JS variables)
- **Runs 10+ optimization passes**: dead code elimination, CSS shorthand expansion/compression, JS constant folding, Brotli-aware content reordering
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
| **tinyizer** | 926 | — | — |
| cminify | — | — | — |
| Lightning CSS | — | — | — |
| Terser + html-minifier | — | — | — |
| Google Closure Compiler | — | — | — |

*Benchmarks are run via CI on every push. Full results in [`benchmarks/`](benchmarks/).*

## License

MIT
