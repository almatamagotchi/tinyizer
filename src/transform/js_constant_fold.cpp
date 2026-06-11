#include "optimizer.h"
#include "../parser/js_parser.h"
#include "../ir/js_scope.h"
#include <cctype>
#include <string>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <regex>
#include <algorithm>
#include <functional>

namespace tinyizer {

// JavaScript constant folding and simple peephole optimizations.
//
// Evaluates constant expressions at compile time:
//   - Arithmetic: 2+3 -> 5, 10*0 -> 0, etc.
//   - String concat: "hello " + "world" -> "hello world"
//   - Boolean: !true -> false, !!x -> x (when x is boolean)
//   - Type coercion: +"42" -> 42, !!1 -> true
//   - Known functions: Math.min(3, 5) -> 3
//
// Also handles:
//   - if(false) removal
//   - while(false) removal
//   - return undefined removal
//   - typeof known_value evaluation

static std::string eval_binary(const std::string& left, const std::string& op, const std::string& right) {
    // Try numeric evaluation
    char* end;
    double l = std::strtod(left.c_str(), &end);
    if (*end != '\0') return {}; // not a pure number

    double r = std::strtod(right.c_str(), &end);
    if (*end != '\0') return {};

    double result = 0;

    if (op == "+") result = l + r;
    else if (op == "-") result = l - r;
    else if (op == "*") result = l * r;
    else if (op == "/") {
        if (r == 0) return {};
        result = l / r;
    }
    else if (op == "%") {
        if (r == 0) return {};
        result = std::fmod(l, r);
    }
    else if (op == "**") result = std::pow(l, r);
    else return {};

    // Format result compactly
    if (result == static_cast<long long>(result)) {
        return std::to_string(static_cast<long long>(result));
    }
    // Use %g to strip trailing zeros: 0.750000 -> 0.75
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", result);
    std::string s(buf);
    // Ensure the result doesn't look like an integer when it's fractional:
    // "0.5" is fine; "0" for 0.0 is fine too.
    // But "1e-7" might lose the leading-zero idiom... keep scientific notation.
    return s;
}

static std::string fold_numeric_exprs(const std::string& js) {
    // Fold simple numeric expressions: NUMBER OP NUMBER
    // Use a careful scan to avoid matching inside strings or comments.
    // Assumes input has already been comment-stripped (minify_js_text does that).
    std::string result;
    result.reserve(js.size());
    
    size_t i = 0;
    bool changed = false;
    
    while (i < js.size()) {
        // Skip whitespace
        if (js[i] == ' ' || js[i] == '\t' || js[i] == '\n') {
            result += js[i];
            i++;
            continue;
        }
        
        // Skip string literals
        if (js[i] == '\'' || js[i] == '"' || js[i] == '`') {
            char quote = js[i];
            result += js[i];
            i++;
            while (i < js.size() && js[i] != quote) {
                if (js[i] == '\\' && i+1 < js.size()) {
                    result += js[i];
                    i++;
                }
                result += js[i];
                i++;
            }
            if (i < js.size()) { result += js[i]; i++; }
            continue;
        }
        
        // Try to match NUMBER OP NUMBER
        // Look for a number starting at position i
        size_t start = i;
        bool has_dot = false;
        bool is_hex = false;
        while (i < js.size() && (std::isdigit(js[i]) || js[i] == '.' || 
               (js[i] == 'x' || js[i] == 'X') || 
               (is_hex && std::isxdigit(js[i])))) {
            if (js[i] == 'x' || js[i] == 'X') is_hex = true;
            if (js[i] == '.') has_dot = true;
            i++;
        }
        
        if (i == start) {
            result += js[i];
            i++;
            continue;
        }
        
        std::string left_num(js.begin() + start, js.begin() + i);
        // Fast check: is it a pure decimal number?
        // Only fold decimal numbers for safety (not hex/octal)
        bool left_valid = true;
        for (char c : left_num) {
            if (!std::isdigit(c) && c != '.') { left_valid = false; break; }
        }
        
        if (!left_valid || left_num.empty()) {
            result += left_num;
            continue;
        }
        
        // Save position in case we need to backtrack
        size_t saved_i = i;
        std::string saved_left = left_num;
        
        // Skip whitespace
        while (i < js.size() && (js[i] == ' ' || js[i] == '\t' || js[i] == '\n')) i++;
        
        // Read operator
        if (i >= js.size()) { result += left_num; continue; }
        std::string op;
        char c = js[i];
        if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%') {
            op = std::string(1, c);
        } else if (c == '*' && i+1 < js.size() && js[i+1] == '*') {
            op = "**";
            i++;
        } else {
            // Not an operator
            result += left_num;
            i = saved_i; // backtrack to before whitespace skip
            continue;
        }
        i++;
        
        // Skip whitespace after operator
        while (i < js.size() && (js[i] == ' ' || js[i] == '\t' || js[i] == '\n')) i++;
        
        // Read right operand (must be a number)
        size_t right_start = i;
        while (i < js.size() && (std::isdigit(js[i]) || js[i] == '.')) i++;
        
        if (i == right_start) {
            // No number found — backtrack
            result += left_num + op;
            continue;
        }
        
        std::string right_num(js.begin() + right_start, js.begin() + i);
        bool right_valid = true;
        for (char c : right_num) {
            if (!std::isdigit(c) && c != '.') { right_valid = false; break; }
        }
        
        if (!right_valid || right_num.empty()) {
            // Backtrack: output left number + operator (without whitespace)
            result += left_num;
            i = saved_i;
            continue;
        }
        
        // Evaluate
        std::string folded = eval_binary(left_num, op, right_num);
        if (folded.empty()) {
            // Evaluation failed (e.g., division by zero)
            result += left_num + op + right_num;
            changed = true; // at least we tried
            continue;
        }
        
        result += folded;
        changed = true;
    }
    
