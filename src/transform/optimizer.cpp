#include "optimizer.h"
#include "serializer.h"
#include "../util/frequency_map.h"
#include "../parser/tokenizer.h"
#include "../parser/js_parser.h"
#include <functional>
#include <queue>
#include <algorithm>
#include <set>
#include <sstream>
#include <cctype>
#include <cstring>
#include <iostream>
#include <regex>

namespace tinyizer {

// Forward declarations
static void scan_string_ranges_in_text(const std::string& text,
                                       std::vector<std::pair<size_t, size_t>>& ranges);

// ---- Strip 'var' keyword from top-level declarations in non-strict mode ----
// In non-strict mode, `var x = 1` is equivalent to `x = 1` (outside strict).
// Removing the `var ` saves 4 bytes per declaration. Strips at brace depth 0
// (top‑level global), inside for‑loop initializers, and inside function scopes
// when the declaration is single‑variable (no comma‑separated multi‑decl).
static size_t strip_var_keywords(std::string& js) {
    // Skip if strict mode directive is present
    if (js.find("\"use strict\"") == 0 || js.find("'use strict'") == 0) return 0;

    // Pre-scan string ranges to skip `var` inside strings
    std::vector<std::pair<size_t, size_t>> string_ranges;
    scan_string_ranges_in_text(js, string_ranges);

    auto inside_string = [&](size_t pos) {
        if (string_ranges.empty()) return false;
        auto it = std::upper_bound(string_ranges.begin(), string_ranges.end(), pos,
            [](size_t p, const auto& r) { return p < r.first; });
        if (it == string_ranges.begin()) return false;
        --it;
        return pos >= it->first && pos < it->second;
    };

    auto is_ident = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
    };

    // Check whether a var declaration starting at pos (after "var ") contains
    // a comma before its terminating semicolon — if so, it's a multi‑decl like
    // `var a=1,b=2;` and we cannot safely strip `var `.
    auto is_single_decl = [&](size_t scan_start) -> bool {
        int paren_depth = 0;
        for (size_t i = scan_start; i < js.size(); i++) {
            if (inside_string(i)) continue;
            char ch = js[i];
            if (ch == '(') paren_depth++;
            else if (ch == ')') { if (paren_depth > 0) paren_depth--; }
            else if (ch == ';' && paren_depth == 0) return true;   // single decl
            else if (ch == ',' && paren_depth == 0) return false;  // multi‑decl
            else if (ch == '{' || ch == '}') return false;          // hit block — bail
        }
        return true; // end of input — treat as single decl
    };

    // Check if pos points to "var " that is inside a for-loop initializer.
    // Scans backwards from pos for "for(" or "for (" to confirm.
    auto inside_for_init = [&](size_t pos) -> bool {
        if (pos < 5) return false;           // need at least "for(v"
        size_t p = pos - 1;
        // skip whitespace between '(' and 'var'
        while (p > 0 && (js[p] == ' ' || js[p] == '\t')) p--;
        if (js[p] != '(') return false;      // need an open-paren before var
        if (p < 3) return false;
        // check for "for" before the '('
        if (js.compare(p-3, 3, "for") == 0) {
            // word boundary before "for"
            size_t before = p - 3;
            return (before == 0 || !is_ident(js[before-1]));
        }
        return false;
    };

    size_t removed = 0;
    int depth = 0;

    for (size_t pos = 0; pos < js.size(); ) {
        char c = js[pos];

        // Track brace depth (skip braces inside strings)
        if (!inside_string(pos)) {
            if (c == '{') depth++;
            else if (c == '}') depth--;
        }

        // Check for "var " at top level (depth 0), inside for-loop init,
        // or inside function scope with a single-decl (no comma).
        if (!inside_string(pos) &&
            js.compare(pos, 4, "var ") == 0) {

            bool can_strip = false;
            if (depth == 0) {
                // top level — strip if left word boundary
                can_strip = (pos == 0 || !is_ident(js[pos - 1]));
            } else if (inside_for_init(pos)) {
                // inside for(...) initializer at any depth
                can_strip = true;
            } else if (depth > 0 && is_single_decl(pos + 4)) {
                // inside function scope, single-variable declaration
                can_strip = (pos == 0 || !is_ident(js[pos - 1]));
            }

            if (can_strip) {
                js.erase(pos, 4);  // remove "var "
                removed++;
                continue;  // don't advance pos — recheck this position
            }
        }

        pos++;
    }

    return removed;
}

// ---- Replace `undefined` with `void 0` ----
// In expressions, `undefined` is 9 bytes while `void 0` is 6, saving 3
// bytes per occurrence. Only replaces the global `undefined` identifier
// (skips property accesses like `obj.undefined`, declarations, parameters,
// and occurrences within string literals or comments).
static size_t replace_undefined_with_void(std::string& js) {
    // Pre-scan string ranges to skip `undefined` inside strings
    std::vector<std::pair<size_t, size_t>> string_ranges;
    scan_string_ranges_in_text(js, string_ranges);

    auto inside_string = [&](size_t pos) {
        if (string_ranges.empty()) return false;
        auto it = std::upper_bound(string_ranges.begin(), string_ranges.end(), pos,
            [](size_t p, const auto& r) { return p < r.first; });
        if (it == string_ranges.begin()) return false;
        --it;
        return pos >= it->first && pos < it->second;
    };

    auto is_ident = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
    };

    const std::string target = "undefined";
    size_t replacements = 0;

    // Find all positions of 'undefined' first (scanning left to right),
    // then replace from right to left to avoid position shift issues.
    std::vector<size_t> positions;
    size_t pos = 0;
    while ((pos = js.find(target, pos)) != std::string::npos) {
        // Skip if inside a string literal
        if (inside_string(pos)) { pos += target.size(); continue; }

        // Skip if preceded by a '.' (property access: obj.undefined)
        if (pos > 0 && js[pos - 1] == '.') { pos += target.size(); continue; }

        // Skip if part of a larger identifier (rare but for safety)
        if (pos > 0 && is_ident(js[pos - 1])) { pos += target.size(); continue; }
        if (pos + target.size() < js.size() && is_ident(js[pos + target.size()])) {
            pos += target.size(); continue;
        }

        // Skip if this is a declaration: var/let/const undefined = ...
        // or a function/arrow parameter: (undefined), function(undefined), (undefined)=>{} etc.
        // Scan backwards for var/let/const/function keyword before any non-whitespace.
        // Also check for parameter position (preceded by '(' or ',').
        bool is_decl = false;
        {
            size_t scan = pos;
            while (scan > 0 && (js[scan - 1] == ' ' || js[scan - 1] == '\t' || js[scan - 1] == '\n' || js[scan - 1] == '\r'))
                scan--;
            // Check for var/let/const
            if (scan >= 3 && js.compare(scan - 3, 3, "var") == 0) {
                if (scan >= 4 && is_ident(js[scan - 4])) { /* nope */ }
                else is_decl = true;
            }
            if (scan >= 3 && js.compare(scan - 3, 3, "let") == 0) {
                if (scan >= 4 && is_ident(js[scan - 4])) { /* nope */ }
                else is_decl = true;
            }
            if (scan >= 5 && js.compare(scan - 5, 5, "const") == 0) {
                if (scan >= 6 && is_ident(js[scan - 6])) { /* nope */ }
                else is_decl = true;
            }
            // Check for function/arrow parameter: (undefined, ...) or ,undefined)
            // scan > 0: there's at least one char before the identifier.
            if (scan > 0 && (js[scan - 1] == '(' || js[scan - 1] == ',')) {
                // Check backwards for 'function' keyword (possibly with a name)
                // kw should point to the '(' or ',' to start scanning backwards
                size_t kw = scan;
                // Skip past '(' or ',' to look for function name before it
                if (kw > 0 && (js[kw - 1] == '(' || js[kw - 1] == ','))
                    kw--;
                // Now skip whitespace and identifiers backward to find 'function'
                while (kw > 0 && (js[kw - 1] == ' ' || js[kw - 1] == '\t' || js[kw - 1] == '\n' || js[kw - 1] == '\r'))
                    kw--;
                // Skip the optional function name: 'function f(' => skip 'f'
                while (kw > 0 && is_ident(js[kw - 1]))
                    kw--;
                // Skip whitespace between name and 'function'
                while (kw > 0 && (js[kw - 1] == ' ' || js[kw - 1] == '\t' || js[kw - 1] == '\n' || js[kw - 1] == '\r'))
                    kw--;
                if (kw >= 8 && js.compare(kw - 8, 8, "function") == 0) {
                    is_decl = true;
                } else {
                    // Arrow function parameter: (undefined)=> or (a,undefined)=> or (undefined,...)
                    // Forward-scan past this identifier to find ) or , then =>
                    size_t fwd = pos + target.size();
                    while (fwd < js.size() && (js[fwd] == ' ' || js[fwd] == '\t' || js[fwd] == '\n' || js[fwd] == '\r'))
                        fwd++;
                    if (fwd < js.size() && (js[fwd] == ')' || js[fwd] == ',')) {
                        // Find matching closing paren and arrow
                        size_t arrow_scan = fwd;
                        int depth = (js[fwd] == ')') ? 0 : 1;
                        if (js[fwd] == ',') {
                            // Advance past remaining params to find closing )
                            while (arrow_scan < js.size() && depth > 0) {
                                arrow_scan++;
                                if (arrow_scan >= js.size()) break;
                                if (js[arrow_scan] == '(') depth++;
                                else if (js[arrow_scan] == ')') depth--;
                            }
                        }
                        // Now at ')' (or past it), skip to look for =>
                        while (arrow_scan < js.size() && (js[arrow_scan] == ' ' || js[arrow_scan] == '\t' || js[arrow_scan] == '\n' || js[arrow_scan] == '\r' || js[arrow_scan] == ')'))
                            arrow_scan++;
                        if (arrow_scan + 1 < js.size() && js[arrow_scan] == '=' && js[arrow_scan + 1] == '>')
                            is_decl = true;
                    }
                }
            }
        }
        if (is_decl) { pos += target.size(); continue; }

        positions.push_back(pos);
        pos += target.size();
    }

    // Replace from right to left (replacing 9 chars with 6 chars shifts earlier
    // positions, but since we're moving right-to-left, already-collected
    // positions are unaffected).
    for (auto it = positions.rbegin(); it != positions.rend(); ++it) {
        js.replace(*it, target.size(), "void 0");
        replacements++;
    }

    return replacements;
}

// ---- Replace `Infinity` with `1/0` ----
// `Infinity` is 8 bytes while `1/0` is 3, saving 5 bytes per occurrence.
// Same safety rules as the undefined→void 0 pass: skip strings, property
// accesses, declarations, and function parameters.
static size_t replace_infinity_with_division(std::string& js) {
    std::vector<std::pair<size_t, size_t>> string_ranges;
    scan_string_ranges_in_text(js, string_ranges);

    auto inside_string = [&](size_t pos) {
        if (string_ranges.empty()) return false;
        auto it = std::upper_bound(string_ranges.begin(), string_ranges.end(), pos,
            [](size_t p, const auto& r) { return p < r.first; });
        if (it == string_ranges.begin()) return false;
        --it;
        return pos >= it->first && pos < it->second;
    };

    auto is_ident = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
    };

    const std::string target = "Infinity";
    size_t replacements = 0;

    std::vector<size_t> positions;
    size_t pos = 0;
    while ((pos = js.find(target, pos)) != std::string::npos) {
        if (inside_string(pos)) { pos += target.size(); continue; }

        // Skip if preceded by '.' (property access: obj.Infinity)
        if (pos > 0 && js[pos - 1] == '.') { pos += target.size(); continue; }

        // Skip if part of a larger identifier
        if (pos > 0 && is_ident(js[pos - 1])) { pos += target.size(); continue; }
        if (pos + target.size() < js.size() && is_ident(js[pos + target.size()])) {
            pos += target.size(); continue;
        }

        // Skip declarations and parameters (same logic as undefined pass)
        bool is_decl = false;
        {
            size_t scan = pos;
            while (scan > 0 && (js[scan - 1] == ' ' || js[scan - 1] == '\t' || js[scan - 1] == '\n' || js[scan - 1] == '\r'))
                scan--;
            if (scan >= 3 && js.compare(scan - 3, 3, "var") == 0) {
                if (scan < 4 || !is_ident(js[scan - 4])) is_decl = true;
            }
            if (scan >= 3 && js.compare(scan - 3, 3, "let") == 0) {
                if (scan < 4 || !is_ident(js[scan - 4])) is_decl = true;
            }
            if (scan >= 5 && js.compare(scan - 5, 5, "const") == 0) {
                if (scan < 6 || !is_ident(js[scan - 6])) is_decl = true;
            }
            if (scan > 0 && (js[scan - 1] == '(' || js[scan - 1] == ','))
                is_decl = true;
        }
        if (is_decl) { pos += target.size(); continue; }

        positions.push_back(pos);
        pos += target.size();
    }

    for (auto it = positions.rbegin(); it != positions.rend(); ++it) {
        js.replace(*it, target.size(), "1/0");
        replacements++;
    }

    return replacements;
}

// ---- Strip redundant parens after `return` ----
// `return (expr)` → `return expr` saves 2 bytes per occurrence.
// The parens are redundant in expression context; only needed with ASI
// (line break between `return` and expression), which doesn't apply
// in minified single-line output.
static size_t strip_return_parens(std::string& js) {
    std::vector<std::pair<size_t, size_t>> string_ranges;
    scan_string_ranges_in_text(js, string_ranges);

    auto inside_string = [&](size_t pos) {
        if (string_ranges.empty()) return false;
        auto it = std::upper_bound(string_ranges.begin(), string_ranges.end(), pos,
            [](size_t p, const auto& r) { return p < r.first; });
        if (it == string_ranges.begin()) return false;
        --it;
        return pos >= it->first && pos < it->second;
    };

    const std::string prefix = "return(";
    size_t replacements = 0;
    size_t pos = 0;

    while ((pos = js.find(prefix, pos)) != std::string::npos) {
        // Skip if inside string
        if (inside_string(pos)) { pos += prefix.size(); continue; }

        // Verify `return` is a standalone keyword (not part of a larger identifier)
        if (pos >= 6) {
            char prev = js[pos - 1];
            if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_' || prev == '$') {
                pos += prefix.size(); continue;
            }
        }

        // Find matching closing paren
        size_t open_pos = pos + 6; // position of '('
        size_t scan = open_pos + 1;
        int depth = 1;
        bool found = false;
        while (scan < js.size() && depth > 0) {
            if (inside_string(scan)) { scan++; continue; }
            if (js[scan] == '(') depth++;
            else if (js[scan] == ')') depth--;
            if (depth == 0) { found = true; break; }
            scan++;
        }
        if (!found || scan <= open_pos + 1) {
            // No matching paren, or empty parens: skip
            pos += prefix.size(); continue;
        }

        // Extract contents between parens
        size_t content_start = open_pos + 1;
        size_t content_end = scan; // position of ')'
        std::string contents = js.substr(content_start, content_end - content_start);

        // Replace `return(contents)` with `return contents`
        // Need a space between `return` and contents when contents starts with
        // an identifier char (+/-/!/~ for unary ops bind to `return` otherwise)
        char first = '\0';
        for (size_t i = 0; i < contents.size(); i++) {
            if (contents[i] != ' ' && contents[i] != '\t' && contents[i] != '\n' && contents[i] != '\r') {
                first = contents[i];
                break;
            }
        }
        bool need_space = (first != '\0' &&
            (std::isalnum(static_cast<unsigned char>(first)) ||
             first == '_' || first == '$' || first == '!' || first == '~' ||
             first == '+' || first == '-'));

        std::string replacement = "return";
        if (need_space) replacement += ' ';
        replacement += contents;

        js.replace(pos, content_end - pos + 1, replacement);
        replacements++;
        pos += replacement.size();
    }

    return replacements;
}

