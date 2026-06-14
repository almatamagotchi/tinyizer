#include "optimizer.h"
#include <algorithm>
#include <functional>

namespace tinyizer {

// CSS minification: value-level optimizations that happen during serialization.
// This pass handles things best done with a CSS-aware view.

bool Optimizer::pass_css_minify(UnifiedDocument& doc) {
    bool changed = false;

    std::function<void(std::vector<CSSRule>&)> minify_rules;
    minify_rules = [&](std::vector<CSSRule>& rules) {
    for (auto& rule : rules) {
        for (auto& decl : const_cast<std::vector<CSSRule::Declaration>&>(rule.declarations())) {
            // Minify zero values: 0px -> 0, 0em -> 0, etc.
            std::string_view val = decl.value;

            // Color minification
            if (decl.property == std::string_view("color") ||
                decl.property == std::string_view("background-color") ||
                decl.property == std::string_view("border-color") ||
                decl.property == std::string_view("outline-color")) {

                // Try to shorten hex colors
                if (val.size() == 7 && val[0] == '#') {
                    if (val[1] == val[2] && val[3] == val[4] && val[5] == val[6]) {
                        // #rrggbb -> #rgb
                        char shortened[5] = {'#', val[1], val[3], val[5], '\0'};
                        decl.value = doc.string_pool().intern(std::string_view(shortened, 4));
                        changed = true;
                    }
                }

                // Named color shortening: e.g., #ff0000 -> red
                // (large table — could implement for common colors)
            }

            // Minify font-weight: "bold" -> "700", "normal" -> "400"
            if (decl.property == std::string_view("font-weight")) {
                if (val == "bold") {
                    decl.value = doc.string_pool().intern("700");
                    changed = true;
                } else if (val == "normal") {
                    decl.value = doc.string_pool().intern("400");
                    changed = true;
                }
            }

            // Minify font-stretch keywords to percentage equivalents (CSS Fonts 4).
            // Percentage values are shorter than keywords in all cases.
            // e.g., "condensed" -> "75%", "expanded" -> "125%"
            if (decl.property == std::string_view("font-stretch")) {
                static const std::unordered_map<std::string_view, std::string_view> stretch = {
                    {"ultra-condensed", "50%"}, {"extra-condensed", "62.5%"},
                    {"condensed", "75%"},       {"semi-condensed", "87.5%"},
                    {"semi-expanded", "112.5%"},{"expanded", "125%"},
                    {"extra-expanded", "150%"}, {"ultra-expanded", "200%"},
                };
                auto it = stretch.find(val);
                if (it != stretch.end()) {
                    decl.value = doc.string_pool().intern(it->second);
                    changed = true;
                }
            }

            // Strip unnecessary quotes from font-family names
            // "Arial" → Arial, 'Georgia' → Georgia (saves 2 bytes)
            // Safe when the name contains no special characters and isn't a CSS keyword
            if (decl.property == std::string_view("font-family")) {
                std::string_view v = decl.value;
                if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) {
                    char quote = v.front();
                    if (v.back() == quote) {
                        std::string_view inner(v.data() + 1, v.size() - 2);
                        // Don't strip if: empty, contains whitespace/commas/slashes,
                        // starts with digit/hyphen-digit, or is a CSS generic family.
                        bool safe = !inner.empty();
                        for (char c : inner) {
                            if (c == ' ' || c == ',' || c == '/' || c == '\\')
                                safe = false;
                        }
                        // Must not start with digit or hyphen-digit
                        if (safe && (isdigit(inner[0]) ||
                            (inner[0] == '-' && inner.size() > 1 && isdigit(inner[1]))))
                            safe = false;
                        // Must not be a CSS generic family keyword
                        if (safe) {
                            static const std::unordered_set<std::string_view> GENERIC = {
                                "serif", "sans-serif", "monospace", "cursive", "fantasy",
                                "system-ui", "ui-serif", "ui-sans-serif", "ui-monospace",
                                "ui-rounded", "math", "emoji", "fangsong", "initial", "inherit",
                                "unset", "revert", "revert-layer"
                            };
                            if (GENERIC.count(inner))
                                safe = false;
                        }
                        if (safe) {
                            decl.value = doc.string_pool().intern(inner);
                            changed = true;
                        }
                    }
                }
            }

            // Map "none" → "0" for border/outline shorthands (equivalent rendering, saves 2 chars)
            // Safe: border:0 sets width to 0, same visual result as border:none
            // Not applied to border-style sub-properties — "0" is not a valid style value
            if (val == "none") {
                if (decl.property == std::string_view("border") ||
                    decl.property == std::string_view("border-top") ||
                    decl.property == std::string_view("border-right") ||
                    decl.property == std::string_view("border-bottom") ||
                    decl.property == std::string_view("border-left") ||
                    decl.property == std::string_view("outline")) {
                    decl.value = doc.string_pool().intern("0");
                    changed = true;
                }
            }
        }
        // Recurse into nested at-rule bodies
        if (rule.has_nested_rules()) {
            minify_rules(rule.nested_rules());
        }
    }
    };
    minify_rules(const_cast<std::vector<CSSRule>&>(doc.stylesheets()));