    return changed ? result : js;
}

static std::string fold_string_concat(const std::string& js) {
    // Fold adjacent string literals: "a"+"b" -> "ab"
    // This is complex to do safely without AST. 
    // For now, a simple regex-based approach.
    std::string result = js;
    bool changed = true;
    int iterations = 0;
    
    while (changed && iterations < 10) {
        changed = false;
        iterations++;
        std::regex str_concat(R"((['"])([^'"]*)\1\s*\+\s*(['"])([^'"]*)\3)");
        std::smatch m;
        if (std::regex_search(result, m, str_concat)) {
            std::string concat = m[1].str() + m[2].str() + m[4].str() + m[1].str();
            result = m.prefix().str() + concat + m.suffix().str();
            changed = true;
        }
    }
    
    return result;
}

// Find matching closing brace from position after '{'
static size_t find_matching_brace(const std::string& s, size_t open_pos) {
    int depth = 0;
    bool in_string = false;
    char str_char = 0;
    bool in_tmpl = false;
    for (size_t i = open_pos; i < s.size(); i++) {
        char c = s[i];
        if (in_string) {
            if (c == '\\') { i++; continue; }
            if (c == str_char) in_string = false;
            continue;
        }
        if (in_tmpl) {
            if (c == '\\') { i++; continue; }
            if (c == '`') in_tmpl = false;
            continue;
        }
        if (c == '"' || c == '\'') { in_string = true; str_char = c; continue; }
        if (c == '`') { in_tmpl = true; continue; }
        if (c == '{') depth++;
        else if (c == '}') { depth--; if (depth == 0) return i; }
    }
    return std::string::npos;
}

// Find matching semicolon for a statement starting at pos (skips nested braces/blocks)
static size_t find_statement_end(const std::string& s, size_t pos) {
    int brace_depth = 0;
    bool in_string = false;
    char str_char = 0;
    bool in_tmpl = false;
    for (size_t i = pos; i < s.size(); i++) {
        char c = s[i];
        if (in_string) {
            if (c == '\\') { i++; continue; }
            if (c == str_char) in_string = false;
            continue;
        }
        if (in_tmpl) {
            if (c == '\\') { i++; continue; }
            if (c == '`') in_tmpl = false;
            continue;
        }
        if (c == '"' || c == '\'') { in_string = true; str_char = c; continue; }
        if (c == '`') { in_tmpl = true; continue; }
        if (c == '{') brace_depth++;
        else if (c == '}') brace_depth--;
        else if (c == ';' && brace_depth == 0) return i;
    }
    return std::string::npos;
}

