#pragma once
#include "tokenizer.h"
#include "../ir/dom_node.h"
#include "../ir/unified_doc.h"
#include <memory>
#include <string>
#include <unordered_set>

namespace tinyizer {

// Parses HTML5 into a DOM tree. Handles common patterns;
// not a full WHATWG-compliant parser, but good enough for minification.
class HTMLParser {
public:
    explicit HTMLParser(StringPool& pool);

    // Parse HTML string into a DOM tree rooted at the given document.
    // If from_inline is true, parses an HTML fragment (no doctype/head/body wrapping).
    std::unique_ptr<DOMNode> parse(std::string_view html, bool fragment_mode = false);

    // Extract inline CSS and scripts found during parsing
    std::vector<std::string> take_inline_styles() { return std::move(inline_styles_); }
    std::vector<std::string> take_inline_scripts() { return std::move(inline_scripts_); }

private:
    void parse_children(DOMNode* parent, const std::unordered_set<std::string_view>& stop_tags = {});
    DOMNode* parse_element();
    void parse_text(DOMNode* parent);
    void parse_comment(DOMNode* parent);
    void parse_doctype(DOMNode* parent);

    // Attribute parsing
    void parse_attributes(DOMNode* element);

    // Handle special elements (script, style, pre, textarea)
    void parse_raw_text(DOMNode* element, std::string_view end_tag);
    void parse_script_content(DOMNode* element);

    StringPool& pool_;
    std::unique_ptr<Tokenizer> tok_;
    std::vector<std::string> inline_styles_;
    std::vector<std::string> inline_scripts_;
};

} // namespace tinyizer
