#pragma once
#include "tokenizer.h"
#include "../ir/css_rule.h"
#include "../ir/unified_doc.h"
#include <vector>

namespace tinyizer {

// Parses CSS into a list of CSSRule objects.
// Handles standard CSS3 syntax including at-rules, nested blocks,
// selectors, and declarations.
class CSSParser {
public:
    explicit CSSParser(StringPool& pool);

    // Parse a full CSS stylesheet
    std::vector<CSSRule> parse(std::string_view css);

private:
    // Parse a ruleset: selector { declarations }
    CSSRule parse_rule();
    bool parse_at_rule(CSSRule& rule, std::string_view at_name);

    // Parse a selector into parts
    std::vector<CSSRule::SelectorPart> parse_selector();
    CSSRule::SelectorPart parse_selector_part();

    // Parse a declaration: property: value;
    CSSRule::Declaration parse_declaration();

    // Skip a CSS comment /* ... */
    void skip_comment();

    // Skip a block { ... } (for unhandled at-rules)
    void skip_block();

    StringPool& pool_;
    std::unique_ptr<Tokenizer> tok_;
};

} // namespace tinyizer