// ---- Strip else-after-jump ----
// When an if-block ends with a return/throw/break/continue, the following
// else is redundant because execution never reaches it when the if-taken path
// jumps.  Replace `}else` with `}` (the else-body becomes unconditional).
// e.g. if(x){return 1;}else{...} → if(x){return 1;}...  saves 4-5 bytes.
static size_t strip_else_after_jump(std::string& js) {
    std::vector<std::pair<size_t, size_t>> string_ranges;
    scan_string_ranges_in_text(js, string_ranges);

    auto inside_string = [&](size_t pos) {
        if (string_ranges.empty()) return false;
        auto it = std::upper_bound(string_ranges.begin(), string_ranges.end(), pos,
            [](size_t p, const auto& r) { return p < r.first; });
        if (it == string_ranges.begin()) return false;
        --it;
        return pos >= it->first && pos < it->second;
    };

    auto is_id = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
    };

    static const std::vector<std::string> jumps = {"return","throw","break","continue"};

    size_t replacements = 0;
    size_t pos = 0;

    while ((pos = js.find("else", pos)) != std::string::npos) {
        if (inside_string(pos)) { pos += 4; continue; }

        // Walk back past whitespace to find the terminator before `else`
        size_t term = pos;
        while (term > 0 && (js[term-1] == ' ' || js[term-1] == '\t' ||
                              js[term-1] == '\n' || js[term-1] == '\r'))
            term--;
        if (term == 0) { pos += 4; continue; }
        char term_char = js[term-1];
        if (term_char != '}' && term_char != ';') { pos += 4; continue; }

        // Case 1: braced if  ->  } else  -> walk back from }
        // Case 2: unbraced if  ->  ; else  -> check the statement before ;
        if (term_char == '}') {
            size_t brace = term - 1;
            if (inside_string(brace)) { pos += 4; continue; }

            // First, check that the last non-whitespace before `}` is `;`
            // (or the expression directly).  This ensures we don't match
            // a return that sits earlier in the block.
            size_t chk = brace;
            while (chk > 0 && (js[chk-1] == ' ' || js[chk-1] == '\t' || js[chk-1] == '\n' || js[chk-1] == '\r'))
                chk--;
            // If the last meaningful char is `}`, skip (non-jump block).
            if (chk > 0 && js[chk-1] == '}') { pos += 4; continue; }
            // Allow `;` (normal) or direct expression (e.g. `return x}`).
            // Otherwise we'll scan for keywords below.

            int depth = 1;
            bool found_jump = false;
            size_t i = brace;
            while (i > 0) {
                i--;
                char c = js[i];
                if (inside_string(i)) continue;
                if (c == '}') { depth++; continue; }
                if (c == '{') {
                    depth--;
                    if (depth == 0) break;
                    continue;
                }
                if (depth != 1) continue; // only examine our block level

                if (!is_id(c))
                    continue;
                size_t id_end = i + 1;
                while (i > 0 && is_id(js[i-1])) i--;
                std::string id = js.substr(i, id_end - i);
                if (std::find(jumps.begin(), jumps.end(), id) != jumps.end()) {
                    found_jump = true;
                    break;
                }
            }

            if (!found_jump) { pos += 4; continue; }

            size_t erase_start = brace + 1; // after }
            size_t erase_end = pos + 4;     // past `else`
            while (erase_end < js.size() && (js[erase_end] == ' ' || js[erase_end] == '\t' ||
                                              js[erase_end] == '\n' || js[erase_end] == '\r'))
                erase_end++;
            js.erase(erase_start, erase_end - erase_start);
            replacements++;
        } else { // term_char == ';'
            size_t semi = term - 1;
            if (inside_string(semi)) { pos += 4; continue; }

            // Walk backwards from the semicolon, past the jump expression,
            // to find the keyword (return/throw/break/continue).
            // Strategy: scan backward in two phases.
            // Phase 1: skip whitespace after the expression.
            // Phase 2: walk the expression (matching delimiters) until we
            //   hit something that isn't part of it — that should be the keyword.

            size_t i = semi;
            // Phase 1
            while (i > 0 && (js[i-1] == ' ' || js[i-1] == '\t' || js[i-1] == '\n' || js[i-1] == '\r'))
                i--;
            if (i == 0) { pos += 4; continue; }

            // Phase 2: walk backwards over the expression.
            // We scan for the start of the preceding statement.  Because this
            // is minified code, the statement started right after one of:
            // `{`  `}`  `;`  `:`  `(` (the if-condition paren)
            // and we stop at that boundary.
            i--;
            int p_depth = 0, b_depth = 0, c_depth = 0;
            while (i > 0 && !inside_string(i)) {
                char c = js[i];
                if (c == ')' || c == ']' || c == '}') {
                    if (c == ')') p_depth++;
                    else if (c == ']') b_depth++;
                    else c_depth++;
                    i--;
                    continue;
                }
                if (c == '(') {
                    if (p_depth > 0) {
                        p_depth--;
                        // Check if this '(' opens a control structure (if, while,
                        // for, switch, catch, with) rather than a nested call.
                        // If so, stop — the enclosed )...(... pair is the boundary.
                        size_t check = i;
                        while (check > 0 && (js[check-1] == ' ' || js[check-1] == '\t' ||
                                              js[check-1] == '\n' || js[check-1] == '\r'))
                            check--;
                        if (check > 1 && is_id(js[check-1])) {
                            size_t ks = check - 1;
                            while (ks > 0 && is_id(js[ks-1])) ks--;
                            std::string kw = js.substr(ks, check - ks);
                            if (kw == "if" || kw == "while" || kw == "for" ||
                                kw == "switch" || kw == "catch" || kw == "with")
                                break; // boundary of enclosing control construct
                        }
                        i--;
                        continue;
                    }
                    break; // unmatched '(' → start of if-condition or call boundary
                }
                if (c == '[') {
                    if (b_depth > 0) { b_depth--; i--; continue; }
                    break;
                }
                if (c == '{') {
                    if (c_depth > 0) { c_depth--; i--; continue; }
                    break;
                }
                // At depth 0: check for statement boundaries
                if (p_depth == 0 && b_depth == 0 && c_depth == 0) {
                    if (c == ';' || c == ':' || c == ',') break;
                    // If it's not a valid expression character at the top level,
                    // and it's not part of an identifier that might be the keyword,
                    // stop.
                    if (!is_id(c) && c != '.' && c != '!' && c != '~' &&
                        c != '-' && c != '+' && c != ' ' && c != '\t' &&
                        c != '\n' && c != '\r' && c != '\'' && c != '"' && c != '`') {
                        break;
                    }
                }
                i--;
            }

            // Now i points to the char just before our statement's keyword,
            // or i == 0, or i is sitting on the statement-start delimiter.
            // Find the keyword.
            size_t kw_start = i + 1;
            // If we broke on `(` that opens the enclosing if/while/for,
            // skip forward past the matching `)` to find the statement's keyword.
            // Note: we broke inside the while loop, so i points AT the '('.
            if (kw_start <= semi && i < semi && js[i] == '(') {
                kw_start = i;  // start from the '('
                int skip_depth = 1;
                while (kw_start < semi) {
                    kw_start++;
                    if (js[kw_start] == '(') skip_depth++;
                    else if (js[kw_start] == ')') {
                        skip_depth--;
                        if (skip_depth == 0) { kw_start++; break; }
                    }
                }
            }
            while (kw_start < semi && (js[kw_start] == ' ' || js[kw_start] == '\t' ||
                                        js[kw_start] == '\n' || js[kw_start] == '\r'))
                kw_start++;
            size_t kw_end = kw_start;
            while (kw_end < semi && is_id(js[kw_end]))
                kw_end++;
            std::string kw = js.substr(kw_start, kw_end - kw_start);
            if (std::find(jumps.begin(), jumps.end(), kw) != jumps.end()) {
                size_t erase_start = semi + 1; // after ;
                size_t erase_end = pos + 4;     // past `else`
                while (erase_end < js.size() && (js[erase_end] == ' ' || js[erase_end] == '\t' ||
                                                  js[erase_end] == '\n' || js[erase_end] == '\r'))
                    erase_end++;
                js.erase(erase_start, erase_end - erase_start);
                replacements++;
            }
            pos += 4;
        }
    }

    return replacements;
}

// ---- Strip orphan top-level assignments ----
// After var stripping, a dead `var x = 1;` becomes `x = 1;` — an assignment
// whose target is never read elsewhere. Scan top-level (depth 0) for
// identifier = ...; patterns and remove the whole statement when the
// identifier has no other occurrences in the script.
static size_t strip_orphan_assignments(std::string& js) {
    std::vector<std::pair<size_t, size_t>> string_ranges;
    scan_string_ranges_in_text(js, string_ranges);

    auto inside_string = [&](size_t pos) {
        if (string_ranges.empty()) return false;
        auto it = std::upper_bound(string_ranges.begin(), string_ranges.end(), pos,
            [](size_t p, const auto& r) { return p < r.first; });
        if (it == string_ranges.begin()) return false;
        --it;
        return pos >= it->first && pos < it->second;
    };

    auto is_ident = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
    };

    size_t removed = 0;
    int depth = 0;

    for (size_t pos = 0; pos < js.size(); ) {
        char c = js[pos];

        // Track brace depth
        if (!inside_string(pos)) {
            if (c == '{') depth++;
            else if (c == '}') {
                if (depth > 0) depth--;
            }
        }

        // At top level, look for identifier = value;
        if (depth == 0 && !inside_string(pos) && is_ident(c) &&
            !std::isdigit(static_cast<unsigned char>(c))) {

            size_t id_start = pos;
            while (pos < js.size() && is_ident(js[pos])) pos++;
            size_t id_end = pos;
            if (id_end == id_start) { pos++; continue; }
            std::string ident = js.substr(id_start, id_end - id_start);

            // Skip whitespace
            while (pos < js.size() && (js[pos] == ' ' || js[pos] == '\t')) pos++;

            // Must be followed by '='
            if (pos >= js.size() || js[pos] != '=' || inside_string(pos)) {
                pos = id_end;
                continue;
            }

            // Find statement end (semicolon at depth 0, outside strings)
            size_t stmt_end = pos; // start from '='
            int local_depth = 0;
            while (stmt_end < js.size()) {
                char sc = js[stmt_end];
                if (!inside_string(stmt_end)) {
                    if (sc == '{' || sc == '(') local_depth++;
                    else if (sc == '}' || sc == ')') {
                        if (local_depth > 0) local_depth--;
                    }
                    else if (sc == ';' && local_depth == 0) break;
                }
                stmt_end++;
            }
            if (stmt_end >= js.size()) { pos = id_end; continue; } // no semicolon

            // Check if ident appears elsewhere in js (outside this statement)
            bool appears_elsewhere = false;
            for (size_t i = 0; i < js.size(); ) {
                if (i >= id_start && i <= stmt_end) {
                    i = stmt_end + 1;
                    continue;
                }
                if (i + ident.size() > js.size()) break;
                if (!inside_string(i) && js.compare(i, ident.size(), ident) == 0) {
                    bool left_ok = (i == 0 || !is_ident(js[i - 1]));
                    bool right_ok = (i + ident.size() >= js.size() || !is_ident(js[i + ident.size()]));
                    if (left_ok && right_ok) {
                        appears_elsewhere = true;
                        break;
                    }
                }
                i++;
            }

            if (!appears_elsewhere) {
                // Check if the RHS calls a user-defined function (potential
                // side effect).  If the call target name appears in a
                // "function <name>(" declaration elsewhere in the JS,
                // preserve the statement.
                bool has_side_effect = false;
                for (size_t i = pos + 1; i + 2 < stmt_end && i < js.size(); ) {
                    if (js[i] == '(' && !inside_string(i)) {
                        // Walk backward from '(' to find the function name
                        size_t name_end = i;
                        while (name_end > pos + 1 && js[name_end - 1] == ' ')
                            name_end--;
                        size_t name_start = name_end;
                        while (name_start > pos && is_ident(js[name_start - 1]))
                            name_start--;
                        if (name_start < name_end) {
                            std::string callee(js, name_start, name_end - name_start);
                            // Search for "function <callee>(" in the full JS
                            std::string pattern = "function " + callee + "(";
                            if (js.find(pattern) != std::string::npos) {
                                has_side_effect = true;
                                break;
                            }
                        }
                    }
                    i++;
                }
                if (has_side_effect) {
                    pos = stmt_end + 1;
                    continue;
                }

                // Skip leading whitespace before erasing
                size_t erase_start = id_start;
                while (erase_start > 0 && (js[erase_start - 1] == ' ' || js[erase_start - 1] == ';')) {
                    if (js[erase_start - 1] == ';') {
                        erase_start--;  // grab prior semicolon
                        break;
                    }
                    erase_start--;
                }
                js.erase(erase_start, stmt_end + 1 - erase_start);
                removed++;
                pos = erase_start;
                continue;
            }

            pos = stmt_end + 1;
            continue;
        }

        pos++;
    }

    return removed;
}

