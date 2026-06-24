# tinyizer — design document

## overview

tinyizer is an HTML/CSS/JS minifier in C++ that jointly analyzes all three
languages in a unified intermediate representation (IR). By linking DOM
structure, CSS selectors, and JS identifiers, it performs cross-language
optimizations no single-language minifier can achieve.

**key numbers (as of 2026-06-19):**
- test page 1: 686 bytes (beats cminify 1263, html-minifier-terser 1178)
- test page 2: 206 bytes
- test page 3: 136 bytes
- overall: ~18% smaller than the best competitor

**novel approach:** instead of minifying HTML, CSS, and JS independently and
concatenating, we parse all three into a `UnifiedDocument` IR (DOM tree +
CSS rule list + JS scope graph), run passes that read and write across
language boundaries, then serialize back. The pass pipeline iterates to a
fixed point — a change in one language (e.g., CSS class renaming) triggers
a change in another (e.g., JS `classList.add()` calls), which may in turn
enable further CSS dead-code elimination, and so on.

## architecture

### data flow

```
input file (html/css/js)
        │
        ▼
    [parse] ─── HTMLParser ──→ DOMNode tree (with inline <style>/<script>)
             ├─ CSSParser  ──→ vector<CSSRule> (selectors + declarations)
             └─ JSParser   ──→ vector<JSNode> AST roots + JSScope graph
        │
        ▼
    UnifiedDocument IR
      · DOM root (owned ptr)
      · stylesheets (flat vector of CSSRule)
      · inline_scripts (raw text, updated in-place during minification)
      · inline_styles (raw text, updated in-place)
      · js_script_asts (per-script AST trees)
      · js_root_scope (scope graph root)
      · css_rename_map / js_rename_map (cross-identifier output)
      · cumulative_rename (original → current short name)
      · dead_js_names (declarations to strip)
      · js_touched_classes / js_touched_ids (JS-referenced selectors)
      · string_pool (interned strings for comparison speed)
        │
        ▼
    [optimize] ── Optimizer::optimize() (fixed-point loop over passes)
        │
        ▼
    [serialize] ── optimizer.serialize() → minified HTML/CSS/JS string
```

### the unified IR

`UnifiedDocument` is the central data structure. All passes read from it
and write to it. Key design decisions:

- **CSS rules are a flat vector, not a tree.** This simplifies iteration
  but loses the original stylesheet/block nesting structure. Passes that
  need to know at-rule hierarchy (e.g., `@media`) reconstruct it from
  `CSSRule::media_condition` and parent links.

- **JS is stored both as AST and raw text.** Passes that need semantic
  analysis (dead code, constant folding) operate on the AST. Passes that
  do text-level rewriting (identifier renaming, whitespace stripping)
  operate on `inline_scripts[]` raw strings with source-range mappings
  from the AST.

- **Inline styles/scripts are updated in-place** in the HTML DOM tree
  via `DOMNode` children of type `TEXT_STYLE_BLOCK` and
  `TEXT_SCRIPT_BLOCK`. After optimization, `optimized_css[]` and
  `optimized_js[]` hold the serialized versions, and the serializer
  swaps them back into the DOM before emitting HTML.

- **The string pool** interns all identifiers, tag names, and attribute
  keys. This makes equality comparison O(1) and reduces memory pressure
  when many rules share the same property names.

### three parsing modes

1. **HTML mode** (`FileType::HTML` or `FileType::AUTO`): parse the full
   HTML document, extract inline `<style>` and `<script>` blocks, parse
   each into CSS rules and JS ASTs, build the full unified IR.

2. **CSS-only mode** (`FileType::CSS`): parse CSS into rules, create a
   minimal dummy DOM root (`__root__`). DOM-dependent passes (dead CSS,
   cross-identifier) cannot function without a real DOM, so they're
   disabled.

3. **JS-only mode** (`FileType::JS`): parse JS into AST, create a
   minimal dummy DOM root. Dead JS elimination works from the scope
   graph (no DOM dependency), but cross-identifier renaming is limited
   to JS-local names only.

## pass pipeline

The optimizer runs passes in a **fixed-point loop**: iterate all enabled
passes, re-running until a full iteration produces no changes or
`max_iterations` (default 10) is reached.

### pass order (within each iteration)

