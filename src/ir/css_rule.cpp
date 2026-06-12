#include "css_rule.h"
#include "dom_node.h"
#include <string>

namespace tinyizer {

std::vector<std::string_view> CSSRule::referenced_classes() const {
    std::vector<std::string_view> result;
    for (const auto& sel : selectors_) {
        for (const auto& part : sel) {
            if (part.type == SelectorPart::Type::CLASS) {
                result.push_back(part.value);
            }
        }
    }
    return result;
}

std::vector<std::string_view> CSSRule::referenced_ids() const {
    std::vector<std::string_view> result;
    for (const auto& sel : selectors_) {
        for (const auto& part : sel) {
            if (part.type == SelectorPart::Type::ID) {
                result.push_back(part.value);
            }
        }
    }
    return result;
}

std::vector<std::string_view> CSSRule::referenced_elements() const {
    std::vector<std::string_view> result;
    for (const auto& sel : selectors_) {
        for (const auto& part : sel) {
            if (part.type == SelectorPart::Type::ELEMENT) {
                result.push_back(part.value);
            }
        }
    }
    return result;
}

std::string CSSRule::selector_text() const {
    if (selectors_.empty()) return "";
    std::string out;
    for (const auto& part : selectors_[0]) {
        switch (part.type) {
        case SelectorPart::Type::ELEMENT: out += part.value; break;
        case SelectorPart::Type::CLASS: out += '.'; out += part.value; break;
        case SelectorPart::Type::ID: out += '#'; out += part.value; break;
        case SelectorPart::Type::ATTR: out += '['; out += part.value; out += ']'; break;
        case SelectorPart::Type::PSEUDO: out += part.value; break;
        case SelectorPart::Type::COMBINATOR: out += part.value; break;
        case SelectorPart::Type::UNIVERSAL: out += '*'; break;
        }
    }
    return out;
}

bool CSSRule::could_match(const DOMNode& node) const {
    if (node.type() != DOMNode::Type::ELEMENT) return false;

    for (const auto& sel : selectors_) {
        if (sel.empty()) continue;

        // Simple check: does last selector part match?
        // Real cascade is more complex (combinators, specificity), but this
        // is a conservative approximation that errs on keeping rules.
        const auto& last = sel.back();

        switch (last.type) {
        case SelectorPart::Type::ELEMENT:
            if (last.value == node.tag_name()) return true;
            break;
        case SelectorPart::Type::CLASS:
            for (const auto& attr : node.attrs()) {
                if (attr.name == "class") {
                    // Check if class value contains this class
                    std::string_view val = attr.value;
                    size_t pos = 0;
                    while (pos < val.size()) {
                        // Skip whitespace
                        while (pos < val.size() && (val[pos] == ' ' || val[pos] == '\t' || val[pos] == '\n'))
                            pos++;
                        size_t end = pos;
                        while (end < val.size() && val[end] != ' ' && val[end] != '\t' && val[end] != '\n')
                            end++;
                        if (pos < end && val.substr(pos, end - pos) == last.value) return true;
                        pos = end;
                    }
                }
            }
            break;
        case SelectorPart::Type::ID:
            for (const auto& attr : node.attrs()) {
                if (attr.name == "id" && attr.value == last.value) return true;
            }
            break;
        case SelectorPart::Type::UNIVERSAL:
            return true;
        case SelectorPart::Type::ATTR:
            // [attr] matches if attr exists — be conservative
            for (const auto& sel_part : sel) {
                if (sel_part.type == SelectorPart::Type::ATTR) {
                    for (const auto& attr : node.attrs()) {
                        if (attr.name == sel_part.value) return true;
                    }
                }
            }
            break;
        default:
            break;
        }
    }

    return false;
}

} // namespace tinyizer
