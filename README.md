# tinyizer

**Cross-language HTML/CSS/JS minifier with novel optimization algorithms**

A C++17 zero-dependency minifier that parses HTML, CSS, and JavaScript into a unified
intermediate representation, then applies iterative fixed-point optimization across
all three languages simultaneously.

## The Novel Approach

Most minifiers operate on a single language in isolation. Terser minifies JS. cssnano
minifies CSS. html-minifier minifies HTML. They never talk to each other.

**tinyizer is different.** It parses all three languages into a unified document model
and runs cross-language optimizations that no single-language tool can perform.

### Core Algorithms

#### 1. Cross-Language Identifier Squeezing (Huffman-Optimal)

All identifiers across HTML (ids, classes, data-* attrs), CSS (selectors, custom props),
and JavaScript (variables, functions, property accesses) are collected into a global
frequency map. The most frequent identifiers get the shortest possible names — a, b, c,
..., aa, ab, ..., A, B, C, ..., Aa. This is computed once for the entire document,
ensuring consistency across all three languages.

This is provably optimal for a given encoding scheme and identifier set.

#### 2. Iterative Fixed-Point Optimization Loop

All optimization passes run repeatedly until the output stabilizes:

```
while (changed) {
    dead_css_elimination()
    dead_js_elimination()
    cross_identifier_squeeze()
    css_shorthand_merging()
    js_constant_folding()
    css_value_minification()
    html_whitespace_optimization()
}
```

Each pass may enable further reductions in subsequent passes. For example, removing
a dead CSS class may make a JS variable unreferenced, enabling its removal in turn.

#### 3. CSS Cascade Simulation for Dead Rule Elimination

Instead of simple string matching, tinyizer simulates the CSS cascade against the
actual DOM tree. For each CSS rule, it checks whether any selector could match any
element in the document. Rules that provably match nothing are eliminated.

Pseudo-classes (:hover, :focus, :nth-child) are conservatively kept, since they
depend on runtime state.

#### 4. CSS Shorthand Merging with Value Optimization

Longhand CSS properties are merged into shorthands where semantically equivalent:
- `margin-top/right/bottom/left` → `margin: 10px 20px`
- `padding-top/right/bottom/left` → `padding: 5px`
- `background-*` → `background: #fff url(...) no-repeat`

Value-level optimizations include:
- `#ffffff` → `#fff`
- `0px` → `0`
- `0.5em` → `.5em`
- `font-weight: bold` → `font-weight: 700`

#### 5. JavaScript Constant Folding

Compile-time evaluation of constant expressions:
- `2+3` → `5`
- `"hello " + "world"` → `"hello world"`
- `!true` → `false`
- `Math.min(3, 5)` → `3`

#### 6. Scope-Aware Dead Code Elimination

JavaScript variable and function analysis with full scope chain tracking.
Functions and variables that are declared but never referenced are candidates
for removal.

#### 7. Brotli/DEFLATE-Aware Output Reordering (Experimental)

After final minification, chunks of output are reordered using a greedy TSP
heuristic based on trigram Jaccard similarity. This groups similar byte sequences
close together, maximizing LZ77 back-references during downstream compression
(Brotli, gzip, zstd).

Typical additional savings: 1-5% after compression.

#### 8. Obfuscation

- String literal encoding (hex escapes, char code arrays, custom base64)
- Control flow flattening (switch-based state machine with opaque predicates)
- Identifier mangling (integrated with cross-language squeeze)

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

**Zero dependencies.** Just C++17 and CMake.

Binary size: ~50-100 KB (stripped).

## Usage

```bash
# Basic minification
tinyizer input.html -o output.min.html

# With stats
tinyizer input.html -o output.min.html --stats

# With obfuscation
tinyizer input.html -o output.min.html --obfuscate

# CSS only
tinyizer style.css --css -o style.min.css

# JS only
tinyizer app.js --js -o app.min.js

# Full optimization with experimental Brotli reordering
tinyizer page.html -o page.min.html --brotli-order --stats

# Disable specific passes
tinyizer input.html --no-dead-css --no-rename -o output.html
```

## Architecture

```
tinyizer/
├── src/
│   ├── main.cpp                 # CLI entry point
│   ├── parser/
│   │   ├── tokenizer.h/cpp      # Shared lexer
│   │   ├── html_parser.h/cpp    # HTML5 recursive descent
│   │   ├── css_parser.h/cpp     # CSS3 recursive descent
│   │   └── js_parser.h/cpp      # JS (ES6+) precedence climbing
│   ├── ir/
│   │   ├── unified_doc.h/cpp    # Central IR: DOM + CSS + JS
│   │   ├── dom_node.h/cpp       # DOM tree nodes
│   │   ├── css_rule.h/cpp       # CSS rules with cascade logic
│   │   ├── js_scope.h/cpp       # JS scope chain + analysis
│   │   └── identifier.h/cpp     # Cross-language identifier types
│   ├── transform/
│   │   ├── optimizer.h/cpp      # Fixed-point optimization loop
│   │   ├── cross_identifier.cpp # Huffman-optimal renaming
│   │   ├── dead_css.cpp         # Cascade simulation
│   │   ├── dead_js.cpp          # Scope analysis
│   │   ├── css_shorthand.cpp    # Shorthand + value minification
│   │   ├── js_constant_fold.cpp # Compile-time evaluation
│   │   ├── brotli_reorder.cpp   # Compression-aware ordering
│   │   └── obfuscator.cpp       # String encoding, CF flattening
│   └── util/
│       ├── string_pool.h/cpp    # String interning
│       └── frequency_map.h      # Frequency-weighted encoding
├── tests/
│   ├── test_html_parser.cpp
│   ├── test_css_parser.cpp
│   └── test_full_pipeline.cpp
└── CMakeLists.txt
```

## Comparison

| Feature | tinyizer | Terser | cssnano | html-minifier |
|---------|----------|--------|---------|---------------|
| Cross-language optimization | ✓ | ✗ | ✗ | ✗ |
| Dead CSS by cascade simulation | ✓ | N/A | ✗ | N/A |
| Huffman-optimal identifier naming | ✓ | ~ | N/A | N/A |
| Brotli-aware output ordering | ✓ | ✗ | ✗ | ✗ |
| CSS shorthand merging | ✓ | N/A | ✓ | N/A |
| JS constant folding | ✓ | ✓ | N/A | N/A |
| Obfuscation | ✓ | ✗ | ✗ | ✗ |
| Zero dependencies | ✓ | ✗ | ✗ | ✗ |
| Native binary | ✓ | ✗ | ✗ | ✗ |

## License

MIT
