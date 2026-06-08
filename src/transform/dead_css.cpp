#include "optimizer.h"
#include "../parser/tokenizer.h"
#include <unordered_set>
#include <algorithm>
#include <cstring>

namespace tinyizer {

// Dead CSS elimination via cascade simulation.
//
// Algorithm:
// 1. Collect all class names used in the DOM (from HTML class attributes).
// 2. Collect all class names used in JS (via classList.add/remove/toggle,
//    className mutation, setAttribute('class', ...)) — store in doc.js_touched_classes.
// 3. For each CSS rule, check if any of its selectors could match
//    any element in the DOM tree, accounting for cumulative renames.
// 4. If a rule has NO matching selectors, it's dead code — remove it.
// 5. Re-run after other passes because they may expose new dead rules.
//
// This is conservative: if we can't prove a rule doesn't match, we keep it.
// Pseudo-classes (:hover, :focus, :nth-child, etc.) are assumed to match.

// Scan JS text for IDs used in element lookups.
// getElementById("foo"), querySelector("#bar"), etc.
static void collect_js_ids(const std::vector<std::string>& scripts,
                           std::unordered_set<std::string>& ids) {
    for (const auto& script : scripts) {
        const char* p = script.data();
        const char* end = p + script.size();

        // Scan for getElementById('...') and getElementById("...")
        while (p < end) {
            const char* gebi = std::strstr(p, "getElementById(");
            if (!gebi) break;
            const char* args = gebi + 15;
            while (args < end && (*args == ' ' || *args == '\t')) args++;
            if (args < end && (*args == '\'' || *args == '"')) {
                char quote = *args;
                const char* val_start = args + 1;
                const char* val_end = val_start;
                while (val_end < end && *val_end != quote && *val_end != '\n') val_end++;
                if (val_end < end && *val_end == quote && val_end > val_start) {
                    ids.emplace(val_start, val_end - val_start);
                }
            }
            p = gebi + 1;
        }

        // Scan for querySelector("#...") and querySelectorAll("#...")
        p = script.data();
        while (p < end) {
            const char* qs = std::strstr(p, "querySelector");
            if (!qs) break;
            const char* rest = qs + 13;
            const char* open = nullptr;
            for (const char* q = rest; q < end && q - rest < 20; q++) {
                if (*q == '(') { open = q; break; }
            }
            if (!open) { p = qs + 1; continue; }
            const char* args = open + 1;
            while (args < end && (*args == ' ' || *args == '\t')) args++;
            if (args < end && (*args == '\'' || *args == '"')) {
                char quote = *args;
                const char* val_start = args + 1;
                const char* val_end = val_start;
                // Find the #id within the selector string
                while (val_end < end && *val_end != quote && *val_end != '\n') {
                    if (*val_end == '#') {
                        const char* id_start = val_end + 1;
                        const char* id_end = id_start;
                        while (id_end < end && *id_end != quote && *id_end != '\n' &&
                               *id_end != '.' && *id_end != '#' && *id_end != ' ' &&
                               *id_end != '>' && *id_end != '+' && *id_end != '~' &&
                               *id_end != ':') id_end++;
                        if (id_end > id_start) {
                            ids.emplace(id_start, id_end - id_start);
                        }
                    }
                    val_end++;
                }
            }
            p = qs + 1;
        }
    }
}

// Scan JS text for class names used via DOM manipulation APIs.
// Handles the raw (pre-minified) JS source.
static void collect_js_classes(const std::vector<std::string>& scripts,
                               std::unordered_set<std::string>& classes) {
    for (const auto& script : scripts) {
        const char* p = script.data();
        const char* end = p + script.size();

        while (p < end) {
            const char* cls = std::strstr(p, "classList.");
            if (!cls) break;

            const char* method = cls + 10;
            const char* open_paren = nullptr;
            for (const char* q = method; q < end; q++) {
                if (*q == '(') { open_paren = q; break; }
                if (*q == ';' || *q == '\n') break;
            }
            if (!open_paren) { p = cls + 1; continue; }

            std::string_view method_name(method, open_paren - method);
            if (method_name != "add" && method_name != "remove" && method_name != "toggle") {
                p = cls + 1; continue;
            }

            const char* arg = open_paren + 1;
            while (arg < end && *arg != ')' && *arg != ';') {
                while (arg < end && (*arg == ' ' || *arg == '\t' || *arg == '\n')) arg++;
                if (arg >= end || *arg == ')' || *arg == ';') break;

                char quote = 0;
                if (*arg == '\'' || *arg == '"') {
                    quote = *arg;
                    const char* val_start = arg + 1;
                    const char* val_end = val_start;
                    while (val_end < end && *val_end != quote) val_end++;
                    if (val_end < end && val_end > val_start) {
                        classes.emplace(val_start, val_end - val_start);
                    }
                    arg = val_end + 1;
                } else {
                    while (arg < end && *arg != ',' && *arg != ')') arg++;
                }
                if (arg < end && *arg == ',') arg++;
            }
            p = cls + 1;
        }

        // Scan for className = '...' or className += '...'
        p = script.data();
        while (p < end) {
            const char* cn = std::strstr(p, "className");
            if (!cn) break;

            const char* op = cn + 9;
            while (op < end && (*op == ' ' || *op == '\t')) op++;
            if (op >= end || (*op != '=' && !(op[0] == '+' && op[1] == '='))) {
                p = cn + 1; continue;
            }

            const char* val = (*op == '+') ? op + 2 : op + 1;
            while (val < end && (*val == ' ' || *val == '\t')) val++;

            if (val < end && (*val == '\'' || *val == '"')) {
                char quote = *val;
                const char* val_start = val + 1;
                const char* val_end = val_start;
                while (val_end < end && *val_end != quote) val_end++;
                if (val_end < end && val_end > val_start) {
                    std::string_view token(val_start, val_end - val_start);
                    size_t pos = 0;
                    while (pos < token.size()) {
                        while (pos < token.size() && token[pos] == ' ') pos++;
                        size_t tok_end = pos;
                        while (tok_end < token.size() && token[tok_end] != ' ') tok_end++;
                        if (tok_end > pos)
                            classes.emplace(token.substr(pos, tok_end - pos));
                        pos = tok_end;
                    }
                }
            }
            p = cn + 1;
        }

        // Scan for setAttribute('class', '...')
        p = script.data();
        while (p < end) {
            const char* sa = std::strstr(p, "setAttribute(");
            if (!sa) break;

            const char* args = sa + 13;
            char q1 = 0;
            const char* a1_start = nullptr, *a1_end = nullptr;
            for (const char* q = args; q < end && q - args < 40; q++) {
                if (*q == '\'' || *q == '"') {
                    q1 = *q;
                    a1_start = q + 1;
                    a1_end = a1_start;
                    while (a1_end < end && *a1_end != q1) a1_end++;
                    break;
                }
            }
            if (!a1_start || a1_end >= end) { p = sa + 1; continue; }

            std::string_view attr1(a1_start, a1_end - a1_start);
            if (attr1 != "class") { p = sa + 1; continue; }

            const char* after_q1 = a1_end + 1;
            char q2 = 0;
            const char* a2_start = nullptr, *a2_end = nullptr;
            for (const char* q = after_q1; q < end && q - after_q1 < 40; q++) {
                if (*q == '\'' || *q == '"') {
                    q2 = *q;
                    a2_start = q + 1;
                    a2_end = a2_start;
                    while (a2_end < end && *a2_end != q2) a2_end++;
                    break;
                }
            }
            if (!a2_start || a2_end >= end) { p = sa + 1; continue; }

            classes.emplace(a2_start, a2_end - a2_start);
            p = sa + 1;
        }
    }
}

// Check if a CSS class selector name matches, accounting for cumulative renames.
// For example: after cross_identifier renamed "added" → "e", the CSS selector is "e".
// We check: is "e" in dom_classes? Is the selector directly in js_touched?
// Or was "e" renamed from a class that JS touches?
static bool class_is_used(std::string_view sel_class,
                          const std::unordered_set<std::string_view>& dom_classes,
                          const std::unordered_set<std::string>& js_touched,
                          const std::unordered_map<std::string, std::string>& cumulative_rename) {
    if (dom_classes.count(sel_class) > 0) return true;

    // Direct JS use (before any rename)
    if (js_touched.count(std::string(sel_class)) > 0) return true;

    // Cumulative rename: if this selector was renamed from a JS-touched class
    for (const auto& [orig, curr] : cumulative_rename) {
        if (curr == sel_class && js_touched.count(orig) > 0) {
            return true;
        }
    }
    return false;
}

bool Optimizer::pass_dead_css(UnifiedDocument& doc) {
    if (!doc.root()) return false;

    // Collect JS-touched classes (original names, stored once)
    if (doc.js_touched_classes.empty()) {
        collect_js_classes(doc.inline_scripts(), doc.js_touched_classes);
    }
    if (doc.js_touched_ids.empty()) {
        collect_js_ids(doc.inline_scripts(), doc.js_touched_ids);
    }

    // Build DOM element sets
    std::unordered_set<std::string_view> dom_tags;
    std::unordered_set<std::string_view> dom_ids;
    std::unordered_set<std::string_view> dom_classes;
    std::unordered_set<std::string_view> dom_attrs;

    doc.root()->walk([&](const DOMNode& node) {
        if (node.type() != DOMNode::Type::ELEMENT) return;

        dom_tags.insert(node.tag_name());

        for (const auto& attr : node.attrs()) {
            dom_attrs.insert(attr.name);

            if (attr.name == std::string_view("id")) {
                dom_ids.insert(attr.value);
            } else if (attr.name == std::string_view("class")) {
                std::string_view val = attr.value;
                size_t pos = 0;
                while (pos < val.size()) {
                    while (pos < val.size() && is_whitespace(val[pos])) pos++;
                    size_t end = pos;
                    while (end < val.size() && !is_whitespace(val[end])) end++;
                    if (end > pos) {
                        dom_classes.insert(val.substr(pos, end - pos));
                    }
                    pos = end;
                }
            }
        }
    });

    auto& rules = const_cast<std::vector<CSSRule>&>(doc.stylesheets());
    size_t removed = 0;

    size_t i = 0;
    while (i < rules.size()) {
        auto& rule = rules[i];
        bool has_match = false;

        for (const auto& sel : rule.selectors()) {
            if (sel.empty()) continue;

            const auto& last = sel.back();

            switch (last.type) {
            case CSSRule::SelectorPart::Type::ELEMENT:
                if (dom_tags.count(last.value) > 0) has_match = true;
                break;
            case CSSRule::SelectorPart::Type::CLASS:
                if (class_is_used(last.value, dom_classes,
                                  doc.js_touched_classes, doc.cumulative_rename))
                    has_match = true;
                break;
            case CSSRule::SelectorPart::Type::ID:
                if (dom_ids.count(last.value) > 0) has_match = true;
                break;
            case CSSRule::SelectorPart::Type::ATTR:
                if (dom_attrs.count(last.value) > 0) has_match = true;
                break;
            case CSSRule::SelectorPart::Type::UNIVERSAL:
                has_match = true;
                break;
            case CSSRule::SelectorPart::Type::PSEUDO:
                has_match = true;
                break;
            case CSSRule::SelectorPart::Type::COMBINATOR:
                has_match = true;
                break;
            }

            if (has_match) break;
        }

        if (rule.is_at_rule()) {
            has_match = true;
        }

        if (!has_match) {
            rules.erase(rules.begin() + i);
            removed++;
        } else {
            i++;
        }
    }

    if (removed > 0) {
        doc.set_total_minified_bytes(doc.total_minified_bytes() + removed * 10);
        return true;
    }
    return false;
}

} // namespace tinyizer