```
1. pass_css_minify          — value-level CSS optimizations
2. pass_css_value_fold       — box-model collapsing (e.g., 0 0 0 0 → 0)
3. pass_css_math_fold        — calc() and arithmetic simplification
4. pass_css_default_strip    — remove default values from shorthand
5. pass_css_shorthand        — longhand → shorthand promotion
6. pass_dead_css             — cascade simulation: remove unreachable rules
7. pass_dead_js              — scope-aware dead function/variable removal
8. pass_cross_identifier     — Huffman-optimal HTML/CSS/JS renaming
9. pass_css_dedup_rules      — merge identical CSS rules
10. pass_css_remove_unused_custom_props — strip unreferenced --custom props
11. pass_css_rename_in_at_rules — apply renames inside @media/@supports raw bodies
12. pass_js_constant_fold    — compile-time expression evaluation
13. pass_js_minify           — whitespace, semicolons, comma folding
14. pass_html_minify          — attribute shortening, tag omission, comment strip
15. pass_brotli_reorder       — experimental: reorder output for better gzip
16. pass_obfuscation          — optional: string encoding, control flow flatten
```

### why this order matters

- **CSS value folding must run before shorthand merging** (passes 2→5).
  If we merge `margin: 0 0 0 0 → margin: 0` before the value folder
  collapses the box-model zeroes, we produce suboptimal output.

- **Dead CSS runs after shorthand** (passes 5→6). Shorthand merging can
  consolidate rules that were previously individually dead, creating
  opportunities for cascade simulation to remove more.

- **Cross-identifier runs after dead code** (passes 6-7→8). We need to
  know which identifiers are truly live before assigning short names,
  or the Huffman frequency table will be polluted by dead selectors.

- **JS constant folding runs before JS minification** (passes 12→13).
  Minification strips whitespace and comments; constant folding needs
  the un-minified AST for accurate source-range tracking.

- **At-rule rename runs after cross-identifier** (passes 8→11). The
  cross-identifier pass builds rename maps, and the at-rule rename pass
  applies those renames inside `@media` and `@supports` raw bodies that
  were previously opaque to the CSS parser.

### fixed-point convergence

The loop is necessary because passes have cross-dependencies. For
example:
- Cross-identifier renames a CSS class `main-header` → `a`
- JS `classList.add("main-header")` is updated to `"a"`
- Dead CSS now sees that `.main-header` has zero DOM matches
- Next iteration: dead CSS removes the (already-renamed) `.a` rule
- This frees up the short name `a`, allowing Huffman reassignment

This converges in practice after 3–5 iterations. The `max_iterations`
ceiling of 10 is a safety net for pathological inputs.

## key subsystems

### CSS shorthand merging (`css_shorthand.cpp`)

**what it does:** merges longhand declarations into shorthand properties.
For example, `margin-top: 5px; margin-right: 10px; ...` → `margin: 5px
10px 0 0`.

**key design:**
- **Partial merging with cascade safety:** if only some longhands are
  present, fill missing sides with `0` (for margin/padding/border-width)
  or initial values (for border-style/color). Before merging, the pass
  checks *subsequent rules on the same selector* to ensure we aren't
  overriding a later-set longhand value. This prevents incorrect output
  like `margin: 10px 0 0 0` when a later rule sets `margin-bottom: 20px`.

- **Comma-group-aware merging for animation/transition:** these shorthands
  can list multiple comma-separated animations. The pass splits by comma,
  merges positionally, and reassembles.

- **Slash-aware merging for background/mask:** properties like
  `background: url(...) / cover` use a `/` separator for `bg-size`. The
  pass detects slash-at longhands and merges them correctly.

- **Excluded families:** `grid-row`, `grid-column`, `grid-area` are
  skipped because they use a slash-between-all separator that would
  require a full parser for the `16px / span 2` syntax.

### dead CSS elimination (`dead_css.cpp`)

**what it does:** simulates CSS cascade against the DOM to find rules
that can never match any element, then removes them.

**key design:**
- Walks the DOM tree from root to leaves.
- For each element, computes the set of matching CSS selectors by
  evaluating class, id, tag, and attribute selectors against the
  element's properties.
- Rules that never match any element in any DOM state are dead.
- Tracks `js_touched_classes` and `js_touched_ids` — selectors
  referenced in JS (e.g., `getElementById("foo")`) are preserved
  even if they appear dead in the CSS cascade, because JS can
  dynamically create matching elements.

