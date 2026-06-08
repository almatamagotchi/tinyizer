#pragma once
#include <string_view>
#include <vector>
#include <memory>
#include <unordered_map>

namespace tinyizer {

// A parsed CSS rule (simplified but handles real-world patterns)
class CSSRule {
public:
    // Selector components for analysis
    struct SelectorPart {
        enum class Type : uint8_t {
            ELEMENT,   // div, span
            CLASS,     // .foo
            ID,        // #bar
            ATTR,      // [data-x]
            PSEUDO,    // :hover, ::before
            COMBINATOR, // >, +, ~, ' '
            UNIVERSAL, // *
        };
        Type type;
        std::string_view value;  // class name, id, tag name, etc.
    };

    // Declaration (property + value)
    struct Declaration {
        std::string_view property;
        std::string_view value;
        bool important = false;
    };

    CSSRule() = default;

    const std::vector<SelectorPart>& selector_parts() const { return selector_parts_; }
    void add_selector_part(SelectorPart part) { selector_parts_.push_back(part); }

    const std::vector<std::vector<SelectorPart>>& selectors() const { return selectors_; }
    void add_selector(std::vector<SelectorPart> sel) {
        // Also populate legacy flat storage for backward compat
        for (const auto& p : sel) {
            selector_parts_.push_back(p);
        }
        selectors_.push_back(std::move(sel));
    }

    const std::vector<Declaration>& declarations() const { return declarations_; }
    void add_declaration(Declaration decl) { declarations_.push_back(std::move(decl)); }

    bool is_at_rule() const { return is_at_rule_; }
    void set_at_rule(bool v) { is_at_rule_ = v; }
    std::string_view at_rule_name() const { return at_rule_name_; }
    void set_at_rule_name(std::string_view name) { at_rule_name_ = name; }

    // Does this rule's *first* selector reference a specific identifier?
    // (used for cross-language identifier tracking)
    std::vector<std::string_view> referenced_classes() const;
    std::vector<std::string_view> referenced_ids() const;
    std::vector<std::string_view> referenced_elements() const;

    // Check if this rule could match a DOM node (simplified cascade simulation)
    bool could_match(const class DOMNode& node) const;

private:
    std::vector<std::vector<SelectorPart>> selectors_;
    std::vector<Declaration> declarations_;
    bool is_at_rule_ = false;
    std::string_view at_rule_name_;
    // Legacy flat storage, used during parsing
    std::vector<SelectorPart> selector_parts_;
};

} // namespace tinyizer
