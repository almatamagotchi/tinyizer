#include "html_parser.h"
#include <cctype>
#include <algorithm>

namespace tinyizer {

// Tags whose content should not be minified
static const std::unordered_set<std::string_view> RAW_TEXT_TAGS = {
    "script", "style", "pre", "textarea", "code", "xmp"
};

// Tags that are void (self-closing) in HTML5
static const std::unordered_set<std::string_view> VOID_TAGS = {
    "area", "base", "br", "col", "embed", "hr", "img", "input",
    "link", "meta", "param", "source", "track", "wbr"
};

// Inline elements
static const std::unordered_set<std::string_view> INLINE_TAGS = {
    "a", "abbr", "b", "bdi", "bdo", "br", "cite", "code", "data",
    "dfn", "em", "i", "kbd", "mark", "q", "rp", "rt", "ruby", "s",
    "samp", "small", "span", "strong", "sub", "sup", "time", "u",
    "var", "wbr", "del", "ins"
};

HTMLParser::HTMLParser(StringPool& pool) : pool_(pool) {}

std::unique_ptr<DOMNode> HTMLParser::parse(std::string_view html, bool fragment_mode) {
    tok_ = std::make_unique<Tokenizer>(html);

    auto root = std::make_unique<DOMNode>(DOMNode::Type::ELEMENT, pool_.intern("__root__"));

    if (!fragment_mode) {
        // Skip BOM if present
        tok_->match("\xEF\xBB\xBF");
        // Try to parse DOCTYPE
        tok_->skip_whitespace();
        if (tok_->peek_match("<!DOCTYPE") || tok_->peek_match("<!doctype")) {
            parse_doctype(root.get());
        }
    }

    parse_children(root.get());

    return root;
}

void HTMLParser::parse_doctype(DOMNode* parent) {
    size_t start = tok_->pos();
    tok_->match("<!DOCTYPE");
    tok_->match("<!doctype");
    tok_->skip_whitespace();
    // Read to >
    while (!tok_->eof() && tok_->peek() != '>') {
        tok_->advance();
    }
    tok_->match('>');
    // Create a DOCTYPE node so the serializer can emit <!doctypehtml>
    auto doctype = std::make_unique<DOMNode>(DOMNode::Type::DOCTYPE, pool_.intern("html"));
    parent->add_child(std::move(doctype));
}

void HTMLParser::parse_children(DOMNode* parent, const std::unordered_set<std::string_view>& stop_tags) {
    while (!tok_->eof()) {
        tok_->skip_whitespace();

        if (tok_->eof()) break;

        if (tok_->peek() == '<') {
            if (tok_->peek_ahead(1) == '/') {
                // Closing tag
                size_t saved_pos = tok_->pos();
                tok_->skip(2); // skip "</"
                std::string_view tag = tok_->read_identifier();
                if (!tag.empty() && !stop_tags.empty() && stop_tags.count(tag)) {
                    // This stops our parent — rewind and return
                    tok_->set_pos(saved_pos);
                    return;
                }
                // Consume rest of closing tag
                tok_->read_until('>');
                tok_->match('>');
                return; // closing tag for our level
            } else if (tok_->peek_ahead(1) == '!') {
                if (tok_->peek_match("<!--")) {
                    parse_comment(parent);
                } else {
                    // <!DOCTYPE or CDATA or other SGML
                    parse_doctype(parent);
                }
            } else {
                // Opening tag
                DOMNode* child = parse_element();
                if (child) {
                    parent->add_child(std::unique_ptr<DOMNode>(child));
                }
            }
        } else {
            parse_text(parent);
        }
    }
}

DOMNode* HTMLParser::parse_element() {
    tok_->match('<'); // consume opening <

    std::string_view tag_name = tok_->read_identifier();
    if (tag_name.empty()) {
        // Not a valid tag, treat as text
        return nullptr;
    }

    // Lowercase for case-insensitive comparison
    std::string tag_lower;
    for (char c : tag_name) {
        tag_lower += (c >= 'A' && c <= 'Z') ? (c + 32) : c;
    }

    auto* element = new DOMNode(DOMNode::Type::ELEMENT, pool_.intern(tag_lower));

    parse_attributes(element);

    // Check for self-closing
    bool self_closing = tok_->match('/');
    tok_->match('>');

    if (self_closing || VOID_TAGS.count(tag_lower)) {
        return element;
    }

    // Handle raw text elements
    if (tag_lower == "script") {
        parse_script_content(element);
    } else if (tag_lower == "style") {
        parse_raw_text(element, "style");
    } else if (tag_lower == "pre" || tag_lower == "textarea" || tag_lower == "code") {
        parse_raw_text(element, tag_lower);
    } else {
        // Normal element: parse children, stopped by our own close tag
        std::unordered_set<std::string_view> stop = { pool_.intern(tag_lower) };
        // Actually stop on any close tag that matches the current element
        parse_children(element, {tag_lower});
        // Consume the closing tag
        if (tok_->peek_match("</")) {
            tok_->skip(2);
            tok_->read_identifier();
            tok_->read_until('>');
            tok_->match('>');
        }
    }

    return element;
}

void HTMLParser::parse_attributes(DOMNode* element) {
    while (!tok_->eof()) {
        tok_->skip_whitespace();

        char c = tok_->peek();
        if (c == '>' || c == '/' || c == '\0') break;

        // Attribute name — lowercase for case normalization
        std::string_view raw_name = tok_->read_identifier();
        if (raw_name.empty()) {
            // Unexpected char, skip it
            tok_->advance();
            continue;
        }

        // Lowercase the attribute name
        std::string name_lower;
        for (char ch : raw_name) {
            name_lower += (ch >= 'A' && ch <= 'Z') ? (ch + 32) : ch;
        }
        const char* name = pool_.intern(name_lower);

        tok_->skip_whitespace();

        // Check for value
        if (tok_->peek() == '=') {
            tok_->advance();
            tok_->skip_whitespace();

            char quote = tok_->peek();
            bool has_quotes = (quote == '"' || quote == '\'');

            std::string_view value;
            if (has_quotes) {
                // read_quoted handles the opening quote internally and stops at closing quote
                size_t start = tok_->pos();
                // Skip the opening quote, read until closing
                tok_->advance(); // skip opening quote
                size_t val_start = tok_->pos();
                while (!tok_->eof() && tok_->peek() != quote) {
                    if (tok_->peek() == '\\') tok_->advance(); // skip escaped char
                    tok_->advance();
                }
                value = tok_->substr(val_start, tok_->pos());
                tok_->advance(); // skip closing quote
            } else {
                // Unquoted value: read until whitespace or > or /
                size_t start = tok_->pos();
                while (!tok_->eof()) {
                    char ch = tok_->peek();
                    if (is_whitespace(ch) || ch == '>' || ch == '/') break;
                    tok_->advance();
                }
                value = tok_->substr(start, tok_->pos());
            }

            // Intern value
            const char* ivalue = pool_.intern(value);
            element->add_attr(name, ivalue, has_quotes);
        } else {
            // Boolean attribute (no value)
            element->add_attr(name, pool_.intern(""), false);
        }
    }
}

void HTMLParser::parse_text(DOMNode* parent) {
    size_t start = tok_->pos();
    while (!tok_->eof() && tok_->peek() != '<') {
        tok_->advance();
    }

    if (tok_->pos() > start) {
        std::string_view text = tok_->substr(start, tok_->pos());

        // Trim leading and trailing whitespace for non-pre environments
        // (We'll do proper whitespace handling in the minifier)

        auto text_node = std::make_unique<DOMNode>(DOMNode::Type::TEXT, pool_.intern(text));
        parent->add_child(std::move(text_node));
    }
}

void HTMLParser::parse_comment(DOMNode* parent) {
    // Just skip comments — they'll be removed during minification anyway
    tok_->match("<!--");
    size_t start = tok_->pos();
    while (!tok_->eof()) {
        if (tok_->peek() == '-' && tok_->peek_ahead(1) == '-' && tok_->peek_ahead(2) == '>') {
            break;
        }
        tok_->advance();
    }
    std::string_view comment_text = tok_->substr(start, tok_->pos());
    tok_->match("-->");

    auto comment = std::make_unique<DOMNode>(DOMNode::Type::COMMENT, pool_.intern(comment_text));
    parent->add_child(std::move(comment));
}

void HTMLParser::parse_raw_text(DOMNode* element, std::string_view end_tag) {
    size_t start = tok_->pos();
    std::string end_pattern = "</";
    end_pattern += end_tag;
    end_pattern += ">";

    while (!tok_->eof()) {
        if (tok_->peek_match(end_pattern.c_str())) break;
        tok_->advance();
    }

    std::string_view raw = tok_->substr(start, tok_->pos());

    // Store inline style text
    inline_styles_.push_back(std::string(raw));

    // Add as text child
    auto text_node = std::make_unique<DOMNode>(DOMNode::Type::TEXT, pool_.intern(raw));
    element->add_child(std::move(text_node));

    // Consume closing tag
    if (tok_->peek_match(end_pattern.c_str())) {
        tok_->skip(end_pattern.size());
    }
}

void HTMLParser::parse_script_content(DOMNode* element) {
    size_t start = tok_->pos();

    while (!tok_->eof()) {
        if (tok_->peek() == '<' && tok_->peek_ahead(1) == '/' &&
            (tok_->peek_ahead(2) == 's' || tok_->peek_ahead(2) == 'S')) {
            // Check for </script>
            size_t saved = tok_->pos();
            tok_->skip(2); // </
            std::string_view tag = tok_->read_identifier();
            tok_->set_pos(saved);
            std::string tag_lower;
            for (char c : tag) tag_lower += (c >= 'A' && c <= 'Z') ? (c + 32) : c;
            if (tag_lower == "script") break;
        }
        tok_->advance();
    }

    std::string_view script_text = tok_->substr(start, tok_->pos());
    inline_scripts_.push_back(std::string(script_text));

    // Store as text node
    auto text_node = std::make_unique<DOMNode>(DOMNode::Type::TEXT, pool_.intern(script_text));
    element->add_child(std::move(text_node));

    // Consume closing </script>
    if (tok_->peek_match("</")) {
        tok_->skip(2);
        tok_->read_identifier();
        tok_->read_until('>');
        tok_->match('>');
    }
}

} // namespace tinyizer
