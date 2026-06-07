#include "optimizer.h"
#include "../parser/tokenizer.h"
#include <unordered_set>
#include <algorithm>

namespace tinyizer {

// Dead CSS elimination via cascade simulation.
//
// Algorithm:
// 1. For each CSS rule, check if any of its selectors could match
//    any element in the DOM tree.
// 2. If a rule has NO matching selectors, it's dead code — remove it.
// 3. Re-run after other passes because they may expose new dead rules.
//
// This is conservative: if we can't prove a rule doesn't match, we keep it.
// Pseudo-classes (:hover, :focus, :nth-child, etc.) are assumed to match.

bool Optimizer::pass_dead_css(UnifiedDocument& doc) {
    if (!doc.root()) return false;

    // Build a set of all DOM elements by tag name
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
                // Split class values
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

    // Check each CSS rule
    auto& rules = const_cast<std::vector<CSSRule>&>(doc.stylesheets());
    size_t before = rules.size();
    size_t removed = 0;

    // We need to manually iterate and erase
    size_t i = 0;
    while (i < rules.size()) {
        auto& rule = rules[i];
        bool has_match = false;

        // Check each selector in the rule
        for (const auto& sel : rule.selectors()) {
            if (sel.empty()) continue;

            // Check the LAST selector part (the "subject" of the selector)
            // e.g., in "div .foo > #bar", the subject is #bar
            const auto& last = sel.back();

            switch (last.type) {
            case CSSRule::SelectorPart::Type::ELEMENT:
                if (dom_tags.count(last.value) > 0) has_match = true;
                break;
            case CSSRule::SelectorPart::Type::CLASS:
                if (dom_classes.count(last.value) > 0) has_match = true;
                break;
            case CSSRule::SelectorPart::Type::ID:
                if (dom_ids.count(last.value) > 0) has_match = true;
                break;
            case CSSRule::SelectorPart::Type::ATTR:
                if (dom_attrs.count(last.value) > 0) has_match = true;
                break;
            case CSSRule::SelectorPart::Type::UNIVERSAL:
                has_match = true; // * matches everything
                break;
            case CSSRule::SelectorPart::Type::PSEUDO:
                // Pseudo-classes like :hover, :focus, :nth-child
                // We can't prove they don't match — keep them
                has_match = true;
                break;
            case CSSRule::SelectorPart::Type::COMBINATOR:
                // Combinators without a preceding subject shouldn't appear as the last part
                has_match = true;
                break;
            }

            if (has_match) break;

            // Check ancestor parts more conservatively
            // For now: if the subject matches, accept the whole selector
        }

        // At-rules that aren't media queries: keep them
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
        doc.set_total_minified_bytes(doc.total_minified_bytes() + removed * 10); // estimate
        return true;
    }
    return false;
}

} // namespace tinyizer