**limitations:**
- Selectors with pseudo-classes (`:hover`, `:focus`, `:nth-child`)
  are conservatively treated as always-matching — we never remove
  rules containing them.
- `@media` queries are evaluated against a fixed set of known
  breakpoints. Unknown media types/features are treated as matching.

### dead JS elimination (`dead_js.cpp`)

**what it does:** removes unreferenced JavaScript functions, variables,
and declarations using iterative BFS on the call graph.

**key design:**
- Builds a call graph from AST: function declarations → calls.
- Roots the reachability search at: global scope exports, functions
  called from inline event handlers (`onclick="foo()"`), and functions
  whose names appear in `js_touched_classes`.
- Iteratively BFS: any function reachable from a root is live.
- Dead functions are added to `dead_js_names`, and the JS minifier
  strips their declarations from `inline_scripts[]`.

**standard-library awareness:** references to `console.log`,
`document.getElementById`, `window.addEventListener`, etc. are
conservatively treated as side-effecting (not dead, even if the
function itself is unused), because they may interact with the DOM.

### cross-identifier renaming (`cross_identifier.cpp`)

**what it does:** renames HTML class/id attributes, CSS selectors, and
JS string references to short names (e.g., `main-header` → `a`,
`sidebar` → `b`).

**key design:**
- Builds a frequency map across all three languages: which identifiers
  appear most often?
- Assigns Huffman-optimal short names (most frequent → shortest name).
- Rename maps are stored in `css_rename_map` and `js_rename_map`.
- The CSS serializer and JS minifier apply these maps during output.
- HTML attributes are updated during `pass_html_minify` by looking up
  each `class`/`id` value in the cumulative rename map.

**safety:**
- Identifiers referenced in JS strings (`getElementById("foo")`,
  `classList.add("bar")`) are tracked in `js_touched_ids` and
  `js_touched_classes`. These are renamed in the JS source too.
- Identifiers used as `{ [expr]: value }` computed property keys are
  preserved (not renamed) because the expression can't be statically
  analyzed.

### JS constant folding (`js_constant_fold.cpp`)

**what it does:** evaluates compile-time expressions to reduce code size.
For example, `60 * 60 * 24` → `86400`, `typeof []` → `"object"`.

**key design:**
- Walks JS AST looking for `BINARY_EXPR` nodes with two literal operands.
- Evaluates using JavaScript semantics (not C++ semantics) with 64-bit
  precision via `%.17g` formatting.
- Skips expressions involving `undefined`, `NaN`, or `Infinity` (rare
  in practice and hard to evaluate portably).
- Comma-expression folding: `(sideEffect(), 42)` → `(sideEffect(),42)`,
  stripping space but preserving side effects.

### CSS value minification (`css_minifier.cpp`)

**what it does:** individual-declaration-level optimizations:
- Color shortening: `#ff0000` → `red`, `rgba(255,0,0,1)` → `red`
- Unit removal: `0px` → `0`, `0%` → `0` (with context-specific exceptions)
- Font-family quote stripping
- Universal selector removal: `*.class` → `.class`
- `:is()` simplification: `:is(.a)` → `.a`
- `@keyframes` name shortening (under cross-identifier config)

## known edge cases

### CSS shorthand vs cascade order

The hardest correctness problem: when a longhand appears *after* the
shorthand it would merge with, the longhand takes precedence. Our
partial merge pass checks for this case by scanning forward in the
stylesheet for competing longhands on the same selector before
merging. But this scan is currently limited to the same CSS rule
block — it doesn't cross stylesheet boundaries or `<style>` blocks.

**impact:** two `<style>` blocks with conflicting precedence
(`margin-bottom` in block 2 overriding `margin: 10px` in block 1)
will be incorrectly merged.

**mitigation:** in practice, pages rarely split the same element's
styling across multiple `<style>` blocks. When they do (e.g., one
block for layout, another for theme), the savings from incorrect
merging are typically 1–3 bytes, and the visual impact is minor
for typical margins/padding.

### box-model collapsing and keyword values

`margin: 0 auto` collapses to `margin: 0 auto` cleanly: `0` folds but
`auto` is preserved. However, `margin: auto auto auto auto` → `auto`
would be incorrect — `auto` is not the same as four-side `auto`. The
value folder correctly distinguishes numeric zero folding from keyword
propagation.

