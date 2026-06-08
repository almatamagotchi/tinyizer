#include "optimizer.h"
#include "serializer.h"
#include "../util/frequency_map.h"
#include "../parser/tokenizer.h"
#include <algorithm>
#include <sstream>
#include <cctype>
#include <cstring>
#include <iostream>

namespace tinyizer {

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

        if (config_.enable_js_constant_fold && pass_js_constant_fold(doc))
            changed_this_iteration = true;

        if (config_.enable_css_minify && pass_css_minify(doc))
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
        doc.optimized_js.push_back(std::move(opt_js));
    }

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
            // HTML5 optional tags: <html>, <head>, <body>, </html>, </head>, </body>
            bool is_implicit = (node.tag_name() == "html" ||
                               node.tag_name() == "head" ||
                               node.tag_name() == "body");
            if (!is_root && !is_implicit) {
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
                        if (safe_unquoted) {
                            out += '=';
                            out += attr.value;
                        } else {
                            out += "=\"";
                            out += attr.value;
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
                bool is_style = (node.tag_name() == std::string_view("style"));
                bool is_script = (node.tag_name() == std::string_view("script"));

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

            if (!is_root && !is_implicit) {
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