// Skip whitespace starting at pos, return position of first non-whitespace char
static size_t skip_ws(const std::string& s, size_t pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) pos++;
    return pos;
}

// Fold dead branches: if(false){...}, if(true){...}, while(false){...}
// Keeps the body for if(true), removes body for if(false)/while(false).
// Also handles if(true)...else{...} and if(false)...else{...}
// Uses manual regex_search loop to correctly handle if/else cascades
// (e.g., if(false)...else if(true)... where the else's if is consumed as part
// of the outer if's else branch, not processed as a separate match).
static std::string fold_dead_branches(const std::string& js) {
    std::string result;
    result.reserve(js.size());
    bool changed = false;

    std::regex cond_re(R"((if|while)\s*\(\s*(true|false|0|1|!0|!1)\s*\))");
    std::smatch m;

    size_t last_end = 0;
    std::string search_str = js;

    while (std::regex_search(search_str, m, cond_re)) {
        size_t match_start = m.position();
        size_t match_end = match_start + m.length();

        // Copy text before match to result
        result += search_str.substr(0, match_start);

        std::string keyword = m[1].str();
        std::string condition = m[2].str();
        bool is_truthy = (condition == "true" || condition == "1" || condition == "!0");
        bool is_falsy  = (condition == "false" || condition == "0" || condition == "!1");

        size_t pos = skip_ws(search_str, match_end);
        if (pos >= search_str.size()) {
            result += m.str();
            search_str = search_str.substr(match_end);
            continue;
        }

        if (search_str[pos] == '{') {
            // Braced block
            size_t close_brace = find_matching_brace(search_str, pos);
            if (close_brace == std::string::npos) {
                result += m.str();
                search_str = search_str.substr(match_end);
                continue;
            }
            size_t block_end = close_brace + 1;

            if (keyword == "while" && is_falsy) {
                changed = true;
                search_str = search_str.substr(block_end);
                continue;
            }
            if (keyword == "while" && is_truthy) {
                result += m.str();
                search_str = search_str.substr(match_end);
                continue;
            }

            std::string body = search_str.substr(pos + 1, close_brace - pos - 1);

            size_t after_block = skip_ws(search_str, block_end);
            bool has_else = (after_block + 4 <= search_str.size() &&
                             search_str.substr(after_block, 4) == "else");

            if (is_truthy) {
                changed = true;
                result += body;
                if (has_else) {
                    size_t else_pos = skip_ws(search_str, after_block + 4);
                    if (else_pos < search_str.size() && search_str[else_pos] == '{') {
                        // else { ... } — discard else block
                        size_t else_close = find_matching_brace(search_str, else_pos);
                        if (else_close != std::string::npos) {
                            search_str = search_str.substr(else_close + 1);
                        } else {
                            search_str = search_str.substr(block_end);
                        }
                    } else if (else_pos + 2 <= search_str.size() &&
                               search_str.substr(else_pos, 2) == "if") {
                        // else if(...)... — discard entire else-if chain
                        // Find end of else-if statement and skip it
                        size_t if_pos = skip_ws(search_str, else_pos + 2);
                        if (if_pos < search_str.size() &&
                            search_str[if_pos] == '(') {
                            int p_depth = 0;
                            size_t paren_close = std::string::npos;
                            for (size_t pi = if_pos; pi < search_str.size(); pi++) {
                                if (search_str[pi] == '(') p_depth++;
                                else if (search_str[pi] == ')') {
                                    if (--p_depth == 0) {
                                        paren_close = pi;
                                        break;
                                    }
                                }
                            }
                            if (paren_close != std::string::npos) {
                                size_t body_start =
                                    skip_ws(search_str, paren_close + 1);
                                if (body_start < search_str.size()) {
                                    if (search_str[body_start] == '{') {
                                        size_t body_close = find_matching_brace(
                                            search_str, body_start);
                                        if (body_close != std::string::npos) {
                                            search_str =
                                                search_str.substr(body_close + 1);
                                        } else {
                                            search_str =
                                                search_str.substr(block_end);
                                        }
                                    } else {
                                        size_t stmt_end = find_statement_end(
                                            search_str, body_start);
                                        search_str = search_str.substr(
                                            (stmt_end != std::string::npos)
                                                ? stmt_end + 1
                                                : block_end);
                                    }
                                } else {
                                    search_str = search_str.substr(block_end);
                                }
                            } else {
                                search_str = search_str.substr(block_end);
                            }
                        } else {
                            search_str = search_str.substr(block_end);
                        }
                    } else {
                        // else simple_statement; — discard it
                        size_t stmt_end = find_statement_end(search_str, else_pos);
                        search_str = search_str.substr(
                            (stmt_end != std::string::npos) ? stmt_end + 1
                                                             : block_end);
                    }
                } else {
                    search_str = search_str.substr(block_end);
                }
                continue;
            }

            if (is_falsy) {
                changed = true;
                if (has_else) {
                    size_t else_pos = skip_ws(search_str, after_block + 4);
                    if (else_pos < search_str.size() && search_str[else_pos] == '{') {
                        // else { ... }
                        size_t else_close = find_matching_brace(search_str, else_pos);
                        if (else_close != std::string::npos) {
                            result += search_str.substr(else_pos + 1,
                                                        else_close - else_pos - 1);
                            search_str = search_str.substr(else_close + 1);
                        } else {
                            search_str = search_str.substr(block_end);
                        }
                    } else if (else_pos + 2 <= search_str.size() &&
                               search_str.substr(else_pos, 2) == "if") {
                        // else if(...){...} or else if(...)stmt; — keep the
                        // entire if construct as-is; next iteration of
                        // fold_dead_branches (called from fold_constants_in_text
                        // loop) will process it.
                        // Find the end of this if statement.
                        size_t if_end = match_end; // will be wrong, compute
                        size_t if_pos = skip_ws(search_str, else_pos + 2);
                        if (if_pos < search_str.size() &&
                            search_str[if_pos] == '(') {
                            // find closing paren
                            int p_depth = 0;
                            size_t paren_close = std::string::npos;
                            for (size_t pi = if_pos; pi < search_str.size(); pi++) {
                                if (search_str[pi] == '(') p_depth++;
                                else if (search_str[pi] == ')') {
                                    if (--p_depth == 0) {
                                        paren_close = pi;
                                        break;
                                    }
                                }
                            }
                            if (paren_close != std::string::npos) {
                                size_t body_start = skip_ws(search_str, paren_close + 1);
                                if (body_start < search_str.size()) {
                                    if (search_str[body_start] == '{') {
                                        size_t body_close =
                                            find_matching_brace(search_str, body_start);
                                        if (body_close != std::string::npos) {
                                            // Copy the else-if text and consume
                                            result += search_str.substr(
                                                else_pos, body_close + 1 - else_pos);
                                            search_str =
                                                search_str.substr(body_close + 1);
                                        }
                                    } else {
                                        size_t stmt_end =
                                            find_statement_end(search_str, body_start);
                                        if (stmt_end != std::string::npos) {
                                            result += search_str.substr(
                                                else_pos, stmt_end + 1 - else_pos);
                                            search_str =
                                                search_str.substr(stmt_end + 1);
                                        } else {
                                            search_str =
                                                search_str.substr(else_pos);
                                        }
                                    }
                                } else {
                                    search_str = search_str.substr(else_pos);
                                }
                            } else {
                                search_str = search_str.substr(else_pos);
                            }
                        } else {
                            search_str = search_str.substr(else_pos);
                        }
                    } else {
                        // else simple_statement;
                        size_t stmt_end = find_statement_end(search_str, else_pos);
                        if (stmt_end != std::string::npos) {
                            result += search_str.substr(else_pos,
                                                        stmt_end - else_pos + 1);
                            search_str = search_str.substr(stmt_end + 1);
                        } else {
                            // No semicolon? Consume to end-of-line or EOF.
                            // Fallback: include remaining text
                            result += search_str.substr(else_pos);
                            search_str.clear();
                        }
                    }
                } else {
                    search_str = search_str.substr(block_end);
                }
                continue;
            }
        } else {
            // Unbraced statement
            size_t stmt_start = pos;
            if (keyword == "while") {
                if (is_falsy) {
                    size_t stmt_end = find_statement_end(search_str, stmt_start);
                    if (stmt_end != std::string::npos) {
                        changed = true;
                        search_str = search_str.substr(stmt_end + 1);
                        continue;
                    }
                }
                result += m.str();
                search_str = search_str.substr(match_end);
                continue;
            }

            size_t stmt_end = find_statement_end(search_str, stmt_start);
            if (stmt_end == std::string::npos) {
                result += m.str();
                search_str = search_str.substr(match_end);
                continue;
            }

            size_t after_stmt = skip_ws(search_str, stmt_end + 1);
            bool has_else = (after_stmt + 4 <= search_str.size() &&
                             search_str.substr(after_stmt, 4) == "else");

            if (is_truthy) {
                changed = true;
                result += search_str.substr(stmt_start, stmt_end - stmt_start + 1);
                if (has_else) {
                    size_t else_pos = skip_ws(search_str, after_stmt + 4);
                    if (else_pos < search_str.size() && search_str[else_pos] == '{') {
                        size_t else_close = find_matching_brace(search_str, else_pos);
                        search_str = search_str.substr(
                            (else_close != std::string::npos) ? else_close + 1
                                                               : stmt_end + 1);
                    } else {
                        size_t else_stmt_end = find_statement_end(search_str, else_pos);
                        search_str = search_str.substr(
                            (else_stmt_end != std::string::npos) ? else_stmt_end + 1
                                                                  : stmt_end + 1);
                    }
                } else {
                    search_str = search_str.substr(stmt_end + 1);
                }
                continue;
            }

            if (is_falsy) {
                changed = true;
                if (has_else) {
                    size_t else_pos = skip_ws(search_str, after_stmt + 4);
                    if (else_pos < search_str.size() && search_str[else_pos] == '{') {
                        size_t else_close = find_matching_brace(search_str, else_pos);
                        if (else_close != std::string::npos) {
                            result += search_str.substr(else_pos + 1,
                                                        else_close - else_pos - 1);
                            search_str = search_str.substr(else_close + 1);
                        } else {
                            search_str = search_str.substr(stmt_end + 1);
                        }
                    } else {
                        size_t else_stmt_end = find_statement_end(search_str, else_pos);
                        if (else_stmt_end != std::string::npos) {
                            result += search_str.substr(else_pos,
                                                        else_stmt_end - else_pos + 1);
                            search_str = search_str.substr(else_stmt_end + 1);
                        } else {
                            search_str = search_str.substr(stmt_end + 1);
                        }
                    }
                } else {
                    search_str = search_str.substr(stmt_end + 1);
                }
                continue;
            }
        }

        result += m.str();
        search_str = search_str.substr(match_end);
    }

    result += search_str;
    return changed ? result : js;
}