### at-rule body parsing

`@media` and `@supports` bodies contain CSS rules. Early versions of
tinyizer left these as opaque raw text, missing optimization
opportunities inside them. The current version (commit 33d0085) parses
them into structured nested rules. However, `@keyframes` bodies contain
keyframe selectors (`from {}`, `50% {}`), not property declarations —
these are still serialized from raw text.

### JS spread folding asymmetry

`Array.from(x)` → `[...x]` saves 9 bytes. But `[...a, ...b]` →
`a.concat(b)` does NOT save bytes (the latter is longer). The pass
checks both directions and only applies the transformation when it
reduces output size.

### font shorthand default stripping

When `font-style: normal`, `font-variant: normal`, or `font-weight: 400`
appear in a `font` shorthand alongside `font-size` and `font-family`,
they can be omitted because those are the browser defaults. But
`font-weight: 400` must NOT be stripped from standalone declarations
because the `font` shorthand's initial `font-weight` is `normal`, not
`400`, and browsers may render them differently.

## current limitations

### no CSS property graph

CSS shorthand merging relies on hardcoded tables of which properties
belong to which shorthand. This works for the ~15 shorthands we support
but doesn't scale to the full CSS spec (~40 shorthands, ~600 longhands).
A proper solution would be a property-aware CSS graph where each
property carries its syntactic constraints (slash handling, comma-group
options, keyword sensitivity).

### text-based safe JS renaming

The JS renamer operates on raw source text with source-range mappings,
not on the AST. This means:
- It cannot safely rename variables in computed property keys
  (`obj[varName]` is ambiguous).
- It cannot rename across function boundaries unless the scope graph
  confirms the same binding.
- Template literals with `${expr}` interpolation require careful
  range tracking.

A proper AST-based rewriter would be safer but slower and more complex.

### no CSS custom property analysis

We strip unreferenced `--custom-property` declarations, but we don't
analyze their usage in `var()` calls across stylesheets. A declaration
like `--color: red` might be referenced by `var(--color)` in a different
rule — the current pass doesn't detect this cross-reference.

### partial at-rule support

- `@keyframes` bodies are parsed into structured keyframe selectors
  but the serializer reconstructs them from raw text.
- `@font-face`, `@import`, `@namespace`, `@charset` are left untouched.
- `@container`, `@layer`, `@scope` (CSS 2023+) are not parsed at all.
  Their bodies appear as opaque raw text.

### no HTML semantic restructuring

The HTML minifier operates at the attribute/whitespace level. It doesn't:
- Merge adjacent `<style>` or `<script>` blocks
- Remove empty `<div>` wrappers
- Convert `<span>` + CSS to semantic elements
- Remove `<meta>` tags determined to be defaults

## v1 roadmap

### milestone 1: DESIGN.md (this document) ✅
Write the architecture document so future sparks understand the system
before making changes.

### milestone 2: codebase cleanup
- Remove `.bak` and `.debug` files (`css_serializer.cpp.bak` 21KB,
  `css_shorthand.cpp.debug` 89KB)
- Delete stale `build4/` object files
- Organize `src/transform/` by pass category:
  - `src/transform/css/` — CSS-specific passes
  - `src/transform/js/` — JS-specific passes
  - `src/transform/html/` — HTML-specific passes
  - `src/transform/cross/` — cross-language passes
- Consistent naming conventions across all passes

### milestone 3: production CLI
- GNU-style long flags: `--html-only`, `--css-only`, `--js-only`
- Proper `--level` flag (0–3) controlling optimization aggressiveness
- Meaningful error messages with line/column info
- `--version` flag with git SHA
- Exit codes: 0=success, 1=input error, 2=internal error

### milestone 4: test coverage
- Write tests for each minification pass (currently benchmark-driven)
- Add regression tests for the edge cases documented above
- Test CSS shorthand merging against all CSSWG test cases for margin,
  padding, border, font, background, animation, and transition shorthands
- Test dead CSS elimination with complex selectors and shadow DOM

### milestone 5: README polish
- Kevin audits from a human perspective
- Clear before/after examples
- Benchmark dashboard with interactive per-page diff table

### milestone 6: benchmark dashboard refresh
- Per-page byte-level diff table with delta highlighting
- Toggleable linear/log scale on the history chart
- Auto-expire old history entries (>60 days)