// ---- Inline single-use top-level variables ----
// If a top-level variable assignment has a literal RHS and the variable
// is referenced exactly once outside the assignment, replace the reference
// with the literal value and strip the assignment. This enables cascading
// dead-code removal when the now-unused variable gets cleaned up.
//
// e.g., f="World";function d(c){...}d(f); → function d(c){...}d("World");
//       (f had exactly 2 occurrences: the assignment and d(f))
static size_t inline_single_use_variables(std::string& js) {
    std::vector<std::pair<size_t, size_t>> string_ranges;
    scan_string_ranges_in_text(js, string_ranges);

    auto inside_string = [&](size_t pos) {
        if (string_ranges.empty()) return false;
        auto it = std::upper_bound(string_ranges.begin(), string_ranges.end(), pos,
            [](size_t p, const auto& r) { return p < r.first; });
        if (it == string_ranges.begin()) return false;
        --it;
        return pos >= it->first && pos < it->second;
    };

    auto is_ident = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
    };

    auto is_literal_char = [](char c) {
        return c == '"' || c == '\'' || c == '`'
            || std::isdigit(static_cast<unsigned char>(c))
            || c == '-' || c == 't' || c == 'f' || c == 'n';
    };

    auto extract_literal = [&](size_t rhs_start, size_t rhs_end) -> std::string {
        // Trim whitespace
        while (rhs_start < rhs_end && (js[rhs_start] == ' ' || js[rhs_start] == '\t'))
            rhs_start++;
        while (rhs_end > rhs_start && (js[rhs_end-1] == ' ' || js[rhs_end-1] == '\t'))
            rhs_end--;
        if (rhs_start >= rhs_end) return "";

        // String literal: "...", '...', or `...` (simple template, no interpolation)
        if (js[rhs_start] == '"' || js[rhs_start] == '\'' || js[rhs_start] == '`') {
            char q = js[rhs_start];
            size_t p = rhs_start + 1;
            while (p < rhs_end) {
                if (js[p] == '\\') { p += 2; continue; }
                if (js[p] == q) {
                    // Found closing quote — must be the only RHS token
                    if (p + 1 == rhs_end) {
                        return js.substr(rhs_start, p + 1 - rhs_start);
                    }
                    // Allow trailing whitespace only
                    size_t after = p + 1;
                    while (after < rhs_end && (js[after] == ' ' || js[after] == '\t'))
                        after++;
                    if (after == rhs_end)
                        return js.substr(rhs_start, p + 1 - rhs_start);
                    // Expression continues beyond this string literal — compound RHS
                    return "";
                }
                p++;
            }
        }

        // Number: digits, optionally with . and leading -
        {
            size_t p = rhs_start;
            if (js[p] == '-') p++;
            bool has_digit = false;
            while (p < rhs_end && std::isdigit(static_cast<unsigned char>(js[p])))
                { has_digit = true; p++; }
            if (p < rhs_end && js[p] == '.') {
                p++;
                while (p < rhs_end && std::isdigit(static_cast<unsigned char>(js[p])))
                    p++;
            }
            if (has_digit && p == rhs_end)
                return js.substr(rhs_start, rhs_end - rhs_start);
        }

        // Boolean / null / undefined
        std::string_view token(js.data() + rhs_start, rhs_end - rhs_start);
        if (token == "true" || token == "false" || token == "null" || token == "undefined")
            return js.substr(rhs_start, rhs_end - rhs_start);

        return "";  // not a simple literal
    };

    size_t inlined = 0;
    size_t pos = 0;

    // Helper: compute brace depth at a given position
    auto brace_depth_at = [&](size_t at) -> int {
        int d = 0;
        for (size_t i = 0; i < at && i < js.size(); i++) {
            if (inside_string(i)) continue;
            if (js[i] == '{') d++;
            else if (js[i] == '}') { if (d > 0) d--; }
        }
        return d;
    };

    while (pos < js.size()) {
        // Find next '=' at depth 0 or 1 outside strings
        size_t eq_pos = std::string::npos;
        int depth = 0;
        for (size_t i = pos; i < js.size(); i++) {
            if (!inside_string(i)) {
                if (js[i] == '{') depth++;
                else if (js[i] == '}') { if (depth > 0) depth--; }
                else if (js[i] == '=' && depth <= 1) {
                    eq_pos = i;
                    break;
                }
            }
        }
        if (eq_pos == std::string::npos) break;

        // Walk left to find identifier start
        size_t id_end = eq_pos;
        while (id_end > 0 && (js[id_end - 1] == ' ' || js[id_end - 1] == '\t'))
            id_end--;
        if (id_end == 0) { pos = eq_pos + 1; continue; }

        size_t id_start = id_end;
        while (id_start > 0 && is_ident(js[id_start - 1]))
            id_start--;
        if (id_start == id_end) { pos = eq_pos + 1; continue; }

        std::string ident = js.substr(id_start, id_end - id_start);

        // Check left word boundary
        if (id_start > 0 && is_ident(js[id_start - 1])) { pos = eq_pos + 1; continue; }
        // Allow top-level or one level deep (function body)
        int assignment_depth = brace_depth_at(id_start);
        if (assignment_depth > 1) { pos = eq_pos + 1; continue; }

        // Walk right from '=' to find RHS end (semicolon at depth 0)
        size_t rhs_start = eq_pos + 1;
        size_t stmt_end = rhs_start;
        int local_depth = 0;
        while (stmt_end < js.size()) {
            char sc = js[stmt_end];
            if (!inside_string(stmt_end)) {
                if (sc == '{' || sc == '(') local_depth++;
                else if (sc == '}' || sc == ')') {
                    if (local_depth > 0) local_depth--;
                }
                else if (sc == ';' && local_depth == 0) break;
            }
            stmt_end++;
        }
        if (stmt_end >= js.size()) { pos = eq_pos + 1; continue; }

        // Extract the literal value
        std::string literal = extract_literal(rhs_start, stmt_end);
        if (literal.empty()) { pos = stmt_end + 1; continue; }

        // Count total occurrences of ident (word-boundary, outside strings)
        size_t read_pos = std::string::npos;
        size_t count = 0;
        for (size_t i = 0; i < js.size(); ) {
            if (i + ident.size() > js.size()) break;
            if (inside_string(i)) { i++; continue; }
            if (js.compare(i, ident.size(), ident) != 0) { i++; continue; }
            bool left_ok = (i == 0 || !is_ident(js[i - 1]));
            bool right_ok = (i + ident.size() >= js.size() || !is_ident(js[i + ident.size()]));
            if (!left_ok || !right_ok) { i += ident.size(); continue; }

            // Check this isn't part of `ident =` or `ident=` (skip the assignment position)
            size_t after = i + ident.size();
            while (after < js.size() && (js[after] == ' ' || js[after] == '\t')) after++;
            if (after < js.size() && js[after] == '=') {
                // This is the assignment, not a read
                i += ident.size();
                continue;
            }

            // This is a read. Record position if it's the only non-assignment occurrence.
            // Only count reads at the same or deeper scope as the assignment
            int read_depth = brace_depth_at(i);
            if (read_depth < assignment_depth) { i += ident.size(); continue; }

            // This is a read. Record position if it's the only non-assignment occurrence.
            count++;
            if (read_pos == std::string::npos) read_pos = i;
            i += ident.size();
        }

        // We need exactly 1 read occurrence (the assignment is the other)
        if (count != 1) { pos = stmt_end + 1; continue; }

        // Before inlining, check that the read is not inside a for-loop header, etc.
        // (The read should be an expression context, not assignment target)
        // Simple check: read_pos must not be immediately followed by '=' after skipping ws
        {
            size_t after = read_pos + ident.size();
            while (after < js.size() && (js[after] == ' ' || js[after] == '\t')) after++;
            if (after < js.size() && (js[after] == '=' || js[after] == ':')) {
                // Variable is being assigned to, not read — skip
                pos = stmt_end + 1;
                continue;
            }
            // Also check left side — shouldn't be part of 'var' or 'let' or 'const'
            // but we already checked top-level, so this is unlikely
        }

        // Check for function/member context: foo.bar = ident  (not a problem)
        // Just avoid inlining when the read is in a delete statement or similar risk

        // --- Perform the inlining ---
        // Replace the read occurrence with the literal value
        js.replace(read_pos, ident.size(), literal);

        // Adjust positions if replacement shifted things
        size_t shift = literal.size() - ident.size();

        // Remove the assignment statement, including any preceding keyword
        // (const, let, var), whitespace, and semicolon.
        size_t erase_start = id_start;
        // First eat whitespace and semicolons
        while (erase_start > 0 && (js[erase_start - 1] == ' ' || js[erase_start - 1] == ';')) {
            if (js[erase_start - 1] == ';') { erase_start--; break; }
            erase_start--;
        }
        // Then eat the declaration keyword if present: the keyword is immediately
        // before the identifier (possibly with whitespace between). E.g.,
        // "const X=" → erase_start should move past "const ".
        if (erase_start >= 5 && js.compare(erase_start - 5, 5, "const") == 0) {
            erase_start -= 5;
            // Also eat the space(s) between keyword and identifier
            while (erase_start > 0 && js[erase_start - 1] == ' ') erase_start--;
        } else if (erase_start >= 3 && js.compare(erase_start - 3, 3, "let") == 0) {
            erase_start -= 3;
            while (erase_start > 0 && js[erase_start - 1] == ' ') erase_start--;
        } else if (erase_start >= 3 && js.compare(erase_start - 3, 3, "var") == 0) {
            erase_start -= 3;
            while (erase_start > 0 && js[erase_start - 1] == ' ') erase_start--;
        }
        size_t erase_len = (stmt_end + 1) - erase_start;
        if (read_pos > stmt_end) {
            // Read was after the assignment — adjust read_pos for the erase
            read_pos -= erase_len;
        }
        js.erase(erase_start, erase_len);

        inlined++;
        pos = erase_start;
    }

    return inlined;
}