static std::string fold_unary_ops(const std::string& js) {
    // !true -> false, !false -> true, !0 -> true, !1 -> false
    // !"x" -> false, !"" -> true
    // !!0 -> false, !!1 -> true, !!"" -> false, !!"." -> true
    // !!true -> true, !!false -> false
    std::string result = js;
    bool changed = true;
    int iterations = 0;
    
    while (changed && iterations < 20) {
        changed = false;
        iterations++;
        
        // !!true -> true, !!false -> false
        std::regex dd_not_true(R"(!\s*!\s*true)");
        if (std::regex_search(result, dd_not_true)) {
            result = std::regex_replace(result, dd_not_true, "true");
            changed = true; continue;
        }
        std::regex dd_not_false(R"(!\s*!\s*false)");
        if (std::regex_search(result, dd_not_false)) {
            result = std::regex_replace(result, dd_not_false, "false");
            changed = true; continue;
        }
        
        // !!0 -> false, !!1 -> true
        std::regex dd_not_0(R"(!\s*!\s*0\b)");
        if (std::regex_search(result, dd_not_0)) {
            result = std::regex_replace(result, dd_not_0, "false");
            changed = true; continue;
        }
        std::regex dd_not_1(R"(!\s*!\s*1\b)");
        if (std::regex_search(result, dd_not_1)) {
            result = std::regex_replace(result, dd_not_1, "true");
            changed = true; continue;
        }
        
        // String-based ! and !! folding removed: unsafe without string-boundary awareness.
        // Patterns like !"x" could match inside string literals (e.g., "Hello!").
        // The follow-on fold_constants_in_text() call handles these at a different stage.
        
        // !true -> false
        std::regex not_true(R"(!\s*true)");
        if (std::regex_search(result, not_true)) {
            result = std::regex_replace(result, not_true, "false");
            changed = true; continue;
        }
        
        // !false -> true
        std::regex not_false(R"(!\s*false)");
        if (std::regex_search(result, not_false)) {
            result = std::regex_replace(result, not_false, "true");
            changed = true; continue;
        }
        
        // !0 -> true
        std::regex not_0(R"(!\s*0\b)");
        if (std::regex_search(result, not_0)) {
            result = std::regex_replace(result, not_0, "true", std::regex_constants::format_first_only);
            changed = true; continue;
        }
        
        // !1 -> false
        std::regex not_1(R"(!\s*1\b)");
        if (std::regex_search(result, not_1)) {
            result = std::regex_replace(result, not_1, "false", std::regex_constants::format_first_only);
            changed = true; continue;
        }
    }
    
    return result;
}