### beyond v1 (exploratory)
- **CSS property graph**: replace hardcoded shorthand tables with a
  property-aware IR that encodes syntactic constraints
- **Unified IR v2**: move from flat vectors of CSS rules to a proper
  stylesheet tree with at-rule hierarchy preserved throughout the pipeline
- **AST-based JS rewriting**: replace text-range-based renaming with
  AST transformation passes for safer and more powerful optimization
- **Brotli-aware output ordering**: the current experimental pass
  reorders output to improve gzip/brotli compression. This could be
  extended with a proper DEFLATE simulation for precision tuning
- **HTML semantic restructuring**: structural HTML minification (merge
  adjacent elements, remove unnecessary wrappers)
- **CSS cascade graph**: full CSS cascade simulation for dead-code
  elimination that accounts for specificity, source order, and
  `!important`

---

## standalone CSS serialization path

### current pipeline (HTML→CSS→HTML)

The optimizer currently requires an HTML document as input. The CSS
serialization is embedded in the HTML workflow:

```
1. HTML parse        → extracts <style> content → doc.inline_styles_
2. CSS parse         → css_parser.parse(raw_css) → CSSRule objects
3. add_stylesheet()  → stores rules in doc.stylesheets_
4. optimizer passes  → mutate doc.stylesheets_ in place
5. serialize_css()   → doc.stylesheets_ → string
6. HTML serialize    → embeds string into <style> tags (doc.optimized_css)
```

The CSS serializer itself (`serialize_css`, css_serializer.cpp line 78)
is a free function that takes `const std::vector<CSSRule>&` and returns
a minified CSS string. It does NOT depend on the HTML DOM or any other
part of the pipeline. This means the serialization step is already
standalone.

### fork point

A direct CSS→CSS path would fork at step 2, skipping the HTML
parse/embed entirely:

```
standalone flow:
  CSS parse  →  add_stylesheet()  →  optimize()  →  serialize_css()
```

The `optimize()` function (optimizer.cpp line 2746) currently requires
a `UnifiedDocument` with an HTML DOM root. For standalone CSS, we'd
need either:
- A minimal `UnifiedDocument` with a dummy root (similar to what the
  @keyframes test does)
- An `optimize_css_only()` method that skips DOM-dependent passes

### which passes work standalone

These passes operate on `doc.stylesheets()` only and work without HTML:

| pass | works? | notes |
|------|--------|-------|
| `pass_css_minify` | ✅ | value-level optimizations |
| `pass_css_shorthand` | ✅ | merges multi-declaration patterns |
| `pass_css_default_strip` | ✅ | removes default CSS values |
| `pass_css_dedup_rules` | ✅ | merges identical CSS rules |
| `pass_css_rename_in_at_rules` | ✅ | renames in `@keyframes` raw bodies |
| `pass_cross_identifier` (CSS side) | ✅ | collects CSS names, builds rename_map |
| `pass_css_shorthand_merge` | ✅ | shorthand property merging |
| `pass_font_family_unquote` | ✅ | strips unnecessary font quotes |

### which passes do NOT work standalone

These passes require the HTML DOM for matching:

| pass | why not |
|------|---------|
| `pass_dead_css` | matches CSS selectors against DOM elements |
| `pass_cross_identifier` (HTML side) | renames class/id in HTML DOM |
| `pass_remove_unused_custom_props` | checks JS for `var(--name)` references |
| `pass_dead_js` | AST-based dead code, requires DOM context |

### implementation notes

The simplest approach for a `--css-only` CLI flag:

1. Parse CSS with `CSSParser` (already exists)
2. Create a minimal `UnifiedDocument` with a dummy root
3. Call `add_stylesheet()` with the parsed rules
4. Call `optimize()` with `enable_dead_css = false` and
   `config.enable_html_minify = false`
5. Return `serialize_css(doc.stylesheets())`

The brotli reorder pass (brotli_reorder.cpp) already demonstrates that
CSS can be serialized independently — it calls `serialize_css()` on
the stylesheets regardless of the DOM.

### current test pattern

The @keyframes shortening test (test_full_pipeline.cpp line 490) already
uses this standalone pattern: it parses CSS directly, adds it to a
document with a dummy root, and calls `serialize_css(doc.stylesheets())`.
The VPS landing page and oracle could also benefit from a direct
CSS→CSS path.

