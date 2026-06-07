#include "serializer.h"
#include "../parser/tokenizer.h"
#include <sstream>
#include <algorithm>

namespace tinyizer {

// Serialize a list of CSS rules to a minified string
std::string serialize_css(const std::vector<CSSRule>& rules) {
    std::string out;
    out.reserve(16384);

    for (const auto& rule : rules) {
        // At-rule handling
        if (rule.is_at_rule()) {
            out += '@';
            out += rule.at_rule_name();
            out += '{';
        }

        // Multiple selectors
        bool first_sel = true;
        for (const auto& sel : rule.selectors()) {
            if (!first_sel) out += ',';
            first_sel = false;

            for (const auto& part : sel) {
                switch (part.type) {
                case CSSRule::SelectorPart::Type::ELEMENT:
                    out += part.value;
                    break;
                case CSSRule::SelectorPart::Type::CLASS:
                    out += '.';
                    out += part.value;
                    break;
                case CSSRule::SelectorPart::Type::ID:
                    out += '#';
                    out += part.value;
                    break;
                case CSSRule::SelectorPart::Type::ATTR:
                    out += '[';
                    out += part.value;
                    out += ']';
                    break;
                case CSSRule::SelectorPart::Type::PSEUDO:
                    out += part.value;
                    break;
                case CSSRule::SelectorPart::Type::COMBINATOR:
                    out += part.value;  // includes space for descendant combinator
                    break;
                case CSSRule::SelectorPart::Type::UNIVERSAL:
                    out += '*';
                    break;
                }
            }
        }

        out += '{';

        // Declarations
        for (const auto& decl : rule.declarations()) {
            out += decl.property;
            out += ':';
            out += decl.value;
            if (decl.important) out += "!important";
            out += ';';
        }

        out += '}';
    }

    return out;
}

} // namespace tinyizer

// JS text minifier
std::string tinyizer::minify_js_text(const std::string& js) {
    std::string out;
    out.reserve(js.size());
    bool in_line_comment = false;
    bool in_block_comment = false;
    bool in_string = false;
    char string_char = 0;
    bool in_template = false;
    bool need_space = false;
    bool after_word = false;

    for (size_t i = 0; i < js.size(); i++) {
        char c = js[i];
        char next = (i + 1 < js.size()) ? js[i + 1] : 0;

        if (in_line_comment) { if (c == '\n') { in_line_comment = false; need_space = true; } continue; }
        if (in_block_comment) { if (c == '*' && next == '/') { in_block_comment = false; i++; } continue; }

        if (in_string) {
            if (c == '\\') { out += c; out += next; i++; continue; }
            if (c == string_char) in_string = false;
            out += c; after_word = false; continue;
        }
        if (in_template) {
            if (c == '\\') { out += c; out += next; i++; continue; }
            if (c == '`') in_template = false;
            out += c; after_word = false; continue;
        }

        if (c == '"' || c == '\'') { in_string = true; string_char = c; out += c; need_space = false; after_word = false; continue; }
        if (c == '`') { in_template = true; out += c; need_space = false; after_word = false; continue; }

        if (c == '/' && next == '/') { in_line_comment = true; i++; continue; }
        if (c == '/' && next == '*') { in_block_comment = true; i++; continue; }

        // Regex literals: skip space before / when in expression context
        if (c == '/' && after_word && !out.empty()) {
            char prev = out.back();
            if (prev == '=' || prev == '(' || prev == '!' || prev == '&' ||
                prev == '|' || prev == '{' || prev == ';' || prev == ':' ||
                prev == '[' || prev == ',' || prev == '?' || prev == '~' || prev == '\n') {
                out += c; after_word = false; need_space = false; continue;
            }
        }

        if (std::isspace(static_cast<unsigned char>(c))) {
            if (after_word) need_space = true;
            continue;
        }

        if (need_space && after_word && std::isalnum(static_cast<unsigned char>(c))) {
            out += ' ';
        }
        need_space = false;
        out += c;
        after_word = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
    }
    return out;
}
