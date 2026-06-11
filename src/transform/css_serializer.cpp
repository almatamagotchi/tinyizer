#include "serializer.h"
#include "../parser/tokenizer.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace tinyizer {

// CSS generic family names and reserved keywords that MUST be quoted
// if used as a font-family name (we must NOT unquote these).
static const std::unordered_set<std::string> FONT_FAMILY_RESERVED = {
    "serif", "sans-serif", "monospace", "cursive", "fantasy",
    "system-ui", "ui-serif", "ui-sans-serif", "ui-monospace",
    "ui-rounded", "math", "emoji", "fangsong",
    "inherit", "initial", "unset", "revert", "none", "normal"
};

// Check if a character is valid in an unquoted font-family name.
// Allowed: letters, digits, hyphens (but not leading), spaces are NOT allowed.
static bool is_font_family_name_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
}

// Strip unnecessary quotes from font-family values.
// "Arial" -> Arial, 'Georgia' -> Georgia, "Arial","Helvetica" -> Arial,Helvetica
// Only unquotes when the inner name contains no spaces or special chars
// and is not a CSS reserved keyword.
std::string strip_font_family_quotes(const std::string& value) {
    std::string result;
    result.reserve(value.size());

    for (size_t i = 0; i < value.size(); i++) {
        char c = value[i];

        // Check for quoted string
        if (c == '"' || c == '\'') {
            char quote_char = c;
            size_t j = i + 1;
            while (j < value.size() && value[j] != quote_char) {
                if (value[j] == '\\') j++; // skip escaped char
                j++;
            }

            if (j < value.size() && value[j] == quote_char) {
                // Extract inner name
                std::string inner = value.substr(i + 1, j - i - 1);

                // Check if inner is valid as unquoted
                bool valid = !inner.empty();
                for (char ch : inner) {
                    if (!is_font_family_name_char(ch)) { valid = false; break; }
                }

                // Must not match a reserved keyword (case-insensitive for generic families)
                if (valid) {
                    std::string lower;
                    lower.reserve(inner.size());
                    for (char ch : inner) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    if (FONT_FAMILY_RESERVED.count(lower)) valid = false;
                }

                if (valid) {
                    result += inner;
                    i = j;
                    continue;
                }
            }
        }

        result += c;
    }

    return result;
}

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
        bool first_decl = true;
        for (const auto& decl : rule.declarations()) {
            if (!first_decl) out += ';';
            first_decl = false;
            out += decl.property;
            out += ':';
            std::string val(decl.value);
            // Strip unnecessary quotes from font-family values
            if (decl.property == "font-family") {
                val = strip_font_family_quotes(val);
            }
            out += minify_css_value(val);
            if (decl.important) out += "!important";
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
    // Post-processing: remove unnecessary semicolons before closing braces.
    // Safe in all JS contexts: ';}' is an empty statement followed by block end,
    // and the empty statement contributes nothing. Preserves strings/templates/regexes.
    {
        std::string cleaned;
        cleaned.reserve(out.size());
        bool in_str = false;
        char str_char = 0;
        bool in_tmpl = false;
        bool in_regex = false;
        for (size_t i = 0; i < out.size(); i++) {
            char c = out[i];
            char n = (i + 1 < out.size()) ? out[i + 1] : 0;
            if (in_str) {
                if (c == '\\') { cleaned += c; cleaned += n; i++; continue; }
                if (c == str_char) in_str = false;
                cleaned += c; continue;
            }
            if (in_tmpl) {
                if (c == '\\') { cleaned += c; cleaned += n; i++; continue; }
                if (c == '`') in_tmpl = false;
                cleaned += c; continue;
            }
            if (in_regex) {
                if (c == '\\') { cleaned += c; cleaned += n; i++; continue; }
                if (c == '/') in_regex = false;
                cleaned += c; continue;
            }
            if (c == '"' || c == '\'') { in_str = true; str_char = c; cleaned += c; continue; }
            if (c == '`') { in_tmpl = true; cleaned += c; continue; }
            // Detect regex: / followed by non-whitespace in expression context
            if (c == '/' && n != '/' && n != '*' && n != ' ' && n != '\t' && n != '\n') {
                if (!cleaned.empty() && (cleaned.back() == '(' || cleaned.back() == '=' ||
                    cleaned.back() == '!' || cleaned.back() == '&' || cleaned.back() == '|' ||
                    cleaned.back() == '{' || cleaned.back() == ';' || cleaned.back() == ':' ||
                    cleaned.back() == '[' || cleaned.back() == ',' || cleaned.back() == '?' ||
                    cleaned.back() == '~')) {
                    in_regex = true;
                    cleaned += c;
                    continue;
                }
            }
            // Remove semicolon before '}'
            if (c == ';' && n == '}') continue;
            cleaned += c;
        }
        out = std::move(cleaned);
    }
    return out;
}
// Minify raw CSS text: strip whitespace and comments, preserving semantics.
// Used as a fast path for inline <style> content when the CSS parser
// is not invoked or fails to parse.
std::string tinyizer::minify_css_text(const std::string& css) {
    std::string out;
    out.reserve(css.size());
    bool in_string = false;
    char string_delim = 0;
    bool in_comment = false;
    bool last_was_ws = true; // suppress leading whitespace

    for (size_t i = 0; i < css.size(); i++) {
        char c = css[i];
        char next = (i + 1 < css.size()) ? css[i + 1] : '\0';

        // Handle strings first
        if (!in_comment && (c == '"' || c == '\'')) {
            if (!in_string) {
                in_string = true;
                string_delim = c;
                out += c;
                continue;
            } else if (c == string_delim) {
                // Check for escape
                bool escaped = false;
                int bs = 0;
                size_t j = i - 1;
                while (j != (size_t)-1 && css[j] == '\\') { bs++; j--; }
                if (bs % 2 == 0) {
                    in_string = false;
                    string_delim = 0;
                }
                out += c;
                continue;
            }
        }

        if (in_string) {
            out += c;
            continue;
        }

        // Handle comments
        if (!in_comment && c == '/' && next == '*') {
            in_comment = true;
            i++; // skip '*'
            continue;
        }
        if (in_comment && c == '*' && next == '/') {
            in_comment = false;
            i++; // skip '/'
            // Treat comment end as whitespace to prevent token merging
            if (!last_was_ws) { out += ' '; last_was_ws = true; }
            continue;
        }
        if (in_comment) continue;

        // Collapse whitespace
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
            if (!last_was_ws) {
                // Decide if we need whitespace here
                // Keep whitespace if it separates two identifier-like tokens
                char prev = out.empty() ? '\0' : out.back();
                size_t j = i + 1;
                while (j < css.size() && (css[j] == ' ' || css[j] == '\t' || css[j] == '\n' || css[j] == '\r')) j++;
                char after = (j < css.size()) ? css[j] : '\0';

                // Only keep space between values and between selector parts
                bool prev_ident = std::isalnum((unsigned char)prev) || prev == '_' || prev == '-' || prev == '#' || prev == '.';
                bool next_ident = std::isalnum((unsigned char)after) || after == '_' || after == '-' || after == '#' || after == '.';

                if (prev_ident && next_ident && prev != '{' && prev != '}' && prev != ';' &&
                    after != '{' && after != '}' && after != ';' && after != ':') {
                    out += ' ';
                    last_was_ws = true;
                }
            }
            continue;
        }

        last_was_ws = false;
        out += c;
    }

    // Post-pass: apply value-level optimizations (colors, units, zero stripping)
    // Scan inside declaration blocks for :value; or :value} boundaries
    {
        std::string result;
        result.reserve(out.size());
        size_t depth = 0; // brace depth: 0 = selector, 1+ = inside decl block
        bool in_str = false;
        char str_delim = 0;
        std::string last_property; // track property name for property-aware value opts

        for (size_t i = 0; i < out.size(); i++) {
            char c = out[i];

            // Track strings (avoid treating ; or } inside strings as delimiters)
            if (!in_str && (c == '"' || c == '\'')) {
                in_str = true;
                str_delim = c;
                result += c;
                continue;
            }
            if (in_str) {
                result += c;
                if (c == str_delim) {
                    int bs = 0;
                    size_t j = i - 1;
                    while (j != (size_t)-1 && out[j] == '\\') { bs++; j--; }
                    if (bs % 2 == 0) in_str = false;
                }
                continue;
            }

            // Track brace depth
            if (c == '{') { depth++; result += c; continue; }
            if (c == '}') { if (depth > 0) depth--; result += c; continue; }

            // Inside a declaration block, track property names before ':'
            // and ':' separates property from value
            if (depth > 0 && c == ':') {
                // Extract the property name (everything between last ; or { and this ':')
                size_t prop_start = result.size();
                while (prop_start > 0) {
                    prop_start--;
                    if (result[prop_start] == ';' || result[prop_start] == '{') {
                        prop_start++;
                        break;
                    }
                }
                last_property = result.substr(prop_start);
                // Trim whitespace
                while (!last_property.empty() && (last_property.back() == ' ' || last_property.back() == '\t'))
                    last_property.pop_back();
                while (!last_property.empty() && (last_property.front() == ' ' || last_property.front() == '\t'))
                    last_property.erase(0, 1);

                result += c;
                i++;
                if (i >= out.size()) break;

                // Find value end: next unquoted ';' or '}'
                size_t val_start = i;
                while (i < out.size()) {
                    char vc = out[i];
                    if (vc == '"' || vc == '\'') {
                        char sd = vc;
                        i++;
                        while (i < out.size() && out[i] != sd) i++;
                        if (i < out.size()) i++;
                    } else if (vc == ';' || vc == '}') {
                        break;
                    } else {
                        i++;
                    }
                }

                // Extract and optimize value
                if (i > val_start) {
                    std::string val(out, val_start, i - val_start);
                    // Property-specific value optimizations
                    if (last_property == "font-family") {
                        val = strip_font_family_quotes(val);
                    }
                    std::string opt_val = minify_css_value(val);
                    result += opt_val;
                }

                // Output the delimiter
                if (i < out.size()) {
                    result += out[i];
                    i++;
                }
                i--; // compensate for loop increment
                continue;
            }

            result += c;
        }
        out = std::move(result);
    }

    // Remove trailing whitespace
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t' || out.back() == '\n'))
        out.pop_back();

    return out;
}
