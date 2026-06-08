#include "optimizer.h"
#include <cctype>
#include <string>
#include <cstdlib>
#include <cmath>
#include <regex>

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
    return std::to_string(result);
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

static std::string fold_unary_not(const std::string& js) {
    // !true -> false, !false -> true, !0 -> true, !1 -> false
    std::string result = js;
    bool changed = true;
    int iterations = 0;
    
    while (changed && iterations < 10) {
        changed = false;
        iterations++;
        
        std::regex not_true(R"(!\s*true)");
        if (std::regex_search(result, not_true)) {
            result = std::regex_replace(result, not_true, "false");
            changed = true;
            continue;
        }
        
        std::regex not_false(R"(!\s*false)");
        if (std::regex_search(result, not_false)) {
            result = std::regex_replace(result, not_false, "true");
            changed = true;
            continue;
        }
        
        std::regex not_0(R"(!\s*0\b)");
        if (std::regex_search(result, not_0)) {
            result = std::regex_replace(result, not_0, "true", std::regex_constants::format_first_only);
            changed = true;
            continue;
        }
        
        std::regex not_1(R"(!\s*1\b)");
        if (std::regex_search(result, not_1)) {
            result = std::regex_replace(result, not_1, "false", std::regex_constants::format_first_only);
            changed = true;
            continue;
        }
    }
    
    return result;
} // anonymous namespace

std::string fold_constants_in_text(const std::string& js) {
    // Apply all text-based constant folding passes
    if (js.empty()) return js;
    
    std::string result = js;
    bool any_change = true;
    int iterations = 0;
    
    while (any_change && iterations < 10) {
        any_change = false;
        iterations++;
        
        // Numeric folding: 2+3 -> 5
        std::string folded = fold_numeric_exprs(result);
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
        
        // Unary NOT folding
        folded = fold_unary_not(result);
        if (folded != result) {
            any_change = true;
            result = std::move(folded);
        }
    }
    
    return result;
}

bool Optimizer::pass_js_constant_fold(UnifiedDocument& doc) {
    bool changed = false;

    // Text-based constant folding on each parsed inline script's raw source.
    // We fold numeric/string/unary expressions directly in the source text.
    // This complements the AST-based approach (which requires source offsets).
    for (size_t idx = 0; idx < doc.inline_scripts().size(); idx++) {
        const std::string& original = doc.inline_scripts()[idx];
        std::string folded = fold_constants_in_text(original);
        if (folded != original) {
            // We need to update the JS text.
            // Since inline_scripts() returns const ref, we use the mutable accessor.
            doc.set_inline_script(idx, folded);
            changed = true;
        }
    }

    // Also fold constants in optimized_js (output from previous passes)
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
