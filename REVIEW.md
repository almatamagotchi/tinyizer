# for kevin — reviewing tinyizer v1

written by the auto-run, aug 13 2026. the review has been waiting since june 29. here's everything you need in one place.

## what v1 is

- **24 tests**, all passing (html parser, css parser, full pipeline). verified fresh today, aug 13.
- **production CLI**: `tinyizer --html-only | --css-only | --js-only | --level 0-4 | --output f | --version`, stdin in, stdout out.
- **DESIGN.md**: 408 lines — pass pipeline, the IR vision, known edge cases, the v1 roadmap.
- **benchmarks**: 686/206/136 on the three test pages vs 841/292/253 for the best competitor pipeline (Lightning CSS + Closure ADVANCED + html-minifier-terser). ~18% smaller.
- **last source commit**: june 28. nothing has changed since v1 froze — this review can happen without any catching up.

## what to look at first

1. **DESIGN.md** — the architecture. the IR question (single unified IR vs per-language ASTs), the pass ordering, the brotli reordering (measured: zero benefit, kept anyway — your call on removing it).
2. **the CLI** — build and run it on a real page. `--level` semantics are documented; check they match your instinct.
3. **the benchmark numbers** — they're honest (CI-verified, live at the dashboard), but the test pages are small and synthetic. the real question for v1 is whether the 18% holds on a real site. i've never run it against one.

## what's still undecided (all yours)

- the IR: v1 works without a full unified IR. DESIGN.md sketches one. is that v2, or is v1 not v1 until it exists?
- brotli-aware reordering: measured as no-op on all three test pages. remove it or keep it?
- level 4 (class/ID renaming with DOM awareness) is the riskiest pass — dead CSS elimination that depends on the HTML parse. most likely to break real pages.
- licensing/passive income: you mentioned exploring tinyizer licensing (june). never decided.

## what i'd do if you don't get to it

nothing. it's stable. the review is the only open item — the code doesn't need me until you've looked.

---

*if any of this is wrong by the time you read it, the tests still pass and the dashboard is still live — trust those over this note.*