// ---- Single-call function inlining ----
// Finds top-level function declarations called exactly once as a statement
// and inlines the body in place of the call, removing the declaration.
// e.g., function d(c){alert(c)}d("hi") → alert("hi")
// Safety: skips functions using `arguments`, `this`, or recursive calls.
static size_t inline_single_call_functions(std::string& js) {


    // Scan string/template/comment ranges for safe identifier detection
    std::vector<std::pair<size_t, size_t>> string_ranges;
    scan_string_ranges_in_text(js, string_ranges);
    auto inside_string = [&](size_t pos) -> bool {
        for (auto& [s, e] : string_ranges) {
            if (pos >= s && pos < e) return true;
        }
        return false;
    };

    // Helper: skip whitespace forward
    auto skip_ws = [&](size_t pos) -> size_t {
        while (pos < js.size() && (js[pos] == ' ' || js[pos] == '\t' ||
               js[pos] == '\n' || js[pos] == '\r'))
            pos++;
        return pos;
    };

    // Keep rescanning until no more functions are inlined (because positions
    // shift when a function is erased, invalidating stored positions for
    // subsequent functions in the same scan).
    size_t inlined = 0;
    bool any_inlined = false;
    do {
        any_inlined = false;
        string_ranges.clear();
        scan_string_ranges_in_text(js, string_ranges);

        // (Re-)find all function declarations at depth 0
        struct FuncInfo {
            size_t start;       // position of 'function'
            size_t name_start;  // start of name
            size_t name_len;    // length of name
            size_t args_start;  // position of '('
            size_t args_end;    // position after ')'
            size_t body_start;  // position after '{'
            size_t body_end;    // position of closing '}'
            size_t end;         // position after '}'
        };
        std::vector<FuncInfo> funcs;
        size_t pos = 0;
        while (pos < js.size()) {
            // Look for 'function' keyword
            if (pos >= js.size() - 8) break;
            if (js[pos] == 'f' && js[pos+1] == 'u' && js[pos+2] == 'n' &&
                js[pos+3] == 'c' && js[pos+4] == 't' && js[pos+5] == 'i' &&
                js[pos+6] == 'o' && js[pos+7] == 'n') {

                size_t kw_end = pos + 8;
                // Must be preceded by whitespace, ';', '}', or start-of-string
                if (pos > 0 && js[pos-1] != ' ' && js[pos-1] != '\t' &&
                    js[pos-1] != '\n' && js[pos-1] != '\r' &&
                    js[pos-1] != ';' && js[pos-1] != '}' && js[pos-1] != '{') {
                    pos++;
                    continue;
                }
                // Must be followed by whitespace
                if (kw_end >= js.size() || (js[kw_end] != ' ' && js[kw_end] != '\t' &&
                    js[kw_end] != '\n')) {
                    pos = kw_end;
                    continue;
                }

                size_t name_start = skip_ws(kw_end);
                if (name_start >= js.size() || !isalnum(static_cast<unsigned char>(js[name_start])) || js[name_start] == '$') {
                    break;
                }
                // Read function name
                size_t name_end = name_start;
                while (name_end < js.size() && (isalnum(static_cast<unsigned char>(js[name_end])) || js[name_end] == '_' || js[name_end] == '$'))
                    name_end++;

                if (name_end <= name_start) { pos = name_start + 1; continue; }

                std::string fname = js.substr(name_start, name_end - name_start);

                // Look for '('
                size_t args_start = skip_ws(name_end);
                if (args_start >= js.size() || js[args_start] != '(') {
                    pos = name_end;
                    continue;
                }

                // Find matching ')'
                int paren_depth = 1;
                size_t args_end = args_start + 1;
                while (args_end < js.size() && paren_depth > 0) {
                    if (js[args_end] == '(' && !inside_string(args_end)) paren_depth++;
                    else if (js[args_end] == ')' && !inside_string(args_end)) paren_depth--;
                    args_end++;
                }
                if (paren_depth != 0) { pos = args_start + 1; continue; }

                // Look for '{'
                size_t body_start = skip_ws(args_end);
                if (body_start >= js.size() || js[body_start] != '{') {
                    pos = args_end;
                    continue;
                }

                // Find matching '}'
                int brace_depth = 1;
                size_t body_end_p = body_start + 1;
                while (body_end_p < js.size() && brace_depth > 0) {
                    if (js[body_end_p] == '{' && !inside_string(body_end_p)) brace_depth++;
                    else if (js[body_end_p] == '}' && !inside_string(body_end_p)) brace_depth--;
                    body_end_p++;
                }
                if (brace_depth != 0) { pos = body_start + 1; continue; }
                size_t body_end = body_end_p - 1; // points to '}'
                size_t end = body_end_p;

                // Only top-level functions (not nested)
                FuncInfo fi = {pos, name_start, name_end - name_start,
                               args_start, args_end,
                               body_start, body_end, end};
                funcs.push_back(fi);
                pos = end;
            } else {
                pos++;
            }
        }

        if (funcs.empty()) break;

    // For each function, count call sites and check safety
    for (auto& fi : funcs) {
        std::string fname = js.substr(fi.name_start, fi.name_len);

        // Safety checks
        // 0. Verify positions are still valid (string may have been modified
        //    in a previous iteration of the do-while loop)
        if (fi.name_start >= js.size() || fi.name_start + fi.name_len > js.size() ||
            fi.body_start + 1 >= js.size() || fi.body_end > js.size() ||
            fi.body_end <= fi.body_start) {
            continue;
        }

        // 1. Function must not use 'arguments' or 'this'
        std::string body = js.substr(fi.body_start + 1, fi.body_end - fi.body_start - 1);
        {
            bool has_arguments = false;
            size_t bp = 0;
            while (bp < body.size() && !has_arguments) {
                if (bp + 9 <= body.size() && body[bp] == 'a' &&
                    strncmp(&body[bp], "arguments", 9) == 0) {
                    // Check word boundary
                    size_t after = bp + 9;
                    if (after >= body.size() || !isalnum(static_cast<unsigned char>(body[after])) && body[after] != '_' && body[after] != '$')
                        has_arguments = true;
                }
                bp++;
            }
            if (has_arguments) continue;

            bool has_this = false;
            bp = 0;
            while (bp < body.size() && !has_this) {
                if (bp + 4 <= body.size() && body[bp] == 't' &&
                    strncmp(&body[bp], "this", 4) == 0) {
                    size_t before = bp > 0 ? bp - 1 : 0;
                    size_t after = bp + 4;
                    if ((bp == 0 || !isalnum(static_cast<unsigned char>(body[before])) && body[before] != '_' && body[before] != '$') &&
                        (after >= body.size() || !isalnum(static_cast<unsigned char>(body[after])) && body[after] != '_' && body[after] != '$'))
                        has_this = true;
                }
                bp++;
            }
            if (has_this) continue;
        }

        // 2. Count call sites: find `fname(` occurrences outside the function itself
        int call_count = 0;
        size_t call_pos = 0;
        size_t scan_pos = 0;
        while (scan_pos < js.size()) {
            size_t found = js.find(fname, scan_pos);
            if (found == std::string::npos) break;
            // Must be a word boundary before
            if (found > 0 && (isalnum(static_cast<unsigned char>(js[found-1])) ||
                              js[found-1] == '_' || js[found-1] == '$')) {
                scan_pos = found + 1;
                continue;
            }
            // Must be followed by '('
            size_t after = found + fname.size();
            after = skip_ws(after);
            if (after >= js.size() || js[after] != '(') {
                scan_pos = found + 1;
                continue;
            }
            // Skip the function declaration itself (name is within its body range)
            if (found >= fi.start && found < fi.end) {
                scan_pos = found + 1;
                continue;
            }
            // Don't count inside string
            if (inside_string(found)) {
                scan_pos = found + 1;
                continue;
            }
            call_count++;
            call_pos = found;
            scan_pos = found + 1;
        }

        if (call_count != 1) continue;

        // 3. Determine call context: statement call vs expression call
        // A statement call is preceded by ';', '{', '}', '(', or is at start
        size_t stmt_start = call_pos;
        while (stmt_start > 0 && js[stmt_start-1] != ';' && js[stmt_start-1] != '{' &&
               js[stmt_start-1] != '}' && js[stmt_start-1] != '(' &&
               js[stmt_start-1] != '\n' && js[stmt_start-1] != '\r')
            stmt_start--;
        bool is_stmt_call = (stmt_start == 0 ||
                             js[stmt_start-1] == ';' || js[stmt_start-1] == '{' ||
                             js[stmt_start-1] == '}' || js[stmt_start-1] == '(');
         // Override: if there's an = between stmt_start and call_pos, it's an expression call
        for (size_t chk = stmt_start; chk < call_pos; chk++) {
            if (js[chk] == '=' && (chk == 0 || js[chk-1] != '=' && js[chk-1] != '!' &&
                js[chk-1] != '<' && js[chk-1] != '>' && js[chk-1] != '+')) {
                is_stmt_call = false;
                break;
            }
        }

        // For expression calls (e.g., x = foo()), the function must have a return
        if (!is_stmt_call) {
            // Check for a return statement in the function body
            size_t return_pos = std::string::npos;
            size_t return_scan = 0;
            int ret_brace_depth = 0;
            while (return_scan < body.size()) {
                if (body[return_scan] == '{' && !inside_string(fi.body_start + 1 + return_scan)) ret_brace_depth++;
                else if (body[return_scan] == '}' && !inside_string(fi.body_start + 1 + return_scan)) ret_brace_depth--;
                else if (ret_brace_depth == 0 && return_scan + 6 <= body.size() &&
                         body[return_scan] == 'r' && body[return_scan+1] == 'e' &&
                         body[return_scan+2] == 't' && body[return_scan+3] == 'u' &&
                         body[return_scan+4] == 'r' && body[return_scan+5] == 'n') {
                    // Check word boundaries
                    bool before_ok = (return_scan == 0 || !isalnum(static_cast<unsigned char>(body[return_scan-1])) &&
                                      body[return_scan-1] != '_' && body[return_scan-1] != '$');
                    bool after_ok = (return_scan + 6 >= body.size() ||
                                     !isalnum(static_cast<unsigned char>(body[return_scan+6])) &&
                                     body[return_scan+6] != '_' && body[return_scan+6] != '$');
                    if (before_ok && after_ok) {
                        return_pos = return_scan;
                    }
                }
                return_scan++;
            }
            if (return_pos == std::string::npos) continue; // no return — can't inline expression call

            // Extract return value: everything from after 'return' to closing ';' or end
            size_t ret_val_start = return_pos + 6;
            while (ret_val_start < body.size() && (body[ret_val_start] == ' ' || body[ret_val_start] == '\t'))
                ret_val_start++;
            size_t ret_val_end = ret_val_start;
            int semicolon_depth = 0;
            while (ret_val_end < body.size()) {
                if (body[ret_val_end] == '(' || body[ret_val_end] == '[' || body[ret_val_end] == '{') semicolon_depth++;
                else if (body[ret_val_end] == ')' || body[ret_val_end] == ']' || body[ret_val_end] == '}') semicolon_depth--;
                else if (body[ret_val_end] == ';' && semicolon_depth == 0) break;
                ret_val_end++;
            }
            std::string ret_value = body.substr(ret_val_start, ret_val_end - ret_val_start);
            // Trim trailing whitespace
            while (!ret_value.empty() && (ret_value.back() == ' ' || ret_value.back() == '\t'))
                ret_value.pop_back();

            // Extract prefix: everything in the body before the return statement
            std::string prefix = body.substr(0, return_pos);
            // Trim leading/trailing whitespace
            while (!prefix.empty() && (prefix.front() == ' ' || prefix.front() == '\t' ||
                   prefix.front() == '\n' || prefix.front() == '\r'))
                prefix.erase(0, 1);
            while (!prefix.empty() && (prefix.back() == ' ' || prefix.back() == '\t' ||
                   prefix.back() == '\n' || prefix.back() == '\r'))
                prefix.pop_back();

            // Parse parameters and arguments (same as statement path)
            size_t arg_start = call_pos + fname.size();
            arg_start = skip_ws(arg_start);
            if (arg_start >= js.size() || js[arg_start] != '(') continue;
            int p_depth = 1;
            size_t close_paren = arg_start + 1;
            while (close_paren < js.size() && p_depth > 0) {
                if (js[close_paren] == '(' && !inside_string(close_paren)) p_depth++;
                else if (js[close_paren] == ')' && !inside_string(close_paren)) p_depth--;
                close_paren++;
            }
            std::string call_args = js.substr(arg_start + 1, close_paren - arg_start - 2);

            std::string params = js.substr(fi.args_start + 1, fi.args_end - fi.args_start - 2);

            std::vector<std::string> param_list;
            {
                size_t pp = 0, start_p = 0;
                while (pp <= params.size()) {
                    if (pp == params.size() || params[pp] == ',') {
                        std::string p = params.substr(start_p, pp - start_p);
                        size_t ts = 0, te = p.size();
                        while (ts < te && (p[ts] == ' ' || p[ts] == '\t')) ts++;
                        while (te > ts && (p[te-1] == ' ' || p[te-1] == '\t')) te--;
                        p = p.substr(ts, te - ts);
                        size_t eq = p.find('=');
                        if (eq != std::string::npos) p = p.substr(0, eq);
                        ts = 0; te = p.size();
                        while (ts < te && (p[ts] == ' ' || p[ts] == '\t')) ts++;
                        while (te > ts && (p[te-1] == ' ' || p[te-1] == '\t')) te--;
                        if (ts < te) param_list.push_back(p.substr(ts, te - ts));
                        start_p = pp + 1;
                    }
                    pp++;
                }
            }

            std::vector<std::string> arg_list;
            {
                size_t ap = 0, start_a = 0, nest = 0;
                while (ap <= call_args.size()) {
                    if (ap == call_args.size() || (call_args[ap] == ',' && nest == 0)) {
                        std::string a = call_args.substr(start_a, ap - start_a);
                        size_t ts = 0, te = a.size();
                        while (ts < te && (a[ts] == ' ' || a[ts] == '\t')) ts++;
                        while (te > ts && (a[te-1] == ' ' || a[te-1] == '\t')) te--;
                        if (ts < te) arg_list.push_back(a.substr(ts, te - ts));
                        start_a = ap + 1;
                    } else if (call_args[ap] == '(' || call_args[ap] == '[' || call_args[ap] == '{') {
                        nest++;
                    } else if (call_args[ap] == ')' || call_args[ap] == ']' || call_args[ap] == '}') {
                        nest--;
                    }
                    ap++;
                }
            }

            // Substitute parameters in prefix and return value
            std::string inlined_prefix = prefix;
            for (size_t pi = 0; pi < param_list.size() && pi < arg_list.size(); pi++) {
                const std::string& param = param_list[pi];
                const std::string& arg = arg_list[pi];
                size_t rp = 0;
                while (rp < inlined_prefix.size()) {
                    size_t found = inlined_prefix.find(param, rp);
                    if (found == std::string::npos) break;
                    bool before_ok = (found == 0 || !isalnum(static_cast<unsigned char>(inlined_prefix[found-1])) &&
                                      inlined_prefix[found-1] != '_' && inlined_prefix[found-1] != '$');
                    bool after_ok = (found + param.size() >= inlined_prefix.size() ||
                                     (!isalnum(static_cast<unsigned char>(inlined_prefix[found + param.size()])) &&
                                      inlined_prefix[found + param.size()] != '_' &&
                                      inlined_prefix[found + param.size()] != '$'));
                    if (before_ok && after_ok) {
                        inlined_prefix.replace(found, param.size(), arg);
                        rp = found + arg.size();
                    } else {
                        rp = found + 1;
                    }
                }
            }

            std::string inlined_ret = ret_value;
            for (size_t pi = 0; pi < param_list.size() && pi < arg_list.size(); pi++) {
                const std::string& param = param_list[pi];
                const std::string& arg = arg_list[pi];
                size_t rp = 0;
                while (rp < inlined_ret.size()) {
                    size_t found = inlined_ret.find(param, rp);
                    if (found == std::string::npos) break;
                    bool before_ok = (found == 0 || !isalnum(static_cast<unsigned char>(inlined_ret[found-1])) &&
                                      inlined_ret[found-1] != '_' && inlined_ret[found-1] != '$');
                    bool after_ok = (found + param.size() >= inlined_ret.size() ||
                                     (!isalnum(static_cast<unsigned char>(inlined_ret[found + param.size()])) &&
                                      inlined_ret[found + param.size()] != '_' &&
                                      inlined_ret[found + param.size()] != '$'));
                    if (before_ok && after_ok) {
                        inlined_ret.replace(found, param.size(), arg);
                        rp = found + arg.size();
                    } else {
                        rp = found + 1;
                    }
                }
            }

            // Find the enclosing statement start (for prefix insertion)
            size_t stmt_boundary = call_pos;
            while (stmt_boundary > 0 && js[stmt_boundary - 1] != ';' &&
                   js[stmt_boundary - 1] != '{' && js[stmt_boundary - 1] != '}')
                stmt_boundary--;
            if (stmt_boundary > 0 && js[stmt_boundary - 1] == ';')
                stmt_boundary--;  // also consume the ; for clean insertion

            // Erase function declaration first (it's before the call)
            size_t func_erase_start = fi.start;
            while (func_erase_start > 0 &&
                   (js[func_erase_start - 1] == ' ' || js[func_erase_start - 1] == '\t' ||
                    js[func_erase_start - 1] == '\n' || js[func_erase_start - 1] == '\r'))
                func_erase_start--;
            size_t func_erase_end = fi.end;
            while (func_erase_end < js.size() &&
                   (js[func_erase_end] == ' ' || js[func_erase_end] == '\t' ||
                    js[func_erase_end] == '\n' || js[func_erase_end] == '\r'))
                func_erase_end++;
            size_t func_erase_len = func_erase_end - func_erase_start;
            js.erase(func_erase_start, func_erase_len);

            // Shift positions after function erase
            size_t shifted_call_pos = call_pos - func_erase_len;
            size_t shifted_close_paren = close_paren - func_erase_len;
            size_t shifted_stmt = stmt_boundary - func_erase_len;

            // Insert prefix before the statement containing the call
            if (!inlined_prefix.empty()) {
                std::string prefix_stmt = inlined_prefix + ";";
                js.insert(shifted_stmt, prefix_stmt);
                size_t insert_len = prefix_stmt.size();
                shifted_call_pos += insert_len;
                shifted_close_paren += insert_len;
            }

            // Replace call f(...) with return value
            size_t call_len = shifted_close_paren - shifted_call_pos;
            js.replace(shifted_call_pos, call_len, inlined_ret);

            inlined++;
            any_inlined = true;
            break; // re-scan
        }

        // Find the end of the call expression (matching close paren)
        size_t arg_start = call_pos + fname.size();
        arg_start = skip_ws(arg_start);
        if (arg_start >= js.size() || js[arg_start] != '(') continue;
        int p_depth = 1;
        size_t close_paren = arg_start + 1;
        while (close_paren < js.size() && p_depth > 0) {
            if (js[close_paren] == '(' && !inside_string(close_paren)) p_depth++;
            else if (js[close_paren] == ')' && !inside_string(close_paren)) p_depth--;
            close_paren++;
        }

        // Extract call arguments as a single string between '(' and ')'
        std::string call_args = js.substr(arg_start + 1, close_paren - arg_start - 2);

        // Extract function parameters
        std::string params = js.substr(fi.args_start + 1, fi.args_end - fi.args_start - 2);

        // Parse parameters (comma-separated, handling default values)
        std::vector<std::string> param_list;
        {
            size_t pp = 0;
            size_t start_p = 0;
            while (pp <= params.size()) {
                if (pp == params.size() || params[pp] == ',') {
                    std::string p = params.substr(start_p, pp - start_p);
                    // Trim whitespace
                    size_t ts = 0, te = p.size();
                    while (ts < te && (p[ts] == ' ' || p[ts] == '\t')) ts++;
                    while (te > ts && (p[te-1] == ' ' || p[te-1] == '\t')) te--;
                    p = p.substr(ts, te - ts);
                    // Skip default values: a=b → just 'a'
                    size_t eq = p.find('=');
                    if (eq != std::string::npos) p = p.substr(0, eq);
                    // Trim again
                    ts = 0; te = p.size();
                    while (ts < te && (p[ts] == ' ' || p[ts] == '\t')) ts++;
                    while (te > ts && (p[te-1] == ' ' || p[te-1] == '\t')) te--;
                    if (ts < te) param_list.push_back(p.substr(ts, te - ts));
                    start_p = pp + 1;
                }
                pp++;
            }
        }

        // Parse arguments (comma-separated, but respecting nested parens/braces)
        std::vector<std::string> arg_list;
        {
            size_t ap = 0;
            size_t start_a = 0;
            int nest = 0;
            while (ap <= call_args.size()) {
                if (ap == call_args.size() || (call_args[ap] == ',' && nest == 0)) {
                    std::string a = call_args.substr(start_a, ap - start_a);
                    // Trim whitespace
                    size_t ts = 0, te = a.size();
                    while (ts < te && (a[ts] == ' ' || a[ts] == '\t')) ts++;
                    while (te > ts && (a[te-1] == ' ' || a[te-1] == '\t')) te--;
                    if (ts < te) arg_list.push_back(a.substr(ts, te - ts));
                    start_a = ap + 1;
                } else if (call_args[ap] == '(' || call_args[ap] == '[' || call_args[ap] == '{') {
                    nest++;
                } else if (call_args[ap] == ')' || call_args[ap] == ']' || call_args[ap] == '}') {
                    nest--;
                }
                ap++;
            }
        }

        // Build the inlined body: replace parameter names with argument expressions
        std::string inlined_body = body;
        for (size_t pi = 0; pi < param_list.size() && pi < arg_list.size(); pi++) {
            const std::string& param = param_list[pi];
            const std::string& arg = arg_list[pi];

            // Simple string replacement, respecting word boundaries
            size_t rp = 0;
            while (rp < inlined_body.size()) {
                size_t found = inlined_body.find(param, rp);
                if (found == std::string::npos) break;

                // Check word boundary
                bool before_ok = (found == 0 || !isalnum(static_cast<unsigned char>(inlined_body[found-1])) &&
                                  inlined_body[found-1] != '_' && inlined_body[found-1] != '$');
                bool after_ok = (found + param.size() >= inlined_body.size() ||
                                 (!isalnum(static_cast<unsigned char>(inlined_body[found + param.size()])) &&
                                  inlined_body[found + param.size()] != '_' &&
                                  inlined_body[found + param.size()] != '$'));

                if (before_ok && after_ok) {
                    inlined_body.replace(found, param.size(), arg);
                    rp = found + arg.size();
                } else {
                    rp = found + 1;
                }
            }
        }

        // Trim whitespace from inlined body
        while (!inlined_body.empty() && (inlined_body.front() == ' ' || inlined_body.front() == '\t' ||
               inlined_body.front() == '\n' || inlined_body.front() == '\r'))
            inlined_body.erase(0, 1);
        while (!inlined_body.empty() && (inlined_body.back() == ' ' || inlined_body.back() == '\t' ||
               inlined_body.back() == '\n' || inlined_body.back() == '\r' ||
               inlined_body.back() == ';'))
            inlined_body.pop_back();

        if (inlined_body.empty()) {
            // Body became empty - replace the call with nothing
            // Find the semicolon after the call and remove it too
            size_t erase_end = close_paren;
            while (erase_end < js.size() && js[erase_end] != ';' &&
                   js[erase_end] != '\n' && js[erase_end] != '\r')
                erase_end++;
            if (erase_end < js.size() && js[erase_end] == ';') erase_end++;
            js.erase(call_pos, erase_end - call_pos);

            // Also remove the function declaration
            size_t func_erase_end = fi.end;
            // Include trailing whitespace/newline
            while (func_erase_end < js.size() && (js[func_erase_end] == ' ' || js[func_erase_end] == '\n' || js[func_erase_end] == '\r'))
                func_erase_end++;
            // Also remove leading whitespace/semicolons before function
            size_t func_erase_start = fi.start;
            while (func_erase_start > 0 && (js[func_erase_start-1] == ' ' || js[func_erase_start-1] == '\n' || js[func_erase_start-1] == '\r'))
                func_erase_start--;
            js.erase(func_erase_start, func_erase_end - func_erase_start);
            inlined++;
            any_inlined = true;
            break; // positions invalidated, re-scan
        } else {
            // Erase function declaration FIRST (it's before the call since this is a
            // top-level function called after definition).  Then replace the call
            // (shifted by the erase length) with the inlined body.

            // compute erase range for function declaration
            size_t func_erase_start = fi.start;
            while (func_erase_start > 0 &&
                   (js[func_erase_start - 1] == ' ' || js[func_erase_start - 1] == '\t' ||
                    js[func_erase_start - 1] == '\n' || js[func_erase_start - 1] == '\r'))
                func_erase_start--;
            size_t func_erase_end = fi.end;
            while (func_erase_end < js.size() &&
                   (js[func_erase_end] == ' ' || js[func_erase_end] == '\t' ||
                    js[func_erase_end] == '\n' || js[func_erase_end] == '\r'))
                func_erase_end++;

             size_t func_erase_len = func_erase_end - func_erase_start;

             // erase function declaration
             js.erase(func_erase_start, func_erase_len);

            // call_pos has shifted down by func_erase_len (assuming call is after function)
            // but only if func_erase_start < call_pos
            size_t shifted_call_pos;
            if (func_erase_start < call_pos) {
                if (call_pos >= func_erase_end)
                    shifted_call_pos = call_pos - func_erase_len;
                else
                    shifted_call_pos = func_erase_start; // call was inside erased range (shouldn't happen)
            } else {
                shifted_call_pos = call_pos;
            }

            // recompute close_paren position (also shifted)
            size_t shifted_close_paren;
            if (close_paren >= func_erase_end)
                shifted_close_paren = close_paren - func_erase_len;
            else
                shifted_close_paren = func_erase_start;

            // compute erase range for the call (including trailing semicolon)
            size_t call_erase_start = shifted_call_pos;
            size_t call_erase_end = shifted_close_paren;
            while (call_erase_end < js.size() &&
                   (js[call_erase_end] == ' ' || js[call_erase_end] == '\t'))
                call_erase_end++;
            if (call_erase_end < js.size() && js[call_erase_end] == ';')
                call_erase_end++;

            // If the call is in an assignment context (lhs = func()),
            // strip the LHS and = since the function has no return value
            // and the assignment target would become dead code.
            size_t assign_erase = call_erase_start;
            while (assign_erase > 0 && js[assign_erase - 1] == ' ')
                assign_erase--;
            if (assign_erase > 0 && js[assign_erase - 1] == '=') {
                assign_erase--;  // consume =
                // Scan back to statement boundary or identifier
                while (assign_erase > 0 && js[assign_erase - 1] == ' ')
                    assign_erase--;
                while (assign_erase > 0 &&
                       (isalnum(static_cast<unsigned char>(js[assign_erase - 1])) ||
                        js[assign_erase - 1] == '_' || js[assign_erase - 1] == '$'))
                    assign_erase--;
                // Include any preceding var/let/const keyword
                while (assign_erase > 0 && js[assign_erase - 1] == ' ')
                    assign_erase--;
                if (assign_erase >= 3) {
                    std::string kw = js.substr(assign_erase - 3, 3);
                    if (kw == "var" || kw == "let") assign_erase -= 3;
                }
                if (assign_erase >= 5) {
                    std::string kw = js.substr(assign_erase - 5, 5);
                    if (kw == "const") assign_erase -= 5;
                }
                call_erase_start = assign_erase;
            }

             js.replace(call_erase_start, call_erase_end - call_erase_start,
                        inlined_body + ";");
            inlined++;
            any_inlined = true;
            break; // positions invalidated, re-scan

            // Re-scan string ranges since positions shifted
            // (will be redone at top of loop)
        }
    }
    } while (any_inlined);

    return inlined;
}