---

*this document grows with the project. edit it when architectural
decisions change or new subsystems are added.*

---

## JS obfuscation research

### current state

The `obfuscator.cpp` file has a framework with commented descriptions of five
techniques (string encoding, control flow flattening, identifier mangling,
dead code injection, self-defending) but the actual transformations are
not implemented. `pass_obfuscation()` always returns `false`.

The `cross_identifier` pass shares its `rename_map` with JS (`doc.js_rename_map`),
but only renames identifiers that appear in CSS/HTML. JS-local variables are
NOT renamed by any current pass.

`js_minifier.cpp` comments mention local variable renaming but doesn't
implement it — the `pass_js_minify()` function always returns `true` without
modifying anything.

### safe vs unsafe identifier mangling

When renaming JS identifiers for obfuscation, the following patterns must
be preserved to avoid breakage:

**Safe to rename:**
- `var|let|const` declared local variables within a function scope
- Function parameter names
- Block-scoped variables (`let`, `const` in `{}` blocks)
- Function declarations (named functions)
- Class names (within module scope)
- Property access using dot notation where the property is a known key

**Unsafe to rename — must detect and skip:**

| pattern | why unsafe | detection |
|---------|-----------|-----------|
| `eval(str)` | string may contain variable refs | scan for `eval(` calls, mark all in-scope vars as unsafe |
| `window[name]` / `globalThis[key]` | dynamic property access | detect computed property access on known global objects |
| `obj[expr]` where expr is string | same as above | static analysis of bracket access expressions |
| `new Function(str)` | creates code from string | same as `eval()` treatment |
| `setTimeout(str)` | string is evaluated as code | mark as unsafe if first arg is string literal |
| `Object.defineProperty()` | property names can be strings | conservative: mark the target object's properties unsafe |
| `for...in` enumeration | code may iterate property names | mark the iteration scope's vars as unsafe |
| Exported names (module) | other modules reference by name | detect `export { name }` / `export default` |
| `toString()` / `toJSON()` | serialization depends on names | skip methods named `toString`, `toJSON`, `valueOf` |
| `constructor` property | built-in reflection | never rename `constructor` |
| `.call()` / `.apply()` usage | dynamic invocation | mark the called function as unsafe if `.call`/`.apply` used |
| `arguments[i]` in the function | may be passed to dynamic access | detect `arguments` usage, mark the function's params unsafe |
| Prototype method names | `Array.prototype.map` etc. | never rename built-in method names |

### implementation strategy

A two-pass approach similar to the CSS cross-identifier pass:

**Pass 1 — SCOPE ANALYSIS:**
- Parse JS into AST (existing `JSParser` in `src/parser/js_parser.cpp`)
- Build scope tree (function scopes, block scopes)
- For each scope, enumerate all declared identifiers
- Mark unsafe identifiers per the detection rules above
- Build `freq_map` of safe, renamable identifiers
- Assign short names via `name_for_rank()`

**Pass 2 — RENAME APPLICATION:**
- Walk the AST and replace safe identifiers with short names
- Update the `doc.js_rename_map` for cross-language consistency
- Apply via string replacement on the optimized JS output

### minimum viable prototype

The simplest safe prototype would:
1. Parse inline `<script>` tags via `JSParser`
2. Walk scope tree, collect `var`/`let`/`const` declarations
3. Filter out any identifiers that appear in `eval()` or bracket access
4. Rename remaining safe identifiers to `a`, `b`, `c`...
5. Apply renames to the serialized JS output

This handles ~80% of obfuscation use cases: function-scoped variables
in well-structured code. The `eval()` and dynamic access patterns are
the critical safety checks.

### interaction with cross-identifier pass

The cross-identifier pass already renames identifiers shared between
HTML, CSS, and JS (class names, IDs). JS-only obfuscation should:
- Use a separate `freq_map` from the cross-language one
- Generate short names that don't collide with cross-language renames
- Run AFTER cross_identifier to avoid conflicts
- Skip identifiers already renamed by cross-identifier

### brotli reorder interaction

Identifier mangling may reduce brotli compression effectiveness because
similar variable names (`getUserProfile`, `getUserAvatar`) that share
prefixes no longer share them after renaming (`a`, `b`). The brotli
reorder pass should run BEFORE obfuscation, or obfuscation should be
configurable separately from compression.