// Strip leading zero from decimal literals: 0.5->.5, 0.123->.123
// The leading zero before a decimal point is never required in JS.
// Only matches standalone "0." followed by digits, not inside strings.
static std::string strip_leading_zero(const std::string& js) {
    std::string result;
    result.reserve(js.size());
    bool in_string = false;
    char str_char = 0;
    bool in_tmpl = false;
    bool in_regex = false;

    for (size_t i = 0; i < js.size(); i++) {
        char c = js[i];
        char n = (i + 1 < js.size()) ? js[i + 1] : 0;

        if (in_string) {
            if (c == '\\') { result += c; result += n; i++; continue; }
            if (c == str_char) in_string = false;
            result += c; continue;
        }
        if (in_tmpl) {
            if (c == '\\') { result += c; result += n; i++; continue; }
            if (c == '`') in_tmpl = false;
            result += c; continue;
        }
        if (in_regex) {
            if (c == '\\') { result += c; result += n; i++; continue; }
            if (c == '/') in_regex = false;
            result += c; continue;
        }
        if (c == '"' || c == '\'') { in_string = true; str_char = c; result += c; continue; }
        if (c == '`') { in_tmpl = true; result += c; continue; }
        if (c == '/' && n != '/' && n != '*') {
            char prev = result.empty() ? 0 : result.back();
            if (prev == '(' || prev == '=' || prev == '!' || prev == '&' ||
                prev == '|' || prev == '{' || prev == ';' || prev == ':' ||
                prev == '[' || prev == ',' || prev == '?' || prev == '~') {
                in_regex = true;
                result += c;
                continue;
            }
        }

        // Detect "0." pattern: leading zero before a decimal point
        // Must be preceded by a non-identifier character to avoid
        // matching inside longer hex/octal numbers or identifiers.
        if (c == '0' && n == '.') {
            char p = result.empty() ? 0 : result.back();
            bool preceded_by_non_ident = result.empty() ||
                !(std::isalnum(static_cast<unsigned char>(p)) || p == '_' || p == '$');
            if (preceded_by_non_ident) {
                result += '.'; // skip the '0', emit just '.'
                i++; // skip the '0' we consumed and the '.' we'll hit on next loop
                continue;
            }
        }

        result += c;
    }
    return result;
}