// ---- Dead function detection after console stripping ----

// ---- Dead function detection after console stripping ----
// After strip_console_calls removes the last caller of a function,
// that function becomes dead but dead_js has already run. This pass
// scans the text for function declarations with no remaining callers
// and strips them. Handles the common case: console.log(fn("arg")).
// Returns the number of functions removed.
static size_t strip_unreferenced_functions(std::string& js) {
    // Pre-scan string ranges so we can skip identifiers inside strings
    std::vector<std::pair<size_t, size_t>> string_ranges;
    scan_string_ranges_in_text(js, string_ranges);

    // Helper: check if a position is inside a string
    auto inside_string = [&](size_t pos) {
        if (string_ranges.empty()) return false;
        auto it = std::upper_bound(string_ranges.begin(), string_ranges.end(), pos,
            [](size_t p, const auto& r) { return p < r.first; });
        if (it == string_ranges.begin()) return false;
        --it;
        return pos >= it->first && pos < it->second;
    };

    // Helper: is this character a valid identifier char?
    auto is_ident = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
    };

    // First pass: collect function declarations and their name positions
    struct FnEntry {
        std::string name;
        size_t fn_pos;  // position of 'function' keyword
        size_t name_pos; // position of the name
        size_t body_open; // position of '{'
        size_t body_close; // position of matching '}'
        size_t end_pos; // one past '}'
    };
    std::vector<FnEntry> functions;

    size_t pos = 0;
    while (pos < js.size()) {
        // Find "function" keyword
        size_t kw = js.find("function", pos);
        if (kw == std::string::npos) break;

        // Make sure it's a keyword, not part of another identifier
        if (kw > 0 && is_ident(js[kw - 1])) { pos = kw + 8; continue; }
        if (kw + 8 < js.size() && is_ident(js[kw + 8])) { pos = kw + 8; continue; }

        // Skip past "function" and any whitespace
        size_t p = kw + 8;
        while (p < js.size() && (js[p] == ' ' || js[p] == '\t' || js[p] == '\n' || js[p] == '\r'))
            p++;

        // Optional '*' for generator function
        if (p < js.size() && js[p] == '*') {
            p++;
            while (p < js.size() && (js[p] == ' ' || js[p] == '\t' || js[p] == '\n' || js[p] == '\r'))
                p++;
        }

        // Must be a named function (not anonymous after function expression context)
        // We only care about function NAME ( ... )
        if (p >= js.size()) { pos = kw + 8; continue; }
        // Skip if anonymous
        if (js[p] == '(' || js[p] == '[') { pos = kw + 8; continue; }

        // Extract the name
        size_t name_start = p;
        while (p < js.size() && is_ident(js[p])) p++;
        if (p == name_start) { pos = kw + 8; continue; }

        std::string fname = js.substr(name_start, p - name_start);

        // Skip past whitespace to '('
        while (p < js.size() && (js[p] == ' ' || js[p] == '\t' || js[p] == '\n' || js[p] == '\r'))
            p++;
        if (p >= js.size() || js[p] != '(') { pos = kw + 8; continue; }

        // Find matching '{'
        p++; // skip '('
        int paren_depth = 1;
        while (p < js.size() && paren_depth > 0) {
            if (js[p] == '(') paren_depth++;
            else if (js[p] == ')') paren_depth--;
            else if (js[p] == '"' || js[p] == '\'' || js[p] == '`') {
                char q = js[p];
                p++;
                while (p < js.size() && js[p] != q) {
                    if (js[p] == '\\') p++;
                    p++;
                }
            }
            p++;
        }
        // Skip whitespace to '{'
        while (p < js.size() && (js[p] == ' ' || js[p] == '\t' || js[p] == '\n' || js[p] == '\r'))
            p++;
        if (p >= js.size() || js[p] != '{') { pos = kw + 8; continue; }

        size_t body_open = p;
        p++;
        int brace_depth = 1;
        while (p < js.size() && brace_depth > 0) {
            if (js[p] == '{') brace_depth++;
            else if (js[p] == '}') brace_depth--;
            else if (js[p] == '"' || js[p] == '\'' || js[p] == '`') {
                char q = js[p];
                p++;
                while (p < js.size() && js[p] != q) {
                    if (js[p] == '\\') p++;
                    p++;
                }
            }
            if (brace_depth > 0) p++;
        }
        if (p >= js.size()) break;

        size_t body_close = p;
        size_t end_pos = p + 1;

        functions.push_back({std::move(fname), kw, name_start, body_open, body_close, end_pos});
        pos = end_pos;
    }

    if (functions.empty()) return 0;

    // Second pass: for each function name, count occurrences in the ENTIRE text
    // (excluding positions inside strings). Names with exactly 1 occurrence
    // (only the declaration name) are dead.
    std::set<std::string> dead_names;
    for (const auto& fn : functions) {
        size_t count = 0;
        size_t sp = 0;
        while ((sp = js.find(fn.name, sp)) != std::string::npos) {
            // Check word boundaries
            bool left_ok = (sp == 0 || !is_ident(js[sp - 1]));
            bool right_ok = (sp + fn.name.size() >= js.size() || !is_ident(js[sp + fn.name.size()]));
            if (left_ok && right_ok && !inside_string(sp)) {
                count++;
            }
            sp += fn.name.size();
        }
        if (count == 1) {
            dead_names.insert(fn.name);
        }
    }

    if (dead_names.empty()) return 0;

    // Third pass: remove dead functions in reverse order
    size_t removed = 0;
    for (auto it = functions.rbegin(); it != functions.rend(); ++it) {
        if (dead_names.count(it->name) == 0) continue;

        // Erase from "function" to after closing brace, plus trailing whitespace/semicolons
        size_t end = it->end_pos;
        while (end < js.size() && (js[end] == ' ' || js[end] == '\t' || js[end] == '\n' || js[end] == '\r' || js[end] == ';'))
            end++;
        js.erase(it->fn_pos, end - it->fn_pos);
        removed++;
    }

    return removed;
}

// ---- Fold array-esque calls to spread syntax ----
// Array.from(expr) → [...expr]  (saves 9 bytes)
// [].slice.call(expr) → [...expr]  (saves 16 bytes)
// Array.prototype.slice.call(expr) → [...expr]
// Skips cases where Array.from has a second argument (mapping function).
// Only transforms when the call has a single argument.
static size_t fold_spread_patterns(std::string& js) {
    std::vector<std::pair<size_t, size_t>> string_ranges;
    scan_string_ranges_in_text(js, string_ranges);

    auto inside_string = [&](size_t pos) {
        if (string_ranges.empty()) return false;
        auto it = std::upper_bound(string_ranges.begin(), string_ranges.end(), pos,
            [](size_t p, const auto& r) { return p < r.first; });
        if (it == string_ranges.begin()) return false;
        --it;
        return pos >= it->first && pos < it->second;
    };

    auto is_ident = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
    };

    // Helper: find matching closing paren from open_pos; returns end pos (one past ')')
    auto find_closing_paren = [&](const std::string& s, size_t open_pos) -> size_t {
        int depth = 1;
        size_t scan = open_pos + 1;
        while (scan < s.size() && depth > 0) {
            if (!inside_string(scan)) {
                if (s[scan] == '(') depth++;
                else if (s[scan] == ')') depth--;
            }
            scan++;
        }
        if (depth != 0) return std::string::npos;
        return scan; // one past closing paren
    };

    // Helper: check if argument list (between parens) has a top-level comma
    auto has_top_level_comma = [&](const std::string& args) -> bool {
        int d = 0;
        for (size_t i = 0; i < args.size(); i++) {
            if (!inside_string(i)) {
                if (args[i] == '(' || args[i] == '[' || args[i] == '{') d++;
                else if (args[i] == ')' || args[i] == ']' || args[i] == '}') d--;
                else if (args[i] == ',' && d == 0) return true;
            }
        }
        return false;
    };

    size_t replacements = 0;

    // --- Pattern 1: Array.from(expr) ---
    {
        const std::string needle = "Array.from(";
        size_t pos = 0;
        while ((pos = js.find(needle, pos)) != std::string::npos) {
            if (inside_string(pos)) { pos += needle.size(); continue; }

            // Verify 'A' is at start of identifier
            if (pos > 0 && is_ident(js[pos - 1])) { pos++; continue; }

            size_t open_pos = pos + needle.size() - 1; // position of '('
            size_t close_end = find_closing_paren(js, open_pos);
            if (close_end == std::string::npos) { pos++; continue; }
            size_t close_pos = close_end - 1;

            // Extract args between parens
            std::string args = js.substr(open_pos + 1, close_pos - open_pos - 1);
            if (has_top_level_comma(args)) { pos = close_end; continue; }

            // Replace Array.from(arg) with [...arg]
            std::string replacement = "[..." + args + "]";
            js.replace(pos, close_end - pos, replacement);
            replacements++;
            pos += replacement.size();
        }
    }

    // --- Pattern 2: [].slice.call(expr) ---
    {
        const std::string needle = "[].slice.call(";
        size_t pos = 0;
        while ((pos = js.find(needle, pos)) != std::string::npos) {
            if (inside_string(pos)) { pos += needle.size(); continue; }

            size_t open_pos = pos + needle.size() - 1;
            size_t close_end = find_closing_paren(js, open_pos);
            if (close_end == std::string::npos) { pos++; continue; }
            size_t close_pos = close_end - 1;

            std::string args = js.substr(open_pos + 1, close_pos - open_pos - 1);
            if (has_top_level_comma(args)) { pos = close_end; continue; }

            std::string replacement = "[..." + args + "]";
            js.replace(pos, close_end - pos, replacement);
            replacements++;
            pos += replacement.size();
        }
    }

    // --- Pattern 3: Array.prototype.slice.call(expr) ---
    {
        const std::string needle = "Array.prototype.slice.call(";
        size_t pos = 0;
        while ((pos = js.find(needle, pos)) != std::string::npos) {
            if (inside_string(pos)) { pos += needle.size(); continue; }

            if (pos > 0 && is_ident(js[pos - 1])) { pos++; continue; }

            size_t open_pos = pos + needle.size() - 1;
            size_t close_end = find_closing_paren(js, open_pos);
            if (close_end == std::string::npos) { pos++; continue; }
            size_t close_pos = close_end - 1;

            std::string args = js.substr(open_pos + 1, close_pos - open_pos - 1);
            if (has_top_level_comma(args)) { pos = close_end; continue; }

            std::string replacement = "[..." + args + "]";
            js.replace(pos, close_end - pos, replacement);
            replacements++;
            pos += replacement.size();
        }
    }

    return replacements;
}

