#include "optimizer.h"
#include "../util/frequency_map.h"
#include "../parser/tokenizer.h"
#include <unordered_set>
#include <unordered_map>
#include <functional>

namespace tinyizer {

// Cross-language identifier squeezing — the novel optimization.
//
// Algorithm:
// 1. Walk the DOM tree and collect all HTML IDs and classes
// 2. Walk all CSS rules and collect class/id selectors + custom properties
// 3. Walk JS scope tree and collect all variable/function names
// 4. Build a global frequency map (most frequent → shortest name)
// 5. Rename identifiers consistently across all three languages
//
// The key insight: by doing this jointly, we can assign single-character
// names to identifiers used across HTML, CSS, AND JS simultaneously.

bool Optimizer::pass_cross_identifier(UnifiedDocument& doc) {
    FrequencyMap<std::string_view> freq_map;
    std::unordered_map<std::string_view, std::string> rename_map;
    std::unordered_set<std::string_view> doc_html_css_names; // names that appear in HTML/CSS

    // ---- Collect from HTML ----
    if (doc.root()) {
        doc.root()->walk([&](const DOMNode& node) {
            if (node.type() != DOMNode::Type::ELEMENT) return;

            for (const auto& attr : node.attrs()) {
                if (attr.name == std::string_view("id")) {
                    freq_map.record(attr.value);
                    doc_html_css_names.insert(attr.value);
                } else if (attr.name == std::string_view("class")) {
                    // Split class attribute by whitespace
                    std::string_view val = attr.value;
                    if (val.empty()) return;
                    size_t pos = 0;
                    while (pos < val.size()) {
                        while (pos < val.size() && is_whitespace(val[pos])) pos++;
                        size_t end = pos;
                        while (end < val.size() && !is_whitespace(val[end])) end++;
                        if (end > pos) {
                            std::string_view cls = val.substr(pos, end - pos);
                            if (!cls.empty()) {
                                freq_map.record(cls);
                                doc_html_css_names.insert(cls);
                            }
                        }
                        pos = end;
                    }
                }
                // Also track data-* attributes that might be accessed from JS
                if (attr.name.size() > 5 &&
                    attr.name.substr(0, 5) == std::string_view("data-")) {
                    freq_map.record(attr.name);
                    doc_html_css_names.insert(attr.name);
                }
            }
        });
    }

    // ---- Collect from CSS ----
    for (const auto& rule : doc.stylesheets()) {
        // Classes
        for (auto cls : rule.referenced_classes()) {
            if (!cls.empty()) {
                freq_map.record(cls);
                doc_html_css_names.insert(cls);
            }
        }
        // IDs
        for (auto id : rule.referenced_ids()) {
            if (!id.empty()) {
                freq_map.record(id);
                doc_html_css_names.insert(id);
            }
        }
        // Custom properties
        for (const auto& decl : rule.declarations()) {
            if (decl.property.size() >= 2 &&
                decl.property[0] == '-' && decl.property[1] == '-') {
                freq_map.record(decl.property);
                doc_html_css_names.insert(decl.property);
            }
        }
    }

    // ---- Collect from JS ----
    // all_declared_names() is recursive, so calling it on root collects
    // everything. We do NOT descend into child scopes — that would double-count.
    if (doc.js_root_scope()) {
        auto names = doc.js_root_scope()->all_declared_names();
        for (auto name : names) {
            if (!name.empty() && name.size() > 1) {
                freq_map.record(name);
            }
        }
    }

    // ---- JS-touched names are NO LONGER removed from rename candidates ----

    // ---- Check if we have enough identifiers to squeeze ----
    if (freq_map.size() < 2) return false;

    // ---- Collect actual HTML tag names present in the DOM ----
    // A class/id that shares a name with an HTML tag is only risky to rename
    // if that tag actually appears in the document. If <header> isn't in the
    // markup, renaming .header is safe.
    std::unordered_set<std::string_view> actual_html_tags;
    if (doc.root()) {
        doc.root()->walk([&](const DOMNode& node) {
            if (node.type() == DOMNode::Type::ELEMENT) {
                actual_html_tags.insert(node.tag_name());
            }
        });
    }

    // ---- Build rename map ----
    // Two-tier safety: JS keywords/builtins are never renamed.
    // HTML element/attribute names are only skipped if they actually appear
    // in this document's markup as actual tags AND as class/id names.

    auto by_freq = freq_map.sorted();
    size_t rank = 0;
    for (const auto& [name, freq] : by_freq) {
        // Always skip JS keywords and builtins — these are never safe to rename
        static const std::unordered_set<std::string_view> JS_UNSAFE = {
            "document", "window", "console", "Math", "Array", "Object", "String",
            "Number", "Boolean", "Function", "Promise", "JSON", "Error", "Map", "Set",
            "parseInt", "parseFloat", "isNaN", "undefined", "null", "true", "false",
            "var", "let", "const", "function", "return", "if", "else", "for", "while",
            "break", "continue", "switch", "case", "default", "try", "catch", "throw",
            "new", "this", "typeof", "instanceof", "in", "of", "class", "extends",
            "import", "export", "async", "await", "yield", "delete", "void",
        };
        if (JS_UNSAFE.count(name) > 0) continue;

        // Skip a class/id that shares a reserved HTML tag/attribute name
        // only if that tag is actually present in the document AND the name
        // appears as a class/id. If the tag isn't in the markup, renaming
        // the class/id is safe (e.g., renaming .header when <header> is absent).
        static const std::unordered_set<std::string_view> HTML_RESERVED = {
            "html", "head", "body", "div", "span", "p", "a", "img", "ul", "ol", "li",
            "table", "tr", "td", "th", "form", "input", "button", "select", "option",
            "h1", "h2", "h3", "h4", "h5", "h6", "section", "article", "nav", "header",
            "footer", "main", "aside", "style", "script", "link", "meta", "title",
            "class", "id", "style", "href", "src", "alt", "type", "name", "value",
            "width", "height", "color", "margin", "padding", "border", "display",
            "position", "top", "left", "right", "bottom", "font", "size", "text",
            "background", "width", "height", "min-width", "max-width", "overflow",
        };
        if (HTML_RESERVED.count(name) > 0 && doc_html_css_names.count(name) > 0 && actual_html_tags.count(name) > 0) continue;

        if (name.size() <= 1) continue; // already minimal
        std::string short_name;
        do {
            short_name = FrequencyMap<std::string_view>::name_for_rank(rank++);
        } while (JS_UNSAFE.count(short_name) > 0 || HTML_RESERVED.count(short_name) > 0);
        rename_map[name] = short_name;
    }

    if (rename_map.empty()) return false;

    // ---- Update cumulative rename map (original → current short name) ----
    for (const auto& [old_name, new_name] : rename_map) {
        std::string old_str(old_name);
        // Check if this old name was itself a previous rename target
        std::string original = old_str;
        for (const auto& [orig, prev_curr] : doc.cumulative_rename) {
            if (prev_curr == old_str) {
                original = orig;
                break;
            }
        }
        doc.cumulative_rename[original] = new_name;
    }

    // ---- Store rename map for use during serialization ----
    doc.css_rename_map = rename_map;
    doc.js_rename_map = rename_map;  // same names used across all languages

    // ---- Apply renames to CSS rules ----
    for (auto& rule : doc.stylesheets()) {
        for (auto& sel : rule.selectors()) {
            for (auto& part : const_cast<std::vector<CSSRule::SelectorPart>&>(sel)) {
                if (part.type == CSSRule::SelectorPart::Type::CLASS ||
                    part.type == CSSRule::SelectorPart::Type::ID) {
                    auto it = rename_map.find(part.value);
                    if (it != rename_map.end()) {
                        part.value = doc.string_pool().intern(it->second);
                    }
                }
            }
        }
        // Also rename custom properties in declarations
        for (auto& decl : const_cast<std::vector<CSSRule::Declaration>&>(rule.declarations())) {
            auto it = rename_map.find(decl.property);
            if (it != rename_map.end()) {
                decl.property = doc.string_pool().intern(it->second);
            }
            // Could also rename var() references in values, but that's complex
        }
    }

    // ---- Apply renames to HTML DOM ----
    if (doc.root()) {
        // We need a mutable walk
        const_cast<DOMNode*>(doc.root())->walk([&](DOMNode& node) {
            if (node.type() != DOMNode::Type::ELEMENT) return;
            for (auto& attr : const_cast<std::vector<DOMNode::Attr>&>(node.attrs())) {
                if (attr.name == std::string_view("id") || attr.name == std::string_view("class")) {
                    // For class, we need to rename each class value
                    if (attr.name == std::string_view("class")) {
                        // Split, rename, rejoin
                        std::string new_class;
                        std::string_view val = attr.value;
                        size_t pos = 0;
                        bool first = true;
                        while (pos < val.size()) {
                            while (pos < val.size() && is_whitespace(val[pos])) pos++;
                            size_t end = pos;
                            while (end < val.size() && !is_whitespace(val[end])) end++;
                            if (end > pos) {
                                std::string_view cls = val.substr(pos, end - pos);
                                auto it = rename_map.find(cls);
                                if (!first) new_class += ' ';
                                new_class += (it != rename_map.end()) ? it->second : std::string(cls);
                                first = false;
                            }
                            pos = end;
                        }
                        attr.value = doc.string_pool().intern(new_class);
                    } else {
                        auto it = rename_map.find(attr.value);
                        if (it != rename_map.end()) {
                            attr.value = doc.string_pool().intern(it->second);
                        }
                    }
                }
                // Also rename data-* attribute names
                if (attr.name.size() > 5 && attr.name.substr(0, 5) == std::string_view("data-")) {
                    auto it = rename_map.find(attr.name);
                    if (it != rename_map.end()) {
                        attr.name = doc.string_pool().intern("data-" + it->second);
                    }
                }
            }
        });
    }

    // ---- JS-touched names are no longer removed from rename candidates.
    // String-literal updates happen in optimizer.cpp after optimized_js is generated.
    return !rename_map.empty();
}

} // namespace tinyizer