// Remove unnecessary parentheses around function calls and simple identifiers.
 // return(x) -> return x, return (a?b:c) -> return a?b:c.
// Removes outer parens from any return expression where parens are unnecessary.
// Skips parens inside strings, comments, and regex literals.
static std::string strip_unnecessary_parens(const std::string& js) {
    std::string result;
    result.reserve(js.size());

    enum { NORMAL, DQUOTE, SQUOTE, TEMPLATE, LINE_COMMENT, BLOCK_COMMENT, REGEX } state = NORMAL;

    for (size_t i = 0; i < js.size(); ++i) {
        char c = js[i];
        char nc = (i + 1 < js.size()) ? js[i + 1] : 0;

        // State transitions
        if (state == NORMAL) {
            if (c == '/' && nc == '/') { state = LINE_COMMENT; result += c; continue; }
            if (c == '/' && nc == '*') { state = BLOCK_COMMENT; result += c; result += nc; ++i; continue; }
            if (c == '"') { state = DQUOTE; result += c; continue; }
            if (c == '\'') { state = SQUOTE; result += c; continue; }
            if (c == '`') { state = TEMPLATE; result += c; continue; }
            if (c == '/' && !result.empty() && strchr(")}];", result.back())) {
                // Heuristic: / after } ) ] ; is likely regex
                state = REGEX;
                result += c;
                continue;
            }

            // Look for return (...)
            if (c == 'r' && i + 6 < js.size()) {
                const char* rest = js.c_str() + i;
                if (strncmp(rest, "return", 6) == 0) {
                    size_t pos = i + 6;
                    // skip whitespace
                    while (pos < js.size() && (js[pos] == ' ' || js[pos] == '\t')) ++pos;
                    if (pos < js.size() && js[pos] == '(') {
                        // Find matching close paren
                        int depth = 1;
                        size_t close = pos + 1;
                        while (close < js.size() && depth > 0) {
                            char cc = js[close];
                            if (cc == '(') ++depth;
                            else if (cc == ')') --depth;
                            ++close;
                        }
                        if (depth == 0) {
                            --close; // point to ')'
                            // Check what follows: ; } newline or eof
                            size_t after = close + 1;
                            while (after < js.size() && (js[after] == ' ' || js[after] == '\t')) ++after;
                            bool at_end = (after >= js.size() || js[after] == ';' || js[after] == '}' || js[after] == '\n' || js[after] == '\r');
                            if (at_end) {
                                result += std::string(js.c_str() + i, 6); // "return"
                                while (pos < close && (js[pos] == ' ' || js[pos] == '\t')) ++pos; // skip ws + '('
                                ++pos; // skip '('
                                while (pos < close && (js[pos] == ' ' || js[pos] == '\t')) ++pos; // skip ws after '('
                                // Append inner expression without outer parens
                                result += ' ';
                                result += std::string(js.c_str() + pos, close - pos);
                                i = close; // advance past ')'
                                // Skip trailing whitespace
                                while (i + 1 < js.size() && (js[i + 1] == ' ' || js[i + 1] == '\t')) ++i;
                                continue;
                            }
                        }
                    }
                }
            }
            result += c;
        } else if (state == LINE_COMMENT) {
            if (c == '\n') { state = NORMAL; }
            result += c;
        } else if (state == BLOCK_COMMENT) {
            if (c == '*' && nc == '/') { state = NORMAL; result += c; result += nc; ++i; continue; }
            result += c;
        } else if (state == DQUOTE) {
            if (c == '\\') { result += c; if (nc) { result += nc; ++i; } continue; }
            if (c == '"') state = NORMAL;
            result += c;
        } else if (state == SQUOTE) {
            if (c == '\\') { result += c; if (nc) { result += nc; ++i; } continue; }
            if (c == '\'') state = NORMAL;
            result += c;
        } else if (state == TEMPLATE) {
            if (c == '`') state = NORMAL;
            result += c;
        } else if (state == REGEX) {
            if (c == '\\') { result += c; if (nc) { result += nc; ++i; } continue; }
            if (c == '/') state = NORMAL;
            result += c;
        }
    }
    return result;
} // anonymous namespace