// Strip console.*(…) calls from JS text.
// Matches console.log/warn/error/info/debug/trace/table/time/timeEnd/group/groupEnd/assert/count/countReset/dir.
// Handles balanced parentheses and string literals to correctly find the closing ).
// Returns the number of calls removed.
static size_t strip_console_calls(std::string& js) {
    static const char* methods[] = {
        "log", "warn", "error", "info", "debug", "trace", "table",
        "time", "timeEnd", "group", "groupEnd", "assert", "count", "countReset", "dir"
    };

    size_t removed = 0;
    size_t pos = 0;
    while ((pos = js.find("console.", pos)) != std::string::npos) {
        size_t method_start = pos + 8; // after "console."
        // Find which method, if any
        const char* matched = nullptr;
        size_t matched_len = 0;
        for (const char* m : methods) {
            size_t mlen = strlen(m);
            if (js.compare(method_start, mlen, m) == 0) {
                // Must be followed by ( or whitespace then (
                size_t next = method_start + mlen;
                while (next < js.size() && (js[next] == ' ' || js[next] == '\t' || js[next] == '\n'))
                    next++;
                if (next < js.size() && js[next] == '(') {
                    matched = m;
                    matched_len = mlen;
                    break;
                }
            }
        }
        if (!matched) {
            pos++; // not a console method, skip
            continue;
        }

        // Find the opening ( for this method
        size_t open_paren = method_start + matched_len;
        while (open_paren < js.size() && (js[open_paren] == ' ' || js[open_paren] == '\t' || js[open_paren] == '\n'))
            open_paren++;
        if (open_paren >= js.size() || js[open_paren] != '(') {
            pos++;
            continue;
        }

        // Find matching closing paren (handle nesting and strings)
        int depth = 1;
        size_t close_paren = open_paren + 1;
        bool in_string = false;
        char string_char = 0;
        while (close_paren < js.size() && depth > 0) {
            char c = js[close_paren];
            if (in_string) {
                if (c == '\\') {
                    close_paren++; // skip escaped char
                } else if (c == string_char) {
                    in_string = false;
                }
            } else {
                if (c == '"' || c == '\'' || c == '`') {
                    in_string = true;
                    string_char = c;
                } else if (c == '(') {
                    depth++;
                } else if (c == ')') {
                    depth--;
                }
            }
            close_paren++;
        }

        if (depth != 0) {
            // Unmatched paren — abort removal
            pos++;
            continue;
        }

        // close_paren is one past the closing ). Include trailing semicolon if present.
        size_t end = close_paren;
        while (end < js.size() && (js[end] == ' ' || js[end] == '\t' || js[end] == '\n'))
            end++;
        if (end < js.size() && js[end] == ';')
            end++;

        // Erase from "console." to after the call + optional semicolon
        js.erase(pos, end - pos);
        removed++;
        // Don't advance pos — continue from same position (now holding next text)
    }
    return removed;
}

// Strip function declarations that have empty bodies.
// This catches functions that became empty after strip_console_calls removed
// all body statements (e.g., function greet(){console.log("hi")} → function greet(){}).
// Only removes traditional function declarations; arrow functions and methods are left
// for the existing dead_js pass to handle on the next iteration.
static size_t strip_empty_functions(std::string& js) {
    size_t removed = 0;
    size_t pos = 0;
    while (pos < js.size()) {
        // Find "function" keyword
        size_t fn_pos = js.find("function", pos);
        if (fn_pos == std::string::npos) break;
        // Only match if "function" is at the start or preceded by whitespace/semicolon/newline
        // (avoid matching inside identifiers like "myfunction")
        if (fn_pos > 0 && (std::isalnum(js[fn_pos-1]) || js[fn_pos-1] == '_' || js[fn_pos-1] == '$')) {
            pos = fn_pos + 1;
            continue;
        }
        size_t after_fn = fn_pos + 8; // past "function"
        // Skip whitespace to find function name
        while (after_fn < js.size() && (js[after_fn] == ' ' || js[after_fn] == '\t'))
            after_fn++;
        // Must have a name (not anonymous)
        if (after_fn >= js.size() || !std::isalpha(js[after_fn]) && js[after_fn] != '_' && js[after_fn] != '$') {
            pos = fn_pos + 1;
            continue;
        }
        // Skip name
        while (after_fn < js.size() && (std::isalnum(js[after_fn]) || js[after_fn] == '_' || js[after_fn] == '$'))
            after_fn++;
        // Find opening paren
        while (after_fn < js.size() && js[after_fn] != '(') {
            if (js[after_fn] != ' ' && js[after_fn] != '\t') {
                pos = fn_pos + 1;
                goto next_iter;
            }
            after_fn++;
        }
        // Find matching close paren (handle nesting and strings)
        {
            size_t cp = after_fn + 1;
            int depth = 1;
            bool in_str = false;
            char str_c = 0;
            while (cp < js.size() && depth > 0) {
                char c = js[cp];
                if (in_str) {
                    if (c == '\\') cp++;
                    else if (c == str_c) in_str = false;
                } else {
                    if (c == '"' || c == '\'' || c == '`') { in_str = true; str_c = c; }
                    else if (c == '(') depth++;
                    else if (c == ')') depth--;
                }
                cp++;
            }
            if (depth != 0) { pos = fn_pos + 1; continue; }
            after_fn = cp; // after closing paren
        }
        // Find opening brace
        while (after_fn < js.size() && js[after_fn] != '{') {
            if (js[after_fn] != ' ' && js[after_fn] != '\t' && js[after_fn] != '\n' && js[after_fn] != '\r') {
                pos = fn_pos + 1;
                goto next_iter;
            }
            after_fn++;
        }
        // Find matching close brace (handle strings and nested braces)
        {
            size_t cb = after_fn + 1;
            int depth = 1;
            bool in_str = false;
            char str_c = 0;
            bool body_has_content = false;
            while (cb < js.size() && depth > 0) {
                char c = js[cb];
                if (in_str) {
                    if (c == '\\') cb++;
                    else if (c == str_c) in_str = false;
                } else {
                    if (c == '"' || c == '\'' || c == '`') { in_str = true; str_c = c; }
                    else if (c == '{') depth++;
                    else if (c == '}') depth--;
                    else if (depth == 1 && c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ';') {
                        // Non-whitespace, non-semicolon content in the body → not empty
                        body_has_content = true;
                    }
                }
                cb++;
            }
            if (depth != 0) { pos = fn_pos + 1; continue; }
            if (body_has_content) { pos = fn_pos + 1; continue; }
            // Body is empty — erase from "function" to after closing brace
            size_t end = cb; // one past closing brace
            // Include trailing whitespace/semicolons/newlines
            while (end < js.size() && (js[end] == ' ' || js[end] == '\t' || js[end] == '\n' || js[end] == '\r' || js[end] == ';'))
                end++;
            js.erase(fn_pos, end - fn_pos);
            removed++;
            // Don't advance pos — stay at fn_pos for next iteration
            continue;
        }
        next_iter:
        pos = fn_pos + 1;
    }
    return removed;
}

// Replace all occurrences of old_name with new_name in js, but only outside
// string literals (', ", `) and comments (//, /* */). This prevents
// cross-identifier renames from corrupting string content like "total is ".
// Check whether position pos falls inside any string/template/regex literal range.
// ranges must be sorted by first.
static bool inside_string_range(const std::vector<std::pair<size_t, size_t>>& ranges, size_t pos) {
    if (ranges.empty()) return false;
    auto it = std::upper_bound(ranges.begin(), ranges.end(), pos,
        [](size_t p, const auto& r) { return p < r.first; });
    if (it == ranges.begin()) return false;
    --it;
    return pos >= it->first && pos < it->second;
}

static void safe_rename_identifier(std::string& js, const std::string& old_name,
                                   const std::string& new_name,
                                   const std::vector<std::pair<size_t, size_t>>* string_ranges = nullptr) {
    if (old_name.empty()) return;

    // Single-pass scan: track position in js while walking through comments.
    // String/template/regex literals are handled via the pre-built AST ranges.
    std::string result;
    result.reserve(js.size());

    size_t i = 0;
    while (i < js.size()) {
        // Skip string/template/regex literal interiors using AST ranges.
        // Must come BEFORE comment check to avoid treating // inside strings as comments.
        if (string_ranges && !string_ranges->empty()) {
            auto it = std::upper_bound(string_ranges->begin(), string_ranges->end(), i,
                [](size_t p, const auto& r) { return p < r.first; });
            if (it != string_ranges->begin()) {
                --it;
                if (i >= it->first && i < it->second) {
                    // Inside a string literal — copy the rest of the literal
                    while (i < it->second && i < js.size()) {
                        result += js[i];
                        i++;
                    }
                    continue;
                }
            }
        }

        // Check for comment start
        if (js[i] == '/' && i + 1 < js.size()) {
            if (js[i+1] == '/') {
                // Line comment — copy until end of line
                while (i < js.size()) {
                    result += js[i];
                    if (js[i] == '\n') { i++; break; }
                    i++;
                }
                continue;
            }
            if (js[i+1] == '*') {
                // Block comment — copy until */
                result += js[i];
                i++;
                result += js[i];
                i++;
                while (i + 1 < js.size()) {
                    if (js[i] == '*' && js[i+1] == '/') {
                        result += "*/";
                        i += 2;
                        break;
                    }
                    result += js[i];
                    i++;
                }
                continue;
            }
        }

        // Try to match old_name at current position
        if (js.compare(i, old_name.size(), old_name) == 0) {
            // Only rename if NOT inside a string/template/regex literal
            bool in_str = string_ranges && inside_string_range(*string_ranges, i);
            if (!in_str) {
                bool left_ok = (i == 0 || !is_ident_char(js[i - 1]));
                size_t after = i + old_name.size();
                bool right_ok = (after >= js.size() || !is_ident_char(js[after]));

                if (left_ok && right_ok) {
                    result += new_name;
                    i += old_name.size();
                    continue;
                }
            }
        }

        // Default: copy character
        result += js[i];
        i++;
    }

    js = std::move(result);
}

// Walk AST tree and collect (src_start, src_end) ranges for FUNC_DECL/VAR_DECL
// nodes whose declared name matches an entry in dead_names.
// Uses opt_js text to read names (AST value string_views may dangle).
// Check if a subtree contains a call or new expression (side-effecting).
static bool has_call_side_effect(JSNode* node) {
    if (!node) return false;
    if (node->type == JSNodeType::CALL_EXPR || node->type == JSNodeType::NEW_EXPR)
        return true;
    for (const auto& child : node->children) {
        if (has_call_side_effect(child.get())) return true;
    }
    return false;
}

static void collect_dead_node_ranges(JSNode* node,
                                      const std::unordered_set<std::string>& dead_names,
                                      const std::string& opt_js,
                                      std::vector<std::pair<size_t, size_t>>& ranges) {
    if (!node) return;

    if (node->type == JSNodeType::FUNC_DECL && node->src_end > node->src_start) {
        // Read function name from opt_js via stamped source positions
        // (node->value is a string_view into the freed parsing buffer — may dangle).
        // Skip "function" keyword, then any whitespace/comments to reach the identifier.
        size_t pos = node->src_start + 8; // past "function"
        while (pos < node->src_end && pos < opt_js.size() && !is_ident_start(opt_js[pos]))
            pos++;
        size_t name_start = pos;
        while (pos < node->src_end && pos < opt_js.size() && is_ident_char(opt_js[pos]))
            pos++;
        if (pos > name_start) {
            std::string name = opt_js.substr(name_start, pos - name_start);
            if (dead_names.count(name)) {
                ranges.emplace_back(node->src_start, node->src_end);
            }
        }
    }
    else if (node->type == JSNodeType::VAR_DECL && node->src_end > node->src_start) {
        // Check if any declared IDENTIFIER child is dead.
        // Read names from opt_js via stamped source positions — child->value
        // is a string_view into the freed parsing buffer and may dangle.
        for (const auto& child : node->children) {
            if (child->type == JSNodeType::IDENTIFIER &&
                child->is_declaration &&
                child->src_end > child->src_start &&
                child->src_end <= opt_js.size()) {
                std::string name = opt_js.substr(child->src_start,
                                                 child->src_end - child->src_start);
                if (dead_names.count(name)) {
                    // Check if the initializer has side effects (calls/new).
                    // e.g., var x = greetUser(name); — the call must survive.
                    JSNode* side_init = nullptr;
                    for (size_t ci = 0; ci < node->children.size(); ci++) {
                        if (node->children[ci]->type == JSNodeType::IDENTIFIER &&
                            node->children[ci]->is_declaration) continue;
                        if (has_call_side_effect(node->children[ci].get())) {
                            side_init = node->children[ci].get();
                            break;
                        }
                    }
                    if (side_init) {
                        // Extract the call: remove only the "var x =" prefix,
                        // leaving the call expression as a standalone statement.
                        // e.g., var result = greetUser(name);  →  greetUser(name);
                        if (side_init->src_start > node->src_start) {
                            ranges.emplace_back(node->src_start, side_init->src_start);
                        }
                    } else {
                        ranges.emplace_back(node->src_start, node->src_end);
                    }
                    break;
                }
            }
        }
    }

    for (const auto& child : node->children) {
        collect_dead_node_ranges(child.get(), dead_names, opt_js, ranges);
    }
}

// Walk AST and collect (src_start, src_end) ranges for all string/template/regex literals.
static void collect_string_literal_ranges(JSNode* node,
                                          std::vector<std::pair<size_t, size_t>>& ranges) {
    if (!node) return;
    if ((node->type == JSNodeType::LITERAL || node->type == JSNodeType::TEMPLATE_LITERAL) &&
        node->src_end > node->src_start) {
        ranges.emplace_back(node->src_start, node->src_end);
    }
    for (const auto& child : node->children) {
        collect_string_literal_ranges(child.get(), ranges);
    }
}

// Scan the current opt_js text for string/template literal positions.
// Used after dead-code erasure when AST positions are stale.
static void scan_string_ranges_in_text(const std::string& text,
                                       std::vector<std::pair<size_t, size_t>>& ranges) {
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '"' || c == '\'' || c == '`') {
            char quote = c;
            size_t start = i;
            ++i;
            while (i < text.size()) {
                if (text[i] == '\\' && i + 1 < text.size()) { i += 2; continue; }
                if (text[i] == quote) { ++i; break; }
                ++i;
            }
            if (i <= text.size()) ranges.emplace_back(start, i);
            if (i >= text.size()) break;
            --i; // for loop will ++i
        }
    }
    std::sort(ranges.begin(), ranges.end());
}

// Fold "literal" + expr + "literal" into template literal `literal${expr}literal`
// after renaming is complete so identifiers inside ${} use their final names.
static void fold_template_literals_post_rename(std::string& js) {
    // Matches: quote text quote + ident + quote text quote (same quote type)
    static std::regex tl_re(
        R"((['\"])([^'\"\\]*?)\1\s*\+\s*([a-zA-Z_$][\w.$\[\]?'"]*?)\s*\+\s*(['\"])([^'\"\\]*?)\4)"
    );
    bool changed = true;
    int iters = 0;
    while (changed && iters < 20) {
        changed = false;
        iters++;
        std::smatch m;
        if (std::regex_search(js, m, tl_re)) {
            std::string a = m[2].str();
            std::string b = m[5].str();
            if (a.find('`') == std::string::npos &&
                b.find('`') == std::string::npos) {
                std::string expr = m[3].str();
                if (expr.find('\n') == std::string::npos && expr.size() < 200) {
                    std::string tl = "`" + a + "${" + expr + "}" + b + "`";
                    js = m.prefix().str() + tl + m.suffix().str();
                    changed = true;
                }
            }
        }
    }
}

