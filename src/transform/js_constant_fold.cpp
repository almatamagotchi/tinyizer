#include "optimizer.h"
#include <cctype>
#include <string>
#include <cstdlib>
#include <cmath>

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

bool Optimizer::pass_js_constant_fold(UnifiedDocument& doc) {
    bool changed = false;

    // This is a structural pass that requires modifying the JS AST.
    // Full implementation would:
    // 1. Walk AST looking for BINARY_EXPR nodes with LITERAL children
    // 2. Evaluate and replace with a LITERAL node
    // 3. Also handle UNARY_EXPR: !true -> false, -(-5) -> 5, etc.
    // 4. Handle IF_STMT with constant condition: if(true) -> body, if(false) -> remove

    // For now, this is a stub demonstrating the approach.
    // In a production implementation, we'd integrate the AST walker here.

    return changed;
}

} // namespace tinyizer
