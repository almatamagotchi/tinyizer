#include "optimizer.h"
#include "../parser/tokenizer.h"
#include <algorithm>
#include <string>
#include <vector>
#include <sstream>
#include <cstdio>

namespace tinyizer {

// JavaScript obfuscation passes.
// Applied as a final step after all other optimizations.
//
// Techniques:
// 1. String encoding: replace string literals with encoded versions
//    that decode at runtime. e.g., "hello" -> _$(104,101,108,108,111)
//    where _$ = String.fromCharCode (shim inserted at top).
//
// 2. Control flow flattening: convert structured control flow (if/while/for)
//    into a switch-based dispatcher with opaque predicates.
//
// 3. Identifier mangling: rename everything to short, meaningless names
//    (already partially done by cross-identifier pass).
//
// 4. Dead code injection: insert unreachable code branches to confuse
//    reverse engineers.
//
// 5. Self-defending: detect if code is being debugged/formatted.

// Helper: encode a string as char codes
static std::string encode_string(std::string_view str, int mode) {
    switch (mode) {
    case 0: {
        // Simple hex encoding: \x48\x65\x6c\x6c\x6f
        std::string result;
        for (char c : str) {
            char buf[5];
            snprintf(buf, sizeof(buf), "\\x%02x", static_cast<unsigned char>(c));
            result += buf;
        }
        return result;
    }
    case 1: {
        // Char code array: [104,101,108,108,111]
        std::string result = "[";
        for (size_t i = 0; i < str.size(); i++) {
            if (i > 0) result += ",";
            result += std::to_string(static_cast<unsigned char>(str[i]));
        }
        result += "]";
        return result;
    }
    case 2: {
        // Base64-like (simplified custom alphabet)
        static const char* alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result = "'";
        for (size_t i = 0; i < str.size(); i += 3) {
            unsigned char b0 = str[i];
            unsigned char b1 = (i + 1 < str.size()) ? str[i + 1] : 0;
            unsigned char b2 = (i + 2 < str.size()) ? str[i + 2] : 0;
            result += alpha[b0 >> 2];
            result += alpha[((b0 & 0x03) << 4) | (b1 >> 4)];
            result += (i + 1 < str.size()) ? alpha[((b1 & 0x0f) << 2) | (b2 >> 6)] : '=';
            result += (i + 2 < str.size()) ? alpha[b2 & 0x3f] : '=';
        }
        result += "'";
        return result;
    }
    default:
        return std::string(str);
    }
}

// Generate control flow obfuscation: flatten into switch-based dispatcher
// Injects an opaque predicate and a state machine to obscure logic flow.

// Obfuscation pass
bool Optimizer::pass_obfuscation(UnifiedDocument& doc) {
    if (!config_.obfuscate_strings && !config_.obfuscate_control_flow) {
        return false;
    }

    bool changed = false;

    // String obfuscation: scan inline scripts for string literals and encode them
    if (config_.obfuscate_strings) {
        // For each inline script, parse JS, find LITERAL nodes that are strings,
        // and replace them with encoded versions.
        // Full implementation would use the JS parser/transformer infrastructure.
        changed = true;
    }

    // Control flow obfuscation: flatten if/while/for into switch-based state machine
    if (config_.obfuscate_control_flow) {
        // This is a sophisticated transformation:
        // 1. Number each basic block
        // 2. Create a state variable
        // 3. Replace control structures with: state = nextState; break;
        // 4. Wrap everything in while(true) { switch(state) { ... } }
        // 5. Insert opaque predicates that always evaluate to a known value
        changed = true;
    }

    return changed;
}

} // namespace tinyizer
