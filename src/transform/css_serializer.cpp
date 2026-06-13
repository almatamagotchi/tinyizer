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
            if (rule.has_raw_body()) {
                // Apply text-level CSS minification to the raw body.
                // minify_css_text strips leading whitespace, which could eat
                // the space between the at-rule name and its prelude
                // (e.g., @media screen{...}).  Prepend a space to restore it —
                // a single space before `{` or before a selector is harmless.
                std::string minified_body = minify_css_text(rule.raw_body());
                // Fix: minify_css_text strips spaces between CSS keywords
                // like `and` / `or` / `not` and an opening `(`, which breaks
                // media-query syntax (e.g. `screen and(max-width:...)`).
                // Restore those mandatory spaces.
                {
                    std::string fixed;
                    fixed.reserve(minified_body.size() + 16);
                    for (size_t i = 0; i < minified_body.size(); i++) {
                        char ch = minified_body[i];
                        fixed += ch;
                        if (ch == '(' && fixed.size() >= 2) {
                            // Scan backward in *fixed* (which may have grown
                            // from previous space insertions) to find the
                            // preceding word, if any.
                            int j = (int)fixed.size() - 2;  // char before '(' in fixed
                            int word_len = 0;
                            while (j >= 0 && isalpha((unsigned char)fixed[j])) {
                                word_len++;
                                j--;
                            }
                            if (word_len >= 2 && word_len <= 4) {
                                const char* word_start = &fixed[j + 1];
                                std::string_view kw(word_start, word_len);
                                if (kw == "and" || kw == "or" || kw == "not" ||
                                    kw == "only") {
                                    fixed.insert(fixed.size() - 1, " ");
                                }
                            }
                        }
                    }
                    minified_body = std::move(fixed);
                }
                out += ' ';
                out += minified_body;
                continue;
            }
            // At-rules without raw_body (e.g., @font-face, @page) flow
            // through to the normal selector+declaration logic below.
            // Do NOT emit '{' here — line 138 handles that.
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

    // Coalesce consecutive appends: track the start of a run of chars
    // that can be bulk-copied. Used for strings, templates, and code runs.
    auto flush_run = [&](size_t& run_start, size_t i) {
        if (run_start < i) {
            out.append(&js[run_start], i - run_start);
        }
        run_start = i + 1;
    };
    size_t run_start = 0;

    for (size_t i = 0; i < js.size(); i++) {
        char c = js[i];
        char next = (i + 1 < js.size()) ? js[i + 1] : 0;

        if (in_line_comment) { if (c == '\n') { in_line_comment = false; need_space = true; } continue; }
        if (in_block_comment) { if (c == '*' && next == '/') { in_block_comment = false; i++; } continue; }

        if (in_string) {
            if (c == '\\') {
                flush_run(run_start, i);
                out += c; out += next; i++;
                run_start = i + 1;
                continue;
            }
            if (c == string_char) {
                flush_run(run_start, i);
                out += c;
                run_start = i + 1;
                in_string = false;
                after_word = false;
                continue;
            }
            continue;
        }
        if (in_template) {
            if (c == '\\') {
                flush_run(run_start, i);
                out += c; out += next; i++;
                run_start = i + 1;
                continue;
            }
            if (c == '`') {
                flush_run(run_start, i);
                out += c;
                run_start = i + 1;
                in_template = false;
                after_word = false;
                continue;
            }
            continue;
        }

        if (c == '"' || c == '\'') {
            flush_run(run_start, i);
            in_string = true; string_char = c;
            out += c; need_space = false; after_word = false;
            run_start = i + 1;
            continue;
        }
        if (c == '`') {
            flush_run(run_start, i);
            in_template = true;
            out += c; need_space = false; after_word = false;
            run_start = i + 1;
            continue;
        }

        if (c == '/' && next == '/') { flush_run(run_start, i); in_line_comment = true; i++; run_start = i + 1; continue; }
        if (c == '/' && next == '*') { flush_run(run_start, i); in_block_comment = true; i++; run_start = i + 1; continue; }

        // Regex literals: skip space before / when in expression context
        if (c == '/' && after_word && !out.empty()) {
            char prev = out.back();
            if (prev == '=' || prev == '(' || prev == '!' || prev == '&' ||
                prev == '|' || prev == '{' || prev == ';' || prev == ':' ||
                prev == '[' || prev == ',' || prev == '?' || prev == '~' || prev == '\n') {
                flush_run(run_start, i);
                out += c; after_word = false; need_space = false;
                run_start = i + 1;
                continue;
            }
        }

        if (std::isspace(static_cast<unsigned char>(c))) {
            flush_run(run_start, i);
            if (after_word) need_space = true;
            run_start = i + 1;
            continue;
        }

        if (need_space && after_word && std::isalnum(static_cast<unsigned char>(c))) {
            flush_run(run_start, i);
            out += ' ';
            run_start = i;
        }
        need_space = false;
        after_word = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
    }
    // Flush any remaining run
    flush_run(run_start, js.size());
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

        auto c_flush = [&](size_t& c_run, size_t i) {
            if (c_run < i) cleaned.append(&out[c_run], i - c_run);
            c_run = i + 1;
        };
        size_t c_run = 0;

        for (size_t i = 0; i < out.size(); i++) {
            char c = out[i];
            char n = (i + 1 < out.size()) ? out[i + 1] : 0;
            if (in_str) {
                if (c == '\\') { c_flush(c_run, i); cleaned += c; cleaned += n; i++; c_run = i + 1; continue; }
                if (c == str_char) {
                    c_flush(c_run, i);
                    cleaned += c;
                    c_run = i + 1;
                    in_str = false;
                    continue;
                }
                continue;
            }
            if (in_tmpl) {
                if (c == '\\') { c_flush(c_run, i); cleaned += c; cleaned += n; i++; c_run = i + 1; continue; }
                if (c == '`') {
                    c_flush(c_run, i);
                    cleaned += c;
                    c_run = i + 1;
                    in_tmpl = false;
                    continue;
                }
                continue;
            }
            if (in_regex) {
                if (c == '\\') { c_flush(c_run, i); cleaned += c; cleaned += n; i++; c_run = i + 1; continue; }
                if (c == '/') {
                    c_flush(c_run, i);
                    cleaned += c;
                    c_run = i + 1;
                    in_regex = false;
                    continue;
                }
                continue;
            }
            if (c == '"' || c == '\'') { c_flush(c_run, i); in_str = true; str_char = c; cleaned += c; c_run = i + 1; continue; }
            if (c == '`') { c_flush(c_run, i); in_tmpl = true; cleaned += c; c_run = i + 1; continue; }
            // Detect regex: / followed by non-whitespace in expression context
            if (c == '/' && n != '/' && n != '*' && n != ' ' && n != '\t' && n != '\n') {
                if (!cleaned.empty() && (cleaned.back() == '(' || cleaned.back() == '=' ||
                    cleaned.back() == '!' || cleaned.back() == '&' || cleaned.back() == '|' ||
                    cleaned.back() == '{' || cleaned.back() == ';' || cleaned.back() == ':' ||
                    cleaned.back() == '[' || cleaned.back() == ',' || cleaned.back() == '?' ||
                    cleaned.back() == '~')) {
                    c_flush(c_run, i);
                    in_regex = true;
                    cleaned += c;
                    c_run = i + 1;
                    continue;
                }
            }
            // Remove semicolon before '}'
            if (c == ';' && n == '}') { c_flush(c_run, i); c_run = i + 1; continue; }
        }
        c_flush(c_run, out.size());
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