// ---- JS string-literal replacement for renamed classes/IDs ----
// After the optimization loop renames HTML/CSS class and ID values,
// this updates string literals in optimized JS (getElementById, classList,
// querySelector, setAttribute) to match the new short names.
static void apply_js_string_renames(UnifiedDocument& doc) {
    // Build replacement map: original name → new short name, but only for
    // names that are actually referenced in JS string literals.
    std::unordered_map<std::string, std::string> replace;

    // Use cumulative_rename for the final mapping (accumulated across iterations)
    for (const auto& [orig_name, curr_name] : doc.cumulative_rename) {
        if (orig_name != curr_name) {
            if (doc.js_touched_classes.count(std::string(orig_name)) > 0 ||
                doc.js_touched_ids.count(std::string(orig_name)) > 0) {
                replace[std::string(orig_name)] = curr_name;
            }
        }
    }

    // Also check css_rename_map/js_rename_map (fresh renames from last iteration)
    for (const auto& [orig_name, new_name] : doc.css_rename_map) {
        std::string orig_str(orig_name);
        if (doc.js_touched_classes.count(orig_str) > 0 ||
            doc.js_touched_ids.count(orig_str) > 0) {
            // Only add if not already in cumulative (prefer cumulative)
            if (replace.find(orig_str) == replace.end()) {
                replace[orig_str] = new_name;
            }
        }
    }

    if (replace.empty() || doc.optimized_js.empty()) return;

    for (auto& script : doc.optimized_js) {
        for (const auto& [old_name, new_name] : replace) {
            // --- ID lookups ---
            // getElementById("X")
            for (const char* quote : {"\"", "'"}) {
                std::string search = std::string("getElementById(") + quote + old_name + quote + ")";
                std::string repl   = std::string("getElementById(") + quote + new_name + quote + ")";
                size_t pos = 0;
                while ((pos = script.find(search, pos)) != std::string::npos) {
                    script.replace(pos, search.size(), repl);
                    pos += repl.size();
                }
            }

            // querySelector("#X") / querySelectorAll("#X")
            for (const char* quote : {"\"", "'"}) {
                for (const char* fn : {"querySelector(", "querySelectorAll("}) {
                    std::string search = std::string(fn) + quote + "#" + old_name + quote + ")";
                    std::string repl   = std::string(fn) + quote + "#" + new_name + quote + ")";
                    size_t pos = 0;
                    while ((pos = script.find(search, pos)) != std::string::npos) {
                        script.replace(pos, search.size(), repl);
                        pos += repl.size();
                    }
                }
            }

            // --- Class lookups ---
            // querySelector(".X") / querySelectorAll(".X")
            for (const char* quote : {"\"", "'"}) {
                for (const char* fn : {"querySelector(", "querySelectorAll("}) {
                    std::string search = std::string(fn) + quote + "." + old_name + quote + ")";
                    std::string repl   = std::string(fn) + quote + "." + new_name + quote + ")";
                    size_t pos = 0;
                    while ((pos = script.find(search, pos)) != std::string::npos) {
                        script.replace(pos, search.size(), repl);
                        pos += repl.size();
                    }
                }
            }

            // classList.add("X") / remove / toggle / contains
            for (const char* quote : {"\"", "'"}) {
                for (const char* method : {"add", "remove", "toggle", "contains"}) {
                    std::string search = std::string("classList.") + method + "(" + quote + old_name + quote + ")";
                    std::string repl   = std::string("classList.") + method + "(" + quote + new_name + quote + ")";
                    size_t pos = 0;
                    while ((pos = script.find(search, pos)) != std::string::npos) {
                        script.replace(pos, search.size(), repl);
                        pos += repl.size();
                    }
                }
            }

            // setAttribute("class", "X") / setAttribute("id", "X")
            for (const char* attr : {"class", "id"}) {
                for (const char* quote : {"\"", "'"}) {
                    // With space: setAttribute("class", "X")
                    std::string search = std::string("setAttribute(") + quote + attr + quote + ", " + quote + old_name + quote + ")";
                    std::string repl   = std::string("setAttribute(") + quote + attr + quote + ", " + quote + new_name + quote + ")";
                    size_t pos = 0;
                    while ((pos = script.find(search, pos)) != std::string::npos) {
                        script.replace(pos, search.size(), repl);
                        pos += repl.size();
                    }
                    // No space: setAttribute("class","X") (common in minified output)
                    search = std::string("setAttribute(") + quote + attr + quote + "," + quote + old_name + quote + ")";
                    repl   = std::string("setAttribute(") + quote + attr + quote + "," + quote + new_name + quote + ")";
                    pos = 0;
                    while ((pos = script.find(search, pos)) != std::string::npos) {
                        script.replace(pos, search.size(), repl);
                        pos += repl.size();
                    }
                }
            }
        }
    }
}

Optimizer::Optimizer(const OptimizationConfig& config) : config_(config) {}

bool Optimizer::optimize(UnifiedDocument& doc) {
    bool any_change = false;
    passes_run_ = 0;

    // Fixed-point optimization loop:
    // Repeat all enabled passes until nothing changes or max iterations reached.
    // This is the novel approach — each pass may enable further reductions
    // in a subsequent pass, so we loop until convergence.
    for (int i = 0; i < config_.max_iterations; i++) {
        bool changed_this_iteration = false;
        passes_run_++;

        // Order matters: structure-changing passes first, then cosmetic
        if (config_.enable_dead_css && pass_dead_css(doc))
            changed_this_iteration = true;

        if (config_.enable_dead_js && pass_dead_js(doc))
            changed_this_iteration = true;

        if (config_.enable_cross_identifier && pass_cross_identifier(doc))
            changed_this_iteration = true;

        if (config_.enable_css_shorthand && pass_css_value_fold(doc))
            changed_this_iteration = true;

        if (config_.enable_css_shorthand && pass_css_shorthand(doc))
            changed_this_iteration = true;

        if (config_.enable_css_shorthand && pass_css_math_fold(doc))
            changed_this_iteration = true;

        if (config_.enable_js_constant_fold && pass_js_constant_fold(doc))
            changed_this_iteration = true;

        if (config_.enable_css_minify && pass_css_minify(doc))
            changed_this_iteration = true;

        if (config_.enable_css_minify && pass_css_default_strip(doc))
            changed_this_iteration = true;

        if (config_.enable_css_shorthand && pass_css_dedup_rules(doc))
            changed_this_iteration = true;

        if (config_.enable_html_minify && pass_html_minify(doc))
            changed_this_iteration = true;

        if (config_.enable_js_minify && pass_js_minify(doc))
            changed_this_iteration = true;

        if (!changed_this_iteration) break;
        any_change = true;
    }

    // Obfuscation is a final pass (not part of the iterative loop)
    if (config_.enable_obfuscation) {
        pass_obfuscation(doc);
    }

    // Brotli-aware reordering is a final pass
    if (config_.enable_brotli_reorder) {
        pass_brotli_reorder(doc);
    }

    // ---- Generate optimized inline CSS/JS strings ----
    doc.optimized_css.clear();
    doc.optimized_js.clear();

    // CSS: Serialize all optimized CSS rules (after dead code elimination, renaming, shorthand)
    // For now, all inline stylesheets are merged — serialize the entire ruleset
    std::string all_css = serialize_css(doc.stylesheets());
    if (!all_css.empty()) {
        // Assign to all inline style slots
        for (size_t i = 0; i < doc.inline_styles().size(); i++) {
            doc.optimized_css.push_back(all_css);
        }
    } else {
        // Fallback: CSS parser/serializer didn't produce output.
        // Minify raw CSS text directly (whitespace + comment stripping).
        for (const auto& raw_css : doc.inline_styles()) {
            std::string opt = minify_css_text(raw_css);
            if (!opt.empty()) {
                doc.optimized_css.push_back(std::move(opt));
            }
        }
    }

    // JS: For each inline script: dead-code removal → rename → minification
    for (size_t si = 0; si < doc.inline_scripts().size(); ++si) {
        const auto& raw_js = doc.inline_scripts()[si];
        std::string opt_js = raw_js;

        // --- Dead code removal (AST-based; runs first so positions match opt_js) ---
         if (!doc.dead_js_names.empty()) {
            std::vector<std::pair<size_t, size_t>> dead_ranges;
            if (si < doc.js_script_asts().size() && doc.js_script_asts()[si]) {
                collect_dead_node_ranges(doc.js_script_asts()[si].get(),
                                          doc.dead_js_names, opt_js, dead_ranges);
                if (config_.debug_dead_js) {
                    std::cerr << "--- debug-dead-js: script " << si << " ---\n";
                    std::cerr << "dead_js_names (" << doc.dead_js_names.size() << "): ";
                    for (const auto& name : doc.dead_js_names)
                        std::cerr << name << " ";
                    std::cerr << "\ndead_ranges (" << dead_ranges.size() << "):\n";
                    for (const auto& [s, e] : dead_ranges) {
                        size_t len = std::min(size_t(80), e - s);
                        std::cerr << "  [" << s << ", " << e << "] \"" << opt_js.substr(s, len) << "\"...\n";
                    }
                    std::cerr << "opt_js: \"" << opt_js << "\"\n";
                    // Also dump AST source ranges
                    std::cerr << "--- AST dump ---\n";
                }
                 if (!dead_ranges.empty()) {
                    // Sort descending so erasures don't invalidate later indices
                    std::sort(dead_ranges.begin(), dead_ranges.end(),
                        [](const auto& a, const auto& b) { return a.first > b.first; });
                    for (const auto& [start, end] : dead_ranges) {
                        size_t erase_end = end;
                        while (erase_end < opt_js.size() &&
                               (opt_js[erase_end] == ' ' || opt_js[erase_end] == ';' ||
                                opt_js[erase_end] == '\n' || opt_js[erase_end] == '\r'))
                            erase_end++;
                        size_t erase_start = start;
                        while (erase_start > 0 &&
                               (opt_js[erase_start-1] == ' ' || opt_js[erase_start-1] == '\n' ||
                                opt_js[erase_start-1] == '\r'))
                            erase_start--;
                        opt_js.erase(erase_start, erase_end - erase_start);
                    }
                 }
             }
         }


        // Strip console.*() calls
        strip_console_calls(opt_js);
        // Replace `undefined` with `void 0` (9→6 bytes per occurrence)
        replace_undefined_with_void(opt_js);
        // Replace `Infinity` with `1/0` (8→3 bytes per occurrence)
        replace_infinity_with_division(opt_js);
        // Strip redundant parens after `return` (2 bytes saved per occurrence)
        strip_return_parens(opt_js);
        // Strip redundant else after return/throw/break/continue (4-5 bytes/occ)
        strip_else_after_jump(opt_js);
        // Strip functions that became empty after console call removal
        strip_empty_functions(opt_js);
        // Strip functions that became unreferenced after console stripping
        // (e.g., greet() only called via console.log(greet("World")))
        strip_unreferenced_functions(opt_js);
        // Strip 'var' keyword from top-level declarations (non-strict mode)
        // saves 4 bytes per global var: var x=1; → x=1;
        strip_var_keywords(opt_js);
        // Strip orphan assignments: x=1; where x never appears elsewhere
        strip_orphan_assignments(opt_js);
        // Inline single-use top-level variable references whose RHS is a literal.
        // e.g., f="World";d(f); → d("World"); — the orphan pass then removes f=...;
        inline_single_use_variables(opt_js);
        // Re-run orphan removal: inlining may have left the declaration orphaned
        // (but inline removes it directly, so this is a safety net for edge cases)
        strip_orphan_assignments(opt_js);
        // Inline single-call function declarations: if a top-level function
        // is called exactly once as a statement, replace the call with the
        // body and remove the declaration.
        inline_single_call_functions(opt_js);
        strip_orphan_assignments(opt_js);
        // Re-run function elimination: inlining can orphan previously-referenced functions
        strip_empty_functions(opt_js);
        strip_unreferenced_functions(opt_js);

        // ---- Const literal propagation ----
        // Replace const-bound identifier references with their literal values
        // (e.g., const X=1; foo(X); bar(X) → foo(1); bar(1)).
        // Multi-use consts are not handled by inline_single_use_variables.
        // Re-parses to get a fresh AST/scope tree with correct positions.
        // The cascade loop below will then remove the now-orphaned declarations.
        opt_js = propagate_const_literals(opt_js);

        // ---- Cascading dead-code re-elimination ----
        // After console stripping, function stripping, and var stripping,
        // variables that were only read by now-removed dead code become
        // dead themselves.  Re-parse + re-eliminate until convergence.
        {
            // Lambda to collect unreferenced names from a scope tree.
            // Uses reachability analysis for functions: a function is alive
            // if reachable from global scope or an exported/live function.
            // This correctly handles mutually-recursive dead functions.
            auto collect_dead = [](const JSScope* scope,
                                    std::unordered_set<std::string>& dead,
                                    auto& self) -> void {
                if (!scope) return;

                // Collect all function infos for reachability analysis
                std::unordered_map<std::string, const JSScope::FunctionInfo*> all_fns;
                auto gather_fns = [&all_fns](const JSScope* s, auto& gf) -> void {
                    if (!s) return;
                    for (const auto& [n, f] : s->functions()) all_fns[n] = &f;
                    for (const auto& c : s->children()) gf(c.get(), gf);
                };
                gather_fns(scope, gather_fns);

                // Reachability-based liveness
                std::queue<std::string> work;
                std::unordered_set<std::string> live;
                for (const auto& [name, fn] : all_fns) {
                    if (fn->is_exported || fn->callers.count("")) {
                        live.insert(name);
                        work.push(name);
                    }
                }
                while (!work.empty()) {
                    std::string cur = work.front();
                    work.pop();
                    for (const auto& [name, fn] : all_fns) {
                        if (live.count(name)) continue;
                        if (fn->callers.count(cur)) {
                            live.insert(name);
                            work.push(name);
                        }
                    }
                }

                for (const auto& [name, var] : scope->variables()) {
                    if (var.is_declared && !var.is_referenced && !var.is_exported)
                        dead.insert(name);
                }
                for (const auto& [name, fn] : scope->functions()) {
                    if (!live.count(name))
                        dead.insert(name);
                }
                for (const auto& child : scope->children())
                    self(child.get(), dead, self);
            };

            JSScope fresh_root(JSScope::Kind::GLOBAL);
            JSParser reparser;
            int cascade_round = 0;
            const int MAX_CASCADE = 5;

            while (cascade_round < MAX_CASCADE) {
                auto fresh_ast = reparser.parse(opt_js, &fresh_root);
                if (!fresh_ast) break;

                std::unordered_set<std::string> cascade_dead;
                collect_dead(&fresh_root, cascade_dead, collect_dead);

                // Also collect unreferenced variable names (e.g., const
                // declarations whose references were replaced by const
                // propagation). Only the root scope is checked for variables.
                // No initial count check so we don't skip var-only dead code.
                {
                    std::function<void(const JSScope*)> collect_dead_vars =
                        [&](const JSScope* scope) {
                            if (!scope) return;
                            for (const auto& [name, var] : scope->variables()) {
                                if (var.is_declared && !var.is_referenced) {
                                    cascade_dead.insert(name);
                                }
                            }
                            for (const auto& child : scope->children()) {
                                collect_dead_vars(child.get());
                            }
                        };
                    collect_dead_vars(&fresh_root);
                }

                if (cascade_dead.empty()) break;

                std::vector<std::pair<size_t, size_t>> cascade_ranges;
                collect_dead_node_ranges(fresh_ast.get(), cascade_dead,
                                         opt_js, cascade_ranges);

                if (cascade_ranges.empty()) break;

                // Remove dead ranges (reverse order to preserve positions)
                std::sort(cascade_ranges.begin(), cascade_ranges.end());
                size_t shift = 0;
                for (auto& [start, end] : cascade_ranges) {
                    if (start < shift) start = 0; else start -= shift;
                    if (end < shift) end = 0; else end -= shift;
                    if (end > opt_js.size()) end = opt_js.size();
                    opt_js.erase(start, end - start);
                    shift += end - start;
                }

                // Strip var keywords from any newly-exposed top-level vars
                strip_var_keywords(opt_js);
                strip_orphan_assignments(opt_js);

                // Cascade dead-function elimination: after removing dead
                // variables/functions via AST, re-run text-based function
                // elimination passes — inlining and other passes can orphan
                // functions that still look alive to the scope tree.
                strip_empty_functions(opt_js);
                strip_unreferenced_functions(opt_js);

                // Reset fresh_root for next round (clears stale scope data)
                fresh_root = JSScope(JSScope::Kind::GLOBAL);
                cascade_round++;
            }
        }
        // Fold constant expressions
        opt_js = fold_constants_in_text(opt_js);
        // Fold array-esque calls to spread syntax
        fold_spread_patterns(opt_js);

        // --- String literal detection (scan current opt_js; AST stale after erasure) ---
        std::vector<std::pair<size_t, size_t>> string_ranges;
        scan_string_ranges_in_text(opt_js, string_ranges);

        // --- Apply JS name renames, skipping string/template literals ---
        // Recalculate string_ranges after each rename since positions shift
        for (auto& [old_name_sv, new_name] : doc.js_rename_map) {
            std::string old_name(old_name_sv);
            string_ranges.clear();
            scan_string_ranges_in_text(opt_js, string_ranges);
            safe_rename_identifier(opt_js, old_name, new_name,
                                   string_ranges.empty() ? nullptr : &string_ranges);
        }

        // Template literal folding: must run AFTER renaming so that
        // identifiers inside ${} use their final short names.
        fold_template_literals_post_rename(opt_js);

        // Re-run inlining: template literal folding may have created
        // single-use variable assignments like  x=`...${e}`  that can
        // now be inlined (the template literal is a simple RHS).
        inline_single_use_variables(opt_js);
        strip_orphan_assignments(opt_js);

        // Run single-call function inlining again after renaming
        // (function names may have been shortened)
        inline_single_call_functions(opt_js);
        strip_orphan_assignments(opt_js);

        // JS minification
        opt_js = minify_js_text(opt_js);
        // Strip trailing semicolon from end of script — the last
        // statement in JS doesn't need a terminating semicolon.
        while (!opt_js.empty() && opt_js.back() == ';')
            opt_js.pop_back();
        doc.optimized_js.push_back(std::move(opt_js));
    }

    // ---- Apply JS string-literal renames for touched classes/IDs ----
    // Names referenced in getElementById(), classList, querySelector(), etc.
    // were previously excluded from renaming. Now that HTML/CSS have been
    // renamed, we update the string literals in optimized JS to match.
    apply_js_string_renames(doc);

    return any_change;
}

