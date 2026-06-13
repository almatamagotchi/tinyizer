#include "css_parser.h"
#include <cctype>
#include <unordered_set>

namespace tinyizer {

CSSParser::CSSParser(StringPool& pool) : pool_(pool) {}

std::vector<CSSRule> CSSParser::parse(std::string_view css) {
    tok_ = std::make_unique<Tokenizer>(css);
    std::vector<CSSRule> rules;

    while (!tok_->eof()) {
        tok_->skip_whitespace();

        if (tok_->eof()) break;

        char c = tok_->peek();

        // Comment
        if (c == '/' && tok_->peek_ahead(1) == '*') {
            skip_comment();
            continue;
        }

        // CDO/CDC (HTML comment in style tag) — skip
        if (c == '<' || (c == '-' && tok_->peek_ahead(1) == '-' && tok_->peek_ahead(2) == '>')) {
            tok_->advance();
            continue;
        }

        // At-rule
        if (c == '@') {
            tok_->advance();
            std::string_view at_name = tok_->read_identifier();

            CSSRule rule;
            if (parse_at_rule(rule, at_name)) {
                rules.push_back(std::move(rule));
            } else {
                // Unhandled at-rule: skip its block
                if (tok_->peek() == '{') skip_block();
            }
            continue;
        }

        // Regular rule
        if (c == '}' || c == ';') {
            tok_->advance();
            continue;
        }

        CSSRule rule = parse_rule();
        rules.push_back(std::move(rule));
    }

    return rules;
}

CSSRule CSSParser::parse_rule() {
    CSSRule rule;

    // Parse selector
    auto sel = parse_selector();
    if (!sel.empty()) {
        rule.add_selector(std::move(sel));
    }

    // Parse declaration block
    if (tok_->match('{')) {
        while (!tok_->eof()) {
            tok_->skip_whitespace();

            if (tok_->eof()) break;

            if (tok_->peek() == '}') {
                tok_->advance();
                break;
            }

            // Nested rule? This would be for preprocessors like SCSS — skip
            // Check if it looks like a selector (starts with ., #, &, :, or identifier)
            char c = tok_->peek();
            if (c == '.' || c == '#' || c == '&' || c == '*' || c == '[') {
                // Definitely a nested selector — skip the whole nested block
                while (!tok_->eof() && tok_->peek() != '{' && tok_->peek() != '}') tok_->advance();
                if (tok_->peek() == '{') skip_block();
                if (tok_->peek() == '}') { tok_->advance(); break; }
                continue;
            }

            if (c == ':' || is_ident_start(c)) {
                // Could be declaration (ident: value) or pseudo/nested selector
                size_t saved = tok_->pos();
                if (is_ident_start(c)) tok_->read_identifier();
                else tok_->advance(); // ':'
                tok_->skip_whitespace();
                bool is_decl = (tok_->peek() == ':');
                tok_->set_pos(saved);

                if (is_decl) {
                    auto decl = parse_declaration();
                    if (!decl.property.empty()) {
                        rule.add_declaration(std::move(decl));
                    }
                } else {
                    // Nested selector — skip the block
                    while (!tok_->eof() && tok_->peek() != '{' && tok_->peek() != '}') tok_->advance();
                    if (tok_->peek() == '{') skip_block();
                    if (tok_->peek() == '}') { tok_->advance(); break; }
                }
                continue;
            }

            // Comment
            if (c == '/' && tok_->peek_ahead(1) == '*') {
                skip_comment();
                continue;
            }

            auto decl = parse_declaration();
            if (!decl.property.empty()) {
                rule.add_declaration(std::move(decl));
            }
        }
    }

    return rule;
}

bool CSSParser::parse_at_rule(CSSRule& rule, std::string_view at_name) {
    rule.set_at_rule(true);
    rule.set_at_rule_name(pool_.intern(at_name));

    // @import url(...);
    // @charset "utf-8";
    // @namespace url(...);
    // @media (...) { ... }
    // @keyframes name { ... }
    // @font-face { ... }
    // @supports (...) { ... }
    // @page { ... }

    // Store as an at-rule with prelude as first selector
    // For simplicity, store everything from @ until { as a single selector part

    // Read the prelude (everything before {)
    size_t prelude_start = tok_->pos();
    while (!tok_->eof() && tok_->peek() != '{' && tok_->peek() != ';') {
        tok_->advance();
    }
    std::string_view prelude = tok_->substr(prelude_start, tok_->pos());

    // Strip leading/trailing whitespace from the prelude.
    // A space between the at-rule name and `{` should not become a selector.
    while (!prelude.empty() && (prelude[0] == ' ' || prelude[0] == '\t' ||
           prelude[0] == '\r' || prelude[0] == '\n'))
        prelude.remove_prefix(1);
    while (!prelude.empty() && (prelude.back() == ' ' || prelude.back() == '\t' ||
           prelude.back() == '\r' || prelude.back() == '\n'))
        prelude.remove_suffix(1);

    if (!prelude.empty()) {
        CSSRule::SelectorPart part;
        part.type = CSSRule::SelectorPart::Type::ELEMENT;
        part.value = pool_.intern(prelude);
        rule.add_selector_part(part);
        // Commit the selector part to the selectors_ list so the
        // serializer emits it.  (parse_at_rule skips parse_ruleset
        // where add_selector would normally be called.)
        rule.add_selector(std::vector<CSSRule::SelectorPart>(rule.selector_parts()));
    }

    if (tok_->peek() == ';') {
        tok_->advance();
        return true;
    }

    if (tok_->match('{')) {
        // For at-rules known to contain nested rules/blocks (@media, @supports,
        // @keyframes, @document, @scope, @layer, @container), store the raw body
        // text so we can emit it faithfully.  Our declaration model can't
        // represent nested structure.
        static const std::unordered_set<std::string_view> NESTED_AT_RULES = {
            "media", "supports", "keyframes", "-webkit-keyframes",
            "-moz-keyframes", "-o-keyframes", "document", "scope",
            "layer", "container",
        };
        bool store_raw = NESTED_AT_RULES.count(at_name) > 0;

        if (store_raw) {
            // Capture the full body text: prelude + `{...}`.
            // We already consumed the opening `{`.  Walk back to include it
            // in the raw body along with the prelude.
            // body covers everything from the first char after the at-name
            // to the matching `}`.
            size_t body_start = prelude_start;
            // We're currently past the opening `{`.
            // Walk forward from here through the body content.
            int depth = 1;
            while (!tok_->eof() && depth > 0) {
                char c = tok_->advance();
                if (c == '{') depth++;
                else if (c == '}') depth--;
            }
            size_t body_end = tok_->pos();  // one past the closing `}`
            std::string_view raw_body = tok_->substr(body_start, body_end);
            rule.set_raw_body(std::string(raw_body));
            return true;
        }

        // Parse the body — for @font-face / @page, it contains declarations
        while (!tok_->eof()) {
            tok_->skip_whitespace();

            if (tok_->eof()) break;

            if (tok_->peek() == '}') {
                tok_->advance();
                break;
            }

            // Comment
            if (tok_->peek() == '/' && tok_->peek_ahead(1) == '*') {
                skip_comment();
                continue;
            }

            // Try parsing as a declaration first
            size_t saved = tok_->pos();
            auto decl = parse_declaration();
            if (!decl.property.empty()) {
                rule.add_declaration(std::move(decl));
            } else {
                // Not a declaration — might be a nested rule (for @media, @supports)
                // Skip the nested rule content
                tok_->set_pos(saved);
                // Read until { then skip block
                while (!tok_->eof() && tok_->peek() != '{' && tok_->peek() != '}') tok_->advance();
                if (tok_->peek() == '{') {
                    skip_block();
                } else if (tok_->peek() == '}') {
                    break;
                }
            }
        }
        return true;
    }

    return true;
}

std::vector<CSSRule::SelectorPart> CSSParser::parse_selector() {
    std::vector<CSSRule::SelectorPart> parts;

    while (!tok_->eof()) {
        tok_->skip_whitespace();

        if (tok_->eof() || tok_->peek() == '{') break;

        // Combinator or comma (separates selectors)
        char c = tok_->peek();
        if (c == ',') {
            tok_->advance();
            // For now, we just accumulate all parts. Multi-selector handling is in CSSRule.
            tok_->skip_whitespace();
            // Actually, store comma as a separator
            CSSRule::SelectorPart part;
            part.type = CSSRule::SelectorPart::Type::COMBINATOR;
            part.value = pool_.intern(",");
            parts.push_back(part);
            continue;
        }

        if (c == '>' || c == '+' || c == '~') {
            tok_->advance();
            CSSRule::SelectorPart part;
            part.type = CSSRule::SelectorPart::Type::COMBINATOR;
            char buf[2] = {c, '\0'};
            part.value = pool_.intern(std::string_view(buf, 1));
            parts.push_back(part);
            continue;
        }

        auto part = parse_selector_part();
        if (part.type != CSSRule::SelectorPart::Type::ELEMENT || !part.value.empty()) {
            parts.push_back(part);
        }
    }

    return parts;
}

CSSRule::SelectorPart CSSParser::parse_selector_part() {
    CSSRule::SelectorPart part;

    char c = tok_->peek();

    if (c == '#') {
        tok_->advance();
        std::string_view id = tok_->read_identifier();
        part.type = CSSRule::SelectorPart::Type::ID;
        part.value = pool_.intern(id);
    } else if (c == '.') {
        tok_->advance();
        std::string_view cls = tok_->read_identifier();
        part.type = CSSRule::SelectorPart::Type::CLASS;
        part.value = pool_.intern(cls);
    } else if (c == ':') {
        tok_->advance();
        // Could be ::pseudo-element or :pseudo-class
        if (tok_->peek() == ':') tok_->advance();
        std::string_view pseudo = tok_->read_identifier();
        // If pseudo has (), skip them
        if (tok_->peek() == '(') {
            int depth = 1;
            tok_->advance();
            while (!tok_->eof() && depth > 0) {
                if (tok_->peek() == '(') depth++;
                else if (tok_->peek() == ')') depth--;
                if (depth > 0) tok_->advance();
            }
            if (tok_->peek() == ')') tok_->advance();
        }
        part.type = CSSRule::SelectorPart::Type::PSEUDO;
        part.value = pool_.intern(pseudo);
    } else if (c == '[') {
        tok_->advance();
        std::string_view attr = tok_->read_identifier();
        // Skip rest of attribute selector: =, ~=, |=, ^=, $=, *=, value, ]
        while (!tok_->eof() && tok_->peek() != ']') tok_->advance();
        if (tok_->peek() == ']') tok_->advance();
        part.type = CSSRule::SelectorPart::Type::ATTR;
        part.value = pool_.intern(attr);
    } else if (c == '*') {
        tok_->advance();
        part.type = CSSRule::SelectorPart::Type::UNIVERSAL;
        part.value = pool_.intern("*");
    } else if (is_ident_start(c)) {
        std::string_view elem = tok_->read_identifier();
        part.type = CSSRule::SelectorPart::Type::ELEMENT;
        part.value = pool_.intern(elem);
    } else {
        // Unknown, skip
        tok_->advance();
    }

    return part;
}

CSSRule::Declaration CSSParser::parse_declaration() {
    CSSRule::Declaration decl;

    // Property name
    std::string_view prop = tok_->read_identifier();
    if (prop.empty()) return decl;

    tok_->skip_whitespace();
    if (!tok_->match(':')) return decl;

    decl.property = pool_.intern(prop);

    tok_->skip_whitespace();

    // Value: read until ; or }
    size_t value_start = tok_->pos();
    int paren_depth = 0;
    while (!tok_->eof()) {
        char c = tok_->peek();
        if (c == '(') paren_depth++;
        else if (c == ')') paren_depth--;
        else if (c == ';' && paren_depth == 0) break;
        else if (c == '}' && paren_depth == 0) break;
        tok_->advance();
    }

    std::string_view raw_value = tok_->substr(value_start, tok_->pos());
    decl.value = pool_.intern(raw_value);

    // Check for !important
    std::string_view trimmed = raw_value;
    // Find !important
    size_t imp_pos = trimmed.size();
    // Manual search for "!important" or "! important"
    auto is_important_suffix = [](std::string_view sv) {
        // Strip trailing whitespace
        while (!sv.empty() && is_whitespace(sv.back())) sv.remove_suffix(1);
        if (sv.size() >= 10) {
            auto end = sv.substr(sv.size() - 10);
            if (end == "!important") return true;
        }
        if (sv.size() >= 11) {
            auto end = sv.substr(sv.size() - 11);
            if (end == "! important") return true;
        }
        return false;
    };
    if (is_important_suffix(raw_value)) {
        decl.important = true;
    }

    // Consume ; or }
    if (tok_->peek() == ';') tok_->advance();

    return decl;
}

void CSSParser::skip_comment() {
    tok_->match("/*");
    while (!tok_->eof()) {
        if (tok_->peek() == '*' && tok_->peek_ahead(1) == '/') {
            tok_->skip(2);
            return;
        }
        tok_->advance();
    }
}

void CSSParser::skip_block() {
    if (tok_->peek() != '{') {
        // Read until {
        while (!tok_->eof() && tok_->peek() != '{') tok_->advance();
    }
    if (tok_->peek() == '{') {
        int depth = 1;
        tok_->advance();
        while (!tok_->eof() && depth > 0) {
            char c = tok_->advance();
            if (c == '{') depth++;
            else if (c == '}') depth--;
        }
    }
}

} // namespace tinyizer
