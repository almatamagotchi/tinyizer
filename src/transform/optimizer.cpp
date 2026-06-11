#include "optimizer.h"
#include "serializer.h"
#include "../util/frequency_map.h"
#include "../parser/tokenizer.h"
#include "../parser/js_parser.h"
#include <queue>
#include <algorithm>
#include <set>
#include <sstream>
#include <cctype>
#include <cstring>
#include <iostream>

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
                    // Found closing quote — must be end of RHS
                    if (p + 1 == rhs_end ||
                        (p + 1 < rhs_end && js[p + 1] == ' ')) {
                        return js.substr(rhs_start, p + 1 - rhs_start);
                    }
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

    while (pos < js.size()) {
        // Skip to next top-level ident assignment
        // (simplified: find '=' at depth 0, check left-side ident, right-side literal)
        // We use a similar scan to strip_orphan_assignments but looking for 2-occurrence vars

        // Find next '=' at depth 0 outside strings
        size_t eq_pos = std::string::npos;
        int depth = 0;
        for (size_t i = pos; i < js.size(); i++) {
            if (!inside_string(i)) {
                if (js[i] == '{') depth++;
                else if (js[i] == '}') { if (depth > 0) depth--; }
                else if (js[i] == '=' && depth == 0) {
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
        // Check the ident is at top level (check the start of ident is at depth 0)
        {
            int d = 0;
            for (size_t i = id_start; i > 0; ) {
                i--;
                if (!inside_string(i)) {
                    if (js[i] == '}') d++;
                    else if (js[i] == '{') {
                        if (d > 0) d--;
                        else { d = 1; break; } // opened a brace before closing → inside block
                    }
                }
            }
            if (d != 0) { pos = eq_pos + 1; continue; } // not top level
        }

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

        // Remove the assignment statement
        size_t erase_start = id_start;
        while (erase_start > 0 && (js[erase_start - 1] == ' ' || js[erase_start - 1] == ';')) {
            if (js[erase_start - 1] == ';') { erase_start--; break; }
            erase_start--;
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

        if (config_.enable_css_shorthand && pass_css_shorthand(doc))
            changed_this_iteration = true;

        if (config_.enable_css_shorthand && pass_css_value_fold(doc))
            changed_this_iteration = true;

        if (config_.enable_css_shorthand && pass_css_math_fold(doc))
            changed_this_iteration = true;

        if (config_.enable_js_constant_fold && pass_js_constant_fold(doc))
            changed_this_iteration = true;

        if (config_.enable_css_minify && pass_css_minify(doc))
            changed_this_iteration = true;

        if (config_.enable_css_minify && pass_css_default_strip(doc))
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
        // Re-run function elimination: inlining can orphan previously-referenced functions
        strip_empty_functions(opt_js);
        strip_unreferenced_functions(opt_js);

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