// Walk AST, find const-bound identifier references, and replace them with
// their literal values in the text. Re-parses to get a fresh scope tree
// so const_value data is accurate for the current JS state.
std::string propagate_const_literals(const std::string& js) {
    if (js.empty()) return js;

    // Re-parse to get fresh AST and scope tree
    JSScope scope(JSScope::Kind::GLOBAL);
    JSParser parser;
    auto ast = parser.parse(js, &scope);
    if (!ast) return js;

    // Walk AST to find const-bound identifier references
    struct Replacement {
        size_t start;
        size_t end;
        std::string value;
    };
    std::vector<Replacement> reps;

    std::function<void(const JSNode*)> walk = [&](const JSNode* node) {
        if (!node) return;
        if (node->type == JSNodeType::IDENTIFIER && node->is_reference) {
            if (node->scope) {
                auto* var = node->scope->find_variable(node->value);
                if (var && !var->const_value.empty()) {
                    reps.push_back({
                        node->src_start,
                        node->src_end,
                        std::string(var->const_value)
                    });
                }
            }
        }
        for (const auto& child : node->children) {
            walk(child.get());
        }
    };
    walk(ast.get());

    if (reps.empty()) return js;

    // Sort descending by start position so replacements don't shift
    // positions of remaining items.
    std::sort(reps.begin(), reps.end(),
              [](const Replacement& a, const Replacement& b) {
                  return a.start > b.start;
              });

    std::string result = js;
    for (const auto& r : reps) {
        result.replace(r.start, r.end - r.start, r.value);
    }

    return result;
}

