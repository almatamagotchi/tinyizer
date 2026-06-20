# tinyizer — design

architectural overview of the extreme html/css/js minifier. written for autonomous sparks and kevin.

## project philosophy

tinyizer values algorithmic novelty and maximum compression over ease of implementation. it's not a production saas — it's a research-grade minifier that explores how far you can push compression while preserving identical browser rendering. competitors: cminify, lightning css, terser, google closure compiler. current state: beats all by 18%+ on 3 test pages.

## architecture

### central ir: unifieddocument

everything flows through `UnifiedDocument` (src/ir/unified_doc.h). it holds:

- **html**: dom tree (`DOMNode* root_`)
- **css**: vector of `CSSRule` stylesheets
- **js**: vector of inline scripts (strings) + per-script ASTs (`JSNode`)
- **rename maps**: `css_rename_map`, `js_rename_map`, `cumulative_rename`
- **dead names**: `dead_js_names`, `js_touched_classes`, `js_touched_ids`
- **optimized output**: `optimized_css`, `optimized_js` (populated after all passes)

the ir is currently flat — css rules are vectors, js scripts are strings and ASTs. there is no unified property graph or cross-language ir. this is the biggest architectural gap for v1.

### pass pipeline (in order)

pass order matters — later passes depend on earlier ones. the pipeline runs in optimizer.cpp::optimize():

**phase 1: analysis + renaming**
1. `pass_dead_css` — remove unused css rules (selectors with no dom matches)
2. `pass_dead_js` — dead function elimination via iterative bfs call graph
3. `pass_cross_identifier` — rename css classes/ids across html, css, and js strings
4. `pass_css_remove_unused_custom_props` — strip unused custom properties
5. `pass_css_rename_in_at_rules` — apply class/id renames to @media/@supports bodies

**phase 2: css structural transforms**
6. `pass_css_value_fold` — collapse box-model values (e.g., 0 0 0 0 → 0), rgba→hex, named→hex
7. `pass_css_shorthand` — merge longhands into shorthands with cascade safety
8. `pass_css_math_fold` — constant-fold css calc(), min(), max()

**phase 3: js transforms**
9. `pass_js_constant_fold` — constant propagation, typeof/void folding, spread folding

**phase 4: final minification**
10. `pass_css_minify` — strip whitespace, shorten @keyframes names, font-family quote removal
11. `pass_css_default_strip` — remove default values from font shorthand
12. `pass_css_dedup_rules` — deduplicate identical css rules
13. `pass_html_minify` — strip whitespace, remove default type attrs, boolean attr folding, optional tag omission
14. `pass_js_minify` — trailing semicolon stripping, unnecessary ;} removal

**phase 5: post-processing**
15. `pass_obfuscation` — optional (disabled by default)
16. `pass_brotli_reorder` — optional (disabled by default)

**phase 6: serialization**
- css: serialize all stylesheets → `optimized_css` (css_serializer.cpp)
- js: serialize each optimized script → `optimized_js`
- cross-language: update renamed identifiers in js string literals (getElementById, classList, querySelector)

### key subsystems

**css shorthand merging** (css_shorthand.cpp, 90k) — the largest single file. does partial merging for 4-value families (margin, padding, border-width, border-radius, inset) and slash-at families (background, mask, font). cascade-safe: checks if later rules set missing longhands before merging. handles comma-group-aware shorthands (animation, transition).

**js constant folding** (js_constant_fold.cpp, 45k) — text-based folding without a full ast. walks js source preserving strings and comments, folds arithmetic, typeof, void, and spread expressions. uses snprintf("%.17g") to preserve precision.

**cross-identifier** (cross_identifier.cpp, 15k) — the glue pass. renames css classes and ids across all three languages. distinguishes js_unsafe from html_reserved identifiers. per-identifier origin tracking. updates string literals in optimized js after serialization.

**dom parser** (parser/) — html-aware. identifies inline scripts, inline styles, and structural elements for tag omission.

**ir** (ir/) — intermediate representations. dom_node (html tree), css_rule (selector + declarations), js_scope (scope tree for rvalue analysis), unified_doc (central document).

### known edge cases

- **css shorthand partial merging**: cannot merge grid-row, grid-column, grid-area due to slash-between-all separators
- **js dead function elimination**: requires global scope and exports as live roots; mutual recursion handled correctly
- **single-call function inlining**: skips functions using arguments, this, or recursive calls
- **html attribute unquoting**: skips values with whitespace, quotes, =, <, >, or backtick
- **css cascade safety**: shorthand merging checks if later rules on the same selector set the missing longhands
- **@keyframes names**: shortening handles cross-file references

### current limitations

- no unified ir — css, html, and js are optimized as separate languages with cross-reference via rename maps
- css shorthand merging is case-sensitive and property-name based, not abstract syntax
- js constant folding is text-based, missing complex ast-level optimizations
- no bundle-level css optimization (only per-page)
- no wasm build yet (tinyizer-playground blocked on this)
- test coverage is benchmark-driven, not unit-test driven

## v1 roadmap

1. **DESIGN.md** (this file) — done
2. **codebase cleanup** — remove .bak/.debug files, consistent naming, organize src/ by pass type
3. **production cli** — proper flag parsing, --help, --version, --output, error messages
4. **test coverage** — unit tests for each pass, not just benchmark comparisons
5. **readme polish** — kevin reviews from human perspective
6. **benchmark dashboard** — per-page diff table, linear/log toggle
7. **unified ir** (v1.5) — property-aware css graph, ast-level js transforms, cross-language optimization

## file map

```
src/
  main.cpp                  — entry point, cli, orchestration
  parser/
    html_parser.cpp/html_parser.h  — html → dom tree + inline extraction
    css_parser.cpp/css_parser.h    — css → CSSRule vector
    js_parser.cpp/js_parser.h      — js → JSNode ast
  transform/
    optimizer.cpp/optimizer.h      — pass pipeline, config, orchestration
    css_minifier.cpp               — css whitespace + token minification
    css_serializer.cpp             — CSSRule → string
    css_shorthand.cpp              — longhand → shorthand merging
    dead_css.cpp                   — unused rule elimination
    dead_js.cpp                    — dead function elimination
    cross_identifier.cpp           — cross-language class/id renaming
    js_constant_fold.cpp           — js constant propagation + folding
    js_minifier.cpp                — js token minification
    html_minifier.cpp              — html whitespace + attr minification
    obfuscator.cpp                 — optional obfuscation
    brotli_reorder.cpp             — optional brotli-friendly reordering
  ir/
    unified_doc.h                  — central document ir
    dom_node.h                     — html dom node
    css_rule.h                     — css rule (selector + declarations)
    identifier.h                   — identifier with origin tracking
    js_scope.h                     — js scope tree
  util/
    string_pool.h                  — interning string pool
```
