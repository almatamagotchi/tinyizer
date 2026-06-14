#pragma once
#include <string>
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

    // Raw body for at-rules whose content contains nested rules (@media, @keyframes, etc.)
    // that the parser can't fully represent in the flat declaration model.
    // When set, this replaces the standard selector + declarations serialization.
    // Stored as a string copy (not string_view) because the original CSS buffer
    // may be moved after parsing.
    // Raw body access (for at-rules that store nested content as opaque text)
    bool has_raw_body() const { return !raw_body_.empty(); }
    const std::string& raw_body() const { return raw_body_; }
    void set_raw_body(std::string body) { raw_body_ = std::move(body); }

    // Structured nested rules (for at-rules with parsed sub-rules)
    bool has_nested_rules() const { return !nested_rules_.empty(); }
    const std::vector<CSSRule>& nested_rules() const { return nested_rules_; }
    std::vector<CSSRule>& nested_rules() { return nested_rules_; }
    void set_nested_rules(std::vector<CSSRule> rules) { nested_rules_ = std::move(rules); }

    // Does this rule's *first* selector reference a specific identifier?
    // (used for cross-language identifier tracking)
    std::vector<std::string_view> referenced_classes() const;
    std::vector<std::string_view> referenced_ids() const;
    std::vector<std::string_view> referenced_elements() const;

    // Reconstruct the text of the first selector (for cascade analysis)
    std::string selector_text() const;

    // Check if this rule could match a DOM node (simplified cascade simulation)
    bool could_match(const class DOMNode& node) const;

private:
    std::vector<std::vector<SelectorPart>> selectors_;
    std::vector<Declaration> declarations_;
    bool is_at_rule_ = false;
    std::string_view at_rule_name_;
    std::string raw_body_;  // for at-rules with nested content (@media, @keyframes, etc.)
    std::vector<CSSRule> nested_rules_;  // parsed sub-rules for nested at-rules
    // Legacy flat storage, used during parsing
    std::vector<SelectorPart> selector_parts_;
};

} // namespace tinyizer