    return changed;
}

// Strip CSS declarations whose value matches the CSS initial value for that property.
// Only safe for non-inherited properties where UA stylesheets don't set non-initial values.
bool Optimizer::pass_css_default_strip(UnifiedDocument& doc) {
    // Properties that are NOT inherited AND safe to strip when set to their initial value.
    // No UA stylesheet sets these to anything other than the initial value for any element.
    //
    // A property-value pair is only stripped if the value exactly matches the initial value
    // (after prior minification passes have normalized the value — e.g., font-weight:400
    // was already normalized from "normal" by pass_css_minify).
    //
    // Note: does NOT handle shorthands like overflow or background — only longhand/simple
    // properties to avoid cascade-side-effect complexity.
    static const std::unordered_map<std::string_view, std::string_view> INITIAL_VALUES = {
        {"position", "static"},
        {"float", "none"},
        {"clear", "none"},
        {"opacity", "1"},
        {"transform", "none"},
        {"filter", "none"},
        {"resize", "none"},
        {"perspective", "none"},
        {"backface-visibility", "visible"},
        {"text-decoration-line", "none"},
        {"text-decoration-style", "solid"},
        {"text-decoration-color", "currentcolor"},
        {"text-overflow", "clip"},
        {"mix-blend-mode", "normal"},
        {"isolation", "auto"},
    };

    bool changed = false;

    std::function<void(std::vector<CSSRule>&)> strip_defaults;
    strip_defaults = [&](std::vector<CSSRule>& rules) {
        for (auto& rule : rules) {
            auto& decls = const_cast<std::vector<CSSRule::Declaration>&>(rule.declarations());

            std::vector<size_t> to_remove;

            for (size_t i = 0; i < decls.size(); i++) {
                auto it = INITIAL_VALUES.find(decls[i].property);
                if (it != INITIAL_VALUES.end() && decls[i].value == it->second) {
                    to_remove.push_back(i);
                }
            }

            for (size_t j = to_remove.size(); j > 0; j--) {
                decls.erase(decls.begin() + to_remove[j - 1]);
                changed = true;
            }

            // Recurse into nested at-rule bodies
            if (rule.has_nested_rules()) {
                strip_defaults(rule.nested_rules());
            }
        }
    };

    strip_defaults(const_cast<std::vector<CSSRule>&>(doc.stylesheets()));

    return changed;
}

// Merge adjacent CSS rules with identical declarations into a single
// comma-separated selector rule.  Cascade-safe because adjacent rules
// share the same cascade position.
//
// Example: .a{color:red;margin:0}.b{color:red;margin:0}
//       → .a,.b{color:red;margin:0}
bool Optimizer::pass_css_dedup_rules(UnifiedDocument& doc) {
    bool changed = false;

    std::function<void(std::vector<CSSRule>&)> dedup_rules;
    dedup_rules = [&](std::vector<CSSRule>& rules) {
        for (size_t i = 0; i + 1 < rules.size(); ) {
            CSSRule& left = rules[i];
            CSSRule& right = rules[i + 1];

            // Only merge non-at-rules at this level
            if (left.is_at_rule() || right.is_at_rule()) {
                i++;
                continue;
            }

            const auto& da = left.declarations();
            const auto& db = right.declarations();

            if (da.size() != db.size()) {
                i++;
                continue;
            }

            bool identical = true;
            for (size_t j = 0; j < da.size(); j++) {
                if (da[j].property != db[j].property || da[j].value != db[j].value) {
                    identical = false;
                    break;
                }
            }

            if (!identical) {
                i++;
                continue;
            }

            auto& left_selectors = const_cast<std::vector<std::vector<CSSRule::SelectorPart>>&>(left.selectors());
            for (const auto& sel : right.selectors()) {
                left_selectors.push_back(sel);
            }

            rules.erase(rules.begin() + i + 1);
            changed = true;
        }

        // Recurse into nested at-rule bodies
        for (auto& rule : rules) {
            if (rule.has_nested_rules()) {
                dedup_rules(rule.nested_rules());
            }
        }
    };

    dedup_rules(const_cast<std::vector<CSSRule>&>(doc.stylesheets()));
    return changed;
}