std::string fold_constants_in_text(const std::string& js) {
    // Apply all text-based constant folding passes
    if (js.empty()) return js;
    
    std::string result = js;
    bool any_change = true;
    int iterations = 0;
    
    while (any_change && iterations < 10) {
        any_change = false;
        iterations++;
        
        // Dead branch folding: if(false){}, if(true){}, while(false){}
        std::string folded = fold_dead_branches(result);
        if (folded != result) {
            any_change = true;
            result = std::move(folded);
            continue;
        }
        
        // Numeric folding: 2+3 -> 5
        folded = fold_numeric_exprs(result);
        if (folded != result) {
            any_change = true;
            result = std::move(folded);
            continue;
        }
        
        // String concat folding
        folded = fold_string_concat(result);
        if (folded != result) {
            any_change = true;
            result = std::move(folded);
            continue;
        }
        
        // Unary ops folding (!, !! on constants and string literals)
        folded = fold_unary_ops(result);
        if (folded != result) {
            any_change = true;
            result = std::move(folded);
            continue;
        }
        
        // Remove unnecessary parentheses: return(x) -> return x
        folded = strip_unnecessary_parens(result);
        if (folded != result) {
            any_change = true;
            result = std::move(folded);
            continue;
        }

        // Strip leading zero from decimal literals: 0.5 -> .5
        folded = strip_leading_zero(result);
        if (folded != result) {
            any_change = true;
            result = std::move(folded);
        }
    }
    
    return result;
}

bool Optimizer::pass_js_constant_fold(UnifiedDocument& doc) {
    bool changed = false;

    // Fold constants in optimized_js (standalone JS mode output).
    // Inline scripts are folded later during the JS loop (after dead-code
    // elimination), since varying-length folds break AST source offsets.
    for (size_t idx = 0; idx < doc.optimized_js.size(); idx++) {
        std::string folded = fold_constants_in_text(doc.optimized_js[idx]);
        if (folded != doc.optimized_js[idx]) {
            doc.optimized_js[idx] = std::move(folded);
            changed = true;
        }
    }

    return changed;
}

} // namespace tinyizer