// Serialization
std::string Optimizer::serialize(const UnifiedDocument& doc) const {
    std::string result;
    result.reserve(65536);

    // Helper to serialize DOM tree
    struct Serializer {
        std::string& out;
        const OptimizationConfig& cfg;
        const UnifiedDocument& doc;
        int style_idx = 0;
        int script_idx = 0;

        void serialize_node(const DOMNode& node, int depth = 0) {
            switch (node.type()) {
            case DOMNode::Type::ELEMENT:
                serialize_element(node, depth);
                break;
            case DOMNode::Type::TEXT:
                serialize_text(node, depth);
                break;
            case DOMNode::Type::COMMENT:
                if (cfg.keep_license_comments) {
                    std::string_view text = node.text();
                    if (text.find("license") != std::string_view::npos ||
                        text.find("License") != std::string_view::npos ||
                        text.find("copyright") != std::string_view::npos ||
                        text.find("@license") != std::string_view::npos) {
                        out += "<!--";
                        out += text;
                        out += "-->";
                    }
                }
                break;
            case DOMNode::Type::DOCTYPE:
                out += "<!doctypehtml>";
                break;
            }
        }

        void serialize_element(const DOMNode& node, int depth) {
            // Skip the synthetic __root__ element
            bool is_root = (node.tag_name() == std::string_view("__root__"));
            // HTML5 optional start+end tags: <html>, <head>, <body>, </html>, </head>, </body>
            bool is_implicit = (node.tag_name() == "html" ||
                               node.tag_name() == "head" ||
                               node.tag_name() == "body");
            // For style/script elements, use optimized content instead of text children
            bool is_style = (node.tag_name() == std::string_view("style"));
            bool is_script = (node.tag_name() == std::string_view("script"));
            // HTML5 optional END tags (but start tag required):
            // li, dt, dd, p, rt, rp, optgroup, option, colgroup,
            // thead, tbody, tfoot, tr, td, th
            // Condition: element is immediately followed by same-type sibling
            // or is the last child of its parent (no more content).
            bool can_omit_end = false;
            if (!is_implicit && !is_root) {
                // HTML5 optional end tag rules per element.
                // Most common case: same-tag sibling or last child.
                // Some elements also omit before different sibling tags.
                static const std::unordered_map<std::string_view, std::vector<std::string_view>> omit_rules = {
                    // Same-tag or last-child only:
                    {"li", {}},
                    {"rt", {}},
                    {"rp", {}},
                    {"thead", {}},
                    {"tbody", {}},
                    {"tfoot", {}},
                    {"tr", {}},
                    {"td", {}},
                    {"th", {}},
                    // dt omits before another dt OR dd:
                    {"dt", {"dd"}},
                    // dd omits before another dd OR dt:
                    {"dd", {"dt"}},
                    // option omits before option or optgroup:
                    {"option", {"optgroup"}},
                    // optgroup omits before another optgroup:
                    {"optgroup", {}},
                    // colgroup omits before colgroup, tbody, or thead:
                    {"colgroup", {"tbody", "thead"}},
                    // caption omits when not followed by more content or when next
                    // element follows (no whitespace/comments in minified output):
                    {"caption", {"thead", "tbody", "tfoot", "tr", "colgroup"}},
                    // p omits before p, or any block-level element (address, article,
                    // aside, blockquote, details, div, dl, fieldset, figcaption,
                    // figure, footer, form, h1-h6, header, hgroup, hr, main, menu,
                    // nav, ol, pre, section, table, ul):
                    {"p", {
                        "address", "article", "aside", "blockquote", "details",
                        "div", "dl", "fieldset", "figcaption", "figure", "footer",
                        "form", "h1", "h2", "h3", "h4", "h5", "h6",
                        "header", "hgroup", "hr", "main", "menu", "nav",
                        "ol", "pre", "section", "table", "ul"
                    }},
                };
                auto it = omit_rules.find(node.tag_name());
                if (it != omit_rules.end()) {
                    const DOMNode* parent = node.parent();
                    if (parent) {
                        const auto& siblings = parent->children();
                        size_t my_idx = 0;
                        for (; my_idx < siblings.size(); ++my_idx) {
                            if (siblings[my_idx].get() == &node) break;
                        }
                        if (my_idx + 1 >= siblings.size()) {
                            // Last child — end tag can be omitted
                            can_omit_end = true;
                        } else {
                            const auto* next = siblings[my_idx + 1].get();
                            if (next && next->type() == DOMNode::Type::ELEMENT) {
                                std::string_view tag = next->tag_name();
                                if (tag == node.tag_name()) {
                                    can_omit_end = true;
                                } else {
                                    // Check cross-tag allowance list
                                    for (auto& allowed : it->second) {
                                        if (tag == allowed) { can_omit_end = true; break; }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            // HTML5 optional START tags: tbody, colgroup
            // (html, head, body are handled as is_implicit above — both start+end omitted)
            bool can_omit_start = false;
            if (!is_implicit && !is_root) {
                if (node.tag_name() == "tbody" || node.tag_name() == "colgroup") {
                    const auto& children = node.children();
                    bool first_child_ok = false;
                    if (!children.empty()) {
                        const auto* fc = children[0].get();
                        first_child_ok = (fc->type() == DOMNode::Type::ELEMENT &&
                            ((node.tag_name() == "tbody" && fc->tag_name() == "tr") ||
                             (node.tag_name() == "colgroup" && fc->tag_name() == "col")));
                    }
                    if (first_child_ok) {
                        const DOMNode* parent = node.parent();
                        if (parent) {
                            const auto& sibs = parent->children();
                            size_t my_idx = 0;
                            for (; my_idx < sibs.size(); ++my_idx) {
                                if (sibs[my_idx].get() == &node) break;
                            }
                            bool preceded_by_omitted = false;
                            if (my_idx > 0) {
                                const auto* prev = sibs[my_idx - 1].get();
                                if (prev && prev->type() == DOMNode::Type::ELEMENT) {
                                    bool prev_in_family = false;
                                    if (node.tag_name() == "tbody") {
                                        prev_in_family = (prev->tag_name() == "tbody" ||
                                                         prev->tag_name() == "thead" ||
                                                         prev->tag_name() == "tfoot");
                                    } else { // colgroup
                                        prev_in_family = (prev->tag_name() == "colgroup");
                                    }
                                    if (prev_in_family) {
                                        // Would prev's end tag be omitted? Same rules as can_omit_end:
                                        // last child of parent, or next sibling has same tag
                                        // prev's next sibling is us (at my_idx)
                                        if (node.tag_name() == prev->tag_name()) {
                                            preceded_by_omitted = true;
                                        } else if (my_idx >= sibs.size()) {
                                            preceded_by_omitted = true;
                                        }
                                    }
                                }
                            }
                            can_omit_start = !preceded_by_omitted;
                        } else {
                            can_omit_start = true;
                        }
                    }
                }
            }
            if (!is_root && !is_implicit && !can_omit_start) {
                // Skip empty <script> elements (all JS was dead/minified away)
                if (is_script && script_idx < (int)doc.optimized_js.size() &&
                    doc.optimized_js[script_idx].empty()) {
                    script_idx++;
                    return;
                }
                out += '<';
                out += node.tag_name();

                for (const auto& attr : node.attrs()) {
                    // Skip default attribute values per HTML spec
                    static const std::vector<std::tuple<std::string_view, std::string_view, std::string_view>> default_attrs = {
                        {"input", "type", "text"},
                        {"script", "type", "text/javascript"},
                        {"form", "method", "get"},
                        {"button", "type", "submit"},
                        {"form", "enctype", "application/x-www-form-urlencoded"},
                    };
                    bool is_default = false;
                    for (const auto& [tag, aname, dval] : default_attrs) {
                        if (node.tag_name() == tag && attr.name == aname && attr.value == dval) {
                            is_default = true;
                            break;
                        }
                    }
                    if (is_default) continue;
                    // Boolean attribute minification: if attr is a known boolean HTML attribute
                    // and has value equal to its name, emit just the bare attribute name.
                    static const std::vector<std::string_view> bool_attrs = {
                        "checked", "disabled", "readonly", "selected", "multiple", "required",
                        "autoplay", "controls", "loop", "muted", "default",
                        "async", "defer", "novalidate", "formnovalidate",
                        "hidden", "open", "reversed", "ismap", "itemscope",
                        "allowfullscreen", "nomodule", "playsinline"
                    };
                    bool is_boolean = false;
                    for (auto ba : bool_attrs) {
                        if (attr.name == ba) {
                            is_boolean = true;
                            break;
                        }
                    }
                    if (is_boolean && attr.has_quotes && attr.name == attr.value) {
                        out += ' ';
                        out += attr.name;
                        continue;
                    }
                    out += ' ';
                    out += attr.name;
                    if (attr.has_quotes) {
                        // Safe unquoted value check: per HTML spec, unquoted
                        // values cannot contain whitespace, quotes, =, <, >, or backtick.
                        bool safe_unquoted = !attr.value.empty();
                        if (safe_unquoted) {
                            for (char c : attr.value) {
                                if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                                    c == '"' || c == '\'' || c == '=' || c == '<' || c == '>' || c == '`') {
                                    safe_unquoted = false;
                                    break;
                                }
                            }
                        }
                        // Optimize viewport meta content: fold numeric values like "1.0" -> "1"
                        std::string_view opt_val = attr.value;
                        std::string opt_buf;

                        // Strip unnecessary quotes from font-family in inline style attributes
                        if (attr.name == "style") {
                            opt_buf = strip_font_family_quotes(std::string(attr.value));
                            opt_val = opt_buf;
                        }

                        if (node.tag_name() == "meta" && attr.name == "content") {
                            // Check if this is a viewport meta
                            bool is_viewport = false;
                            for (const auto& a : node.attrs()) {
                                if (a.name == "name" && a.value == "viewport") {
                                    is_viewport = true;
                                    break;
                                }
                            }
                            if (is_viewport) {
                                // Fold trailing zeros in numeric values: "1.0" -> "1", "1.50" -> "1.5"
                                opt_buf = attr.value;
                                size_t pos = 0;
                                while (pos < opt_buf.size()) {
                                    if (is_digit(opt_buf[pos]) && (pos == 0 || !is_ident_char(opt_buf[pos-1]))) {
                                        size_t dstart = pos;
                                        while (pos < opt_buf.size() && is_digit(opt_buf[pos])) pos++;
                                        if (pos < opt_buf.size() && opt_buf[pos] == '.') {
                                            size_t dot = pos;
                                            pos++;
                                            size_t frac_start = pos;
                                            while (pos < opt_buf.size() && is_digit(opt_buf[pos])) pos++;
                                            // Strip trailing zeros from fractional part
                                            size_t frac_end = pos;
                                            while (frac_end > frac_start && opt_buf[frac_end - 1] == '0') frac_end--;
                                            if (frac_end == frac_start) {
                                                // All fractional digits are zero -> remove dot and fractional part
                                                opt_buf.erase(dot, pos - dot);
                                                pos = dot;
                                            } else if (frac_end < pos) {
                                                // Some trailing zeros removed
                                                opt_buf.erase(frac_end, pos - frac_end);
                                                pos = frac_end;
                                            }
                                        }
                                    } else {
                                        pos++;
                                    }
                                }
                                // Remove spaces after commas in viewport content:
                                // "width=device-width, initial-scale=1" -> "width=device-width,initial-scale=1"
                                for (size_t i = 1; i < opt_buf.size(); ) {
                                    if (opt_buf[i] == ' ' && opt_buf[i-1] == ',') {
                                        opt_buf.erase(i, 1);
                                    } else {
                                        i++;
                                    }
                                }
                                opt_val = opt_buf;
                            }
                        }
                        if (safe_unquoted) {
                            out += '=';
                            out += opt_val;
                        } else {
                            out += "=\"";
                            out += opt_val;
                            out += '"';
                        }
                    } else if (!attr.value.empty()) {
                        out += '=';
                        out += attr.value;
                    }
                    // Boolean attributes (no value, no has_quotes) just have name
                }

                // Void elements don't need closing tags
                bool is_void = (node.tag_name() == "area" || node.tag_name() == "base" ||
                               node.tag_name() == "br" || node.tag_name() == "col" ||
                               node.tag_name() == "embed" || node.tag_name() == "hr" ||
                               node.tag_name() == "img" || node.tag_name() == "input" ||
                               node.tag_name() == "link" || node.tag_name() == "meta" ||
                               node.tag_name() == "param" || node.tag_name() == "source" ||
                               node.tag_name() == "track" || node.tag_name() == "wbr");

                if (is_void) {
                    out += '>';
                    return;
                }

                // For style/script elements, use optimized content instead of text children

                if (is_style && style_idx < (int)doc.optimized_css.size()) {
                    out += '>';
                    out += doc.optimized_css[style_idx++];
                    out += "</style>";
                    return;
                }
                if (is_script && script_idx < (int)doc.optimized_js.size()) {
                    out += '>';
                    out += doc.optimized_js[script_idx++];
                    out += "</script>";
                    return;
                }

                out += '>';
            }

            for (const auto& child : node.children()) {
                serialize_node(*child, is_root ? depth : depth + 1);
            }

            if (!is_root && !is_implicit && !can_omit_end) {
                out += "</";
                out += node.tag_name();
                out += '>';
            }
        }

        void serialize_text(const DOMNode& node, int depth) {
            std::string_view text = node.text();
            out += text;
        }
    };

    Serializer ser{result, config_, doc};
    if (doc.root()) {
        ser.serialize_node(*doc.root());
    }

    // For standalone CSS/JS output, serialize the optimized content directly
    if (!doc.optimized_css.empty() && (!doc.root() || doc.root()->children().empty())) {
        // No DOM output needed — serialize CSS directly
        result = serialize_css(doc.stylesheets());
    }
    if (!doc.optimized_js.empty() && (!doc.root() || doc.root()->children().empty())) {
        // No DOM output needed — serialize JS directly
        // For JS, we need to minify the stored scripts
        // This is handled in the optimize() pass already
        // Just output the optimized JS
    }

    return result;
}

} // namespace tinyizer