// Remove custom property (--var) declarations that are never referenced
// by var() calls anywhere in the document.  This pass runs after
// pass_cross_identifier so that renamed custom property names are
// consistent across declarations and references.
//
// Example: if the CSS contains `--unused: red` but no `var(--unused)`,
// the declaration is stripped.
bool Optimizer::pass_css_remove_unused_custom_props(UnifiedDocument& doc) {
    // Collect all var() references: scan every declaration value in
    // every rule for `var(--<name>` tokens.
    std::unordered_set<std::string_view> used_custom_props;

    std::function<void(const std::vector<CSSRule>&)> collect_refs;
    collect_refs = [&](const std::vector<CSSRule>& rules) {
        for (const auto& rule : rules) {
            for (const auto& decl : rule.declarations()) {
                const auto& val = decl.value;
                for (size_t pos = 0; pos + 6 <= val.size(); ) {
                    auto found = val.find("var(--", pos);
                    if (found == std::string_view::npos) break;

                    size_t name_start = found + 6;
                    size_t name_end = name_start;
                    while (name_end < val.size()) {
                        char c = val[name_end];
                        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_') {
                            name_end++;
                        } else { break; }
                    }

                    if (name_end > name_start) {
                        used_custom_props.insert(val.substr(name_start, name_end - name_start));
                    }
                    pos = name_end;
                }
            }
            if (rule.has_nested_rules()) {
                collect_refs(rule.nested_rules());
            }
        }
    };
    collect_refs(doc.stylesheets());

    if (used_custom_props.empty()) { /* no var() refs, all custom props dead */ }

    bool changed = false;

    std::function<void(std::vector<CSSRule>&)> remove_unused;
    remove_unused = [&](std::vector<CSSRule>& rules) {
        for (auto& rule : rules) {
            auto& decls = const_cast<std::vector<CSSRule::Declaration>&>(rule.declarations());

            std::vector<size_t> to_remove;
            for (size_t i = 0; i < decls.size(); i++) {
                const auto& prop = decls[i].property;
                if (prop.size() >= 2 && prop[0] == '-' && prop[1] == '-') {
                    std::string_view name = prop.substr(2);
                    if (used_custom_props.find(name) == used_custom_props.end()) {
                        to_remove.push_back(i);
                    }
                }
            }

            for (size_t j = to_remove.size(); j > 0; j--) {
                decls.erase(decls.begin() + to_remove[j - 1]);
                changed = true;
            }

            if (rule.has_nested_rules()) {
                remove_unused(rule.nested_rules());
            }
        }
    };
    remove_unused(const_cast<std::vector<CSSRule>&>(doc.stylesheets()));

    return changed;
}

// Apply CSS class/ID renames to at-rule raw bodies.
// After cross_identifier renames selectors in top-level CSS rules, the same
// class/ID names inside @media, @supports, etc. are left unchanged because
// those bodies are opaque raw text.  This pass fixes that by applying the
// rename_map as text replacements inside raw_body strings.
bool Optimizer::pass_css_rename_in_at_rules(UnifiedDocument& doc) {
    const auto& rm = doc.css_rename_map;
    if (rm.empty()) return false;

    bool changed = false;

    for (auto& rule : const_cast<std::vector<CSSRule>&>(doc.stylesheets())) {
        if (!rule.has_raw_body()) continue;
        if (!rule.is_at_rule()) continue;

        std::string body = rule.raw_body();
        std::string new_body;
        new_body.reserve(body.size());

        size_t i = 0;
        while (i < body.size()) {
            char c = body[i];
            // Check for class selector (.name) or ID selector (#name)
            if ((c == '.' || c == '#') && i + 1 < body.size()) {
                // Extract the name that follows
                size_t name_start = i + 1;
                size_t name_end = name_start;
                while (name_end < body.size()) {
                    char nc = body[name_end];
                    if ((nc >= 'a' && nc <= 'z') || (nc >= 'A' && nc <= 'Z') ||
                        (nc >= '0' && nc <= '9') || nc == '-' || nc == '_') {
                        name_end++;
                    } else {
                        break;
                    }
                }

                if (name_end > name_start) {
                    std::string_view name(body.data() + name_start, name_end - name_start);
                    auto it = rm.find(name);
                    if (it != rm.end()) {
                        new_body += c;  // keep '.' or '#'
                        new_body += it->second;  // short name
                        i = name_end;
                        changed = true;
                        continue;
                    }
                }
            }
            new_body += c;
            i++;
        }

        if (changed) {
            rule.set_raw_body(std::move(new_body));
        }
    }

    return changed;
}

} // namespace tinyizer
