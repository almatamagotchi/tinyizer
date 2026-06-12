#include "optimizer.h"
#include "serializer.h"
#include "../parser/tokenizer.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

// CSS shorthand property merging and value minification
namespace tinyizer {

// Known shorthand properties and their longhand components
static const std::unordered_map<std::string_view, std::vector<std::string_view>> SHORTHAND_MAP = {
    {"margin",       {"margin-top", "margin-right", "margin-bottom", "margin-left"}},
    {"padding",      {"padding-top", "padding-right", "padding-bottom", "padding-left"}},
    {"border",       {"border-width", "border-style", "border-color"}},
    {"border-top",   {"border-top-width", "border-top-style", "border-top-color"}},
    {"border-right", {"border-right-width", "border-right-style", "border-right-color"}},
    {"border-bottom",{"border-bottom-width", "border-bottom-style", "border-bottom-color"}},
    {"border-left",  {"border-left-width", "border-left-style", "border-left-color"}},
    {"border-width", {"border-top-width", "border-right-width", "border-bottom-width", "border-left-width"}},
    {"border-style", {"border-top-style", "border-right-style", "border-bottom-style", "border-left-style"}},
    {"border-color", {"border-top-color", "border-right-color", "border-bottom-color", "border-left-color"}},
    {"outline",      {"outline-width", "outline-style", "outline-color"}},
    {"overflow",     {"overflow-x", "overflow-y"}},
    {"border-radius",{"border-top-left-radius", "border-top-right-radius", "border-bottom-right-radius", "border-bottom-left-radius"}},
    {"background",   {"background-color", "background-image", "background-repeat",
                      "background-attachment", "background-position", "background-size"}},
    {"font",         {"font-style", "font-variant", "font-weight", "font-size",
                      "line-height", "font-family"}},
    {"list-style",   {"list-style-type", "list-style-position", "list-style-image"}},
    {"text-decoration", {"text-decoration-line", "text-decoration-style", "text-decoration-color", "text-decoration-thickness"}},
    {"flex",         {"flex-grow", "flex-shrink", "flex-basis"}},
    {"transition",   {"transition-property", "transition-duration", "transition-timing-function", "transition-delay"}},
    {"animation",    {"animation-name", "animation-duration", "animation-timing-function",
                      "animation-delay", "animation-iteration-count", "animation-direction",
                      "animation-fill-mode", "animation-play-state"}},
    {"gap",          {"row-gap", "column-gap"}},
    {"flex-flow",    {"flex-direction", "flex-wrap"}},
    {"columns",      {"column-width", "column-count"}},
    {"inset",        {"top", "right", "bottom", "left"}},
    {"inset-block",  {"inset-block-start", "inset-block-end"}},
    {"inset-inline", {"inset-inline-start", "inset-inline-end"}},
    {"place-content", {"align-content", "justify-content"}},
    {"place-items",   {"align-items", "justify-items"}},
    {"place-self",    {"align-self", "justify-self"}},
    {"text-emphasis", {"text-emphasis-style", "text-emphasis-color"}},
    {"scroll-margin", {"scroll-margin-top", "scroll-margin-right", "scroll-margin-bottom", "scroll-margin-left"}},
    {"scroll-padding", {"scroll-padding-top", "scroll-padding-right", "scroll-padding-bottom", "scroll-padding-left"}},
    {"mask",         {"mask-image", "mask-position", "mask-size", "mask-repeat",
                      "mask-origin", "mask-clip", "mask-mode", "mask-composite"}},
    {"overflow-block", {"overflow-block-start", "overflow-block-end"}},
    {"overflow-inline", {"overflow-inline-start", "overflow-inline-end"}},
    {"grid-area",    {"grid-row-start", "grid-column-start", "grid-row-end", "grid-column-end"}},
    {"grid-row",     {"grid-row-start", "grid-row-end"}},
    {"grid-column",  {"grid-column-start", "grid-column-end"}},
    {"font-variant", {"font-variant-caps", "font-variant-numeric", "font-variant-alternates",
                       "font-variant-ligatures", "font-variant-east-asian", "font-variant-position",
                       "font-variant-emoji"}},
};

// CSS value minification helpers
// Try to constant-fold a simple calc expression: "10px + 20px" → "30px"
// Returns empty string if folding isn't possible (mixed units, complex expression, etc.)
static std::string try_fold_calc(const std::string& expr) {
    // Find the operator: only handle single + or - at top level
    size_t op_pos = std::string::npos;
    char op_char = 0;
    int depth = 0;
    for (size_t i = 0; i < expr.size(); i++) {
        char c = expr[i];
        if (c == '(') depth++;
        else if (c == ')') depth--;
        else if (depth == 0 && (c == '+' || c == '-')) {
            // Skip leading sign
            if (i > 0 && expr[i-1] != 'e' && expr[i-1] != 'E' && expr[i-1] != '*' && expr[i-1] != '/') {
                op_pos = i;
                op_char = c;
                break;
            }
        }
        else if (depth == 0 && (c == '*' || c == '/')) return ""; // too complex
    }
    if (op_pos == std::string::npos) return ""; // no operator found

    std::string left = expr.substr(0, op_pos);
    std::string right = expr.substr(op_pos + 1);

    // Trim whitespace
    auto trim = [](std::string& s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0,1);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    };
    trim(left);
    trim(right);
    if (left.empty() || right.empty()) return "";

    // Parse numeric value and optional unit from a string like "10.5px"
    auto parse_num_unit = [](const std::string& s, double& num, std::string& unit) -> bool {
        size_t pos = 0;
        // Skip leading sign if present (for subtraction edge cases)
        if (pos < s.size() && s[pos] == '-') pos++;
        bool has_digit = false;
        while (pos < s.size() && (std::isdigit(static_cast<unsigned char>(s[pos])) || s[pos] == '.')) {
            has_digit = true;
            pos++;
        }
        if (!has_digit) return false;
        // Use strtod for conversion (no exceptions)
        char* end = nullptr;
        num = std::strtod(s.c_str(), &end);
        if (end == s.c_str()) return false;
        unit = std::string(end);
        // Trim whitespace from unit
        while (!unit.empty() && std::isspace(static_cast<unsigned char>(unit.front()))) unit.erase(0,1);
        while (!unit.empty() && std::isspace(static_cast<unsigned char>(unit.back()))) unit.pop_back();
        return true;
    };

    double left_num, right_num;
    std::string left_unit, right_unit;
    if (!parse_num_unit(left, left_num, left_unit)) return "";
    if (!parse_num_unit(right, right_num, right_unit)) return "";

    // Only fold when units match (or both unitless)
    if (left_unit != right_unit) return "";

    double result;
    if (op_char == '+') result = left_num + right_num;
    else result = left_num - right_num;

    // Format result: remove trailing zeros to save bytes
    std::string result_str;
    if (result == static_cast<long long>(result)) {
        result_str = std::to_string(static_cast<long long>(result));
    } else {
        result_str = std::to_string(result);
        // Remove trailing zeros
        while (result_str.find('.') != std::string::npos && result_str.size() > 1) {
            char last = result_str.back();
            if (last == '0') result_str.pop_back();
            else if (last == '.') { result_str.pop_back(); break; }
            else break;
        }
    }

    return result_str + left_unit;
}

std::string minify_css_value(const std::string& value) {
    // Named colors shorter than their hex representation
    static const std::unordered_map<std::string, std::string> NAMED_COLOR = {
        {"ff0000","red"},{"d2b48c","tan"},
        {"808080","gray"},{"000080","navy"},{"008080","teal"},{"ffd700","gold"},
        {"cd853f","peru"},{"dda0dd","plum"},{"ffc0cb","pink"},{"fffafa","snow"},
        {"f5deb3","wheat"},{"ff7f50","coral"},{"f0e68c","khaki"},{"faf0e6","linen"},
        {"808000","olive"},{"008000","green"},{"f5f5dc","beige"},{"fffff0","ivory"},
        {"a52a2a","brown"},{"fa8072","salmon"},{"a0522d","sienna"},{"4b0082","indigo"},
        {"ee82ee","violet"},{"da70d6","orchid"},{"ff6347","tomato"},{"ffa500","orange"},
        {"800080","purple"},{"800000","maroon"},
        {"f0ffff","azure"},{"ffe4c4","bisque"},{"c0c0c0","silver"},
    };

    std::string result;
    result.reserve(value.size());

    // Minify colors: #ffffff -> #fff, #ff0000 -> red (sometimes), rgb(0,0,0) -> #000
    // Minify numbers: 0px -> 0, 0.5 -> .5, 1.0 -> 1
    // Minify keyword values where shorter form exists

    // Strip unnecessary calc() wrapper: calc(100%) → 100%, calc(var(--x)) → var(--x)
    if (value.size() > 6 && value.substr(0,5) == "calc(" && value.back() == ')') {
        std::string inner = value.substr(5, value.size() - 6);
        bool is_simple = true;
        int depth = 0;
        for (size_t k = 0; k < inner.size() && is_simple; k++) {
            char ck = inner[k];
            if (ck == '(') depth++;
            else if (ck == ')') { if (depth == 0) is_simple = false; else depth--; }
            else if (ck == '+' || ck == '-' || ck == '*' || ck == '/') {
                if (ck != '-' || (k > 0 && inner[k-1] != 'e' && inner[k-1] != 'E')) is_simple = false;
            }
        }
        if (is_simple && depth == 0) {
            return minify_css_value(inner);
        }
        // Constant-fold simple calc expressions: calc(10px + 20px) → 30px
        if (depth == 0) {
            std::string folded = try_fold_calc(inner);
            if (!folded.empty()) return minify_css_value(folded);
        }
    }

    size_t i = 0;
    while (i < value.size()) {
        // Check for hex color
        if (i + 6 < value.size() && value[i] == '#') {
            bool is_hex = true;
            for (size_t j = 1; j <= 6; j++) {
                if (!is_hex_digit(value[i + j])) { is_hex = false; break; }
            }
            if (is_hex) {
                // Try to shorten: #aabbcc -> #abc
                std::string hex = value.substr(i + 1, 6);
                if (hex[0] == hex[1] && hex[2] == hex[3] && hex[4] == hex[5]) {
                    std::string short_hex = {hex[0], hex[2], hex[4]};
                    // Map short hex to named color when keyword is shorter
                    auto nc = NAMED_COLOR.find(hex);
                    if (nc != NAMED_COLOR.end() && nc->second.size() < 4) {
                        result += nc->second;
                    } else {
                        result += '#';
                        result += short_hex;
                    }
                    i += 7;
                    continue;
                }
                // Map full hex to named color when keyword is shorter
                auto nc = NAMED_COLOR.find(hex);
                if (nc != NAMED_COLOR.end() && nc->second.size() < 7) {
                    result += nc->second;
                } else {
                    result += value.substr(i, 7);
                }
                i += 7;
                continue;
            }
        }

        // Check for rgb() / rgba()
        if (i + 4 < value.size() && tolower(value[i]) == 'r' && tolower(value[i+1]) == 'g' && tolower(value[i+2]) == 'b') {
            bool is_rgba = false;
            size_t j = i + 3; // past "rgb"
            if (j < value.size() && tolower(value[j]) == 'a') { is_rgba = true; j++; }
            if (j < value.size() && value[j] == '(') {
                j++; // past '('
                int channels[4] = {0, 0, 0, 255};
                int channel_idx = 0;
                int max_channels = is_rgba ? 4 : 3;
                bool parse_ok = true;

                for (channel_idx = 0; channel_idx < max_channels && parse_ok; channel_idx++) {
                    while (j < value.size() && is_whitespace(value[j])) j++;
                    if (j >= value.size()) { parse_ok = false; break; }

                    // Parse integer, float, or percentage
                    int int_part = 0, frac_part = 0, frac_div = 1;
                    bool has_digits = false;

                    if (j < value.size() && is_digit(value[j])) {
                        has_digits = true;
                        while (j < value.size() && is_digit(value[j])) {
                            int_part = int_part * 10 + (value[j] - '0');
                            j++;
                        }
                    }

                    // Fractional part (for alpha floats like 0.5)
                    if (j < value.size() && value[j] == '.') {
                        j++;
                        while (j < value.size() && is_digit(value[j])) {
                            frac_part = frac_part * 10 + (value[j] - '0');
                            frac_div *= 10;
                            has_digits = true;
                            j++;
                        }
                    }

                    if (!has_digits) { parse_ok = false; break; }

                    // Percentage?
                    bool is_pct = false;
                    if (j < value.size() && value[j] == '%') {
                        is_pct = true;
                        j++;
                    }

                    // Convert to int
                    if (channel_idx < 3) {
                        if (is_pct) {
                            channels[channel_idx] = (int_part * 255 + 50) / 100;
                        } else {
                            channels[channel_idx] = int_part;
                        }
                        channels[channel_idx] = std::max(0, std::min(255, channels[channel_idx]));
                    } else {
                        // Alpha: float in range 0-1
                        if (is_pct) {
                            channels[channel_idx] = (int_part * 255 + 50) / 100;
                        } else if (frac_div > 1) {
                            double alpha_f = int_part + (double)frac_part / frac_div;
                            channels[channel_idx] = (int)(alpha_f * 255 + 0.5);
                        } else {
                            channels[channel_idx] = std::min(255, int_part * 255);
                        }
                        channels[channel_idx] = std::max(0, std::min(255, channels[channel_idx]));
                    }

                    // Skip whitespace then comma or )
                    while (j < value.size() && is_whitespace(value[j])) j++;
                    if (j < value.size() && value[j] == ',') { j++; continue; }
                    if (j < value.size() && value[j] == ')') { j++; break; }
                    // No comma found but not at closing paren — may be next value or error
                    if (j >= value.size() || value[j] != ')') { parse_ok = false; break; }
                }

                // Ensure closing paren
                while (j < value.size() && is_whitespace(value[j])) j++;
                if (j < value.size() && value[j] == ')') j++;

                // For loop broke out on ')', so channel_idx wasn't incremented.
                // channels_parsed = channel_idx + 1 (the increment that never happened)
                if (parse_ok && channel_idx + 1 >= (is_rgba ? 4 : 3)) {
                    // Build hex output
                    static const char hexdigits[] = "0123456789abcdef";
                    char hex[10];
                    hex[0] = '#';
                    hex[1] = hexdigits[(channels[0] >> 4) & 0xF];
                    hex[2] = hexdigits[channels[0] & 0xF];
                    hex[3] = hexdigits[(channels[1] >> 4) & 0xF];
                    hex[4] = hexdigits[channels[1] & 0xF];
                    hex[5] = hexdigits[(channels[2] >> 4) & 0xF];
                    hex[6] = hexdigits[channels[2] & 0xF];

                    if (is_rgba && channels[3] < 255) {
                        hex[7] = hexdigits[(channels[3] >> 4) & 0xF];
                        hex[8] = hexdigits[channels[3] & 0xF];
                        hex[9] = '\0';
                        // 8-digit hex shortening: #aabbccdd -> #abcd
                        if (hex[1] == hex[2] && hex[3] == hex[4] && hex[5] == hex[6] && hex[7] == hex[8]) {
                            result += '#';
                            result += hex[1];
                            result += hex[3];
                            result += hex[5];
                            result += hex[7];
                        } else {
                            result += hex;
                        }
                    } else {
                        hex[7] = '\0';
                        // 6-digit hex shortening: #aabbcc -> #abc
                        if (hex[1] == hex[2] && hex[3] == hex[4] && hex[5] == hex[6]) {
                            // Map to named color when shorter
                            std::string hex6 = {hex[1], hex[1], hex[3], hex[3], hex[5], hex[5]};
                            auto nc = NAMED_COLOR.find(hex6);
                            if (nc != NAMED_COLOR.end() && nc->second.size() < 4) {
                                result += nc->second;
                            } else {
                                result += '#';
                                result += hex[1];
                                result += hex[3];
                                result += hex[5];
                            }
                        } else {
                            std::string hex6(hex + 1, 6);
                            auto nc = NAMED_COLOR.find(hex6);
                            if (nc != NAMED_COLOR.end() && nc->second.size() < 7) {
                                result += nc->second;
                            } else {
                                result += hex;
                            }
                        }
                    }
                    i = j;
                    continue;
                }
            }
        }

        // Check for hsl() / hsla()
        if (i + 4 < value.size() && tolower(value[i]) == 'h' && tolower(value[i+1]) == 's' && tolower(value[i+2]) == 'l') {
            bool is_hsla = false;
            size_t j = i + 3; // past "hsl"
            if (j < value.size() && tolower(value[j]) == 'a') { is_hsla = true; j++; }
            if (j < value.size() && value[j] == '(') {
                j++; // past '('
                int h = 0, s = 0, l = 0, a = 255;
                int channel_idx = 0;
                int max_channels = is_hsla ? 4 : 3;
                bool parse_ok = true;

                for (channel_idx = 0; channel_idx < max_channels && parse_ok; channel_idx++) {
                    while (j < value.size() && is_whitespace(value[j])) j++;
                    if (j >= value.size()) { parse_ok = false; break; }

                    int int_part = 0, frac_part = 0, frac_div = 1;
                    bool has_digits = false;

                    if (j < value.size() && is_digit(value[j])) {
                        has_digits = true;
                        while (j < value.size() && is_digit(value[j])) {
                            int_part = int_part * 10 + (value[j] - '0');
                            j++;
                        }
                    }

                    if (j < value.size() && value[j] == '.') {
                        j++;
                        while (j < value.size() && is_digit(value[j])) {
                            frac_part = frac_part * 10 + (value[j] - '0');
                            frac_div *= 10;
                            has_digits = true;
                            j++;
                        }
                    }

                    if (!has_digits) { parse_ok = false; break; }

                    bool is_pct = false;
                    if (j < value.size() && value[j] == '%') {
                        is_pct = true;
                        j++;
                    }

                    int val = int_part;
                    if (channel_idx < 3 && is_pct) {
                        val = int_part; // percentage for S/L
                    }
                    // Alpha
                    if (channel_idx == 3) {
                        if (frac_div > 1) {
                            double alpha_f = int_part + (double)frac_part / frac_div;
                            a = (int)(alpha_f * 255 + 0.5);
                        } else if (is_pct) {
                            a = (int_part * 255 + 50) / 100;
                        } else {
                            a = std::min(255, int_part * 255);
                        }
                        a = std::max(0, std::min(255, a));
                    } else {
                        val = std::max(0, std::min(channel_idx == 0 ? 360 : 100, int_part));
                    }
                    if (channel_idx == 0) h = val;
                    else if (channel_idx == 1) s = val;
                    else if (channel_idx == 2) l = val;

                    while (j < value.size() && is_whitespace(value[j])) j++;
                    if (j < value.size() && value[j] == ',') { j++; continue; }
                    if (j < value.size() && value[j] == ')') { j++; break; }
                    if (j >= value.size() || value[j] != ')') { parse_ok = false; break; }
                }

                while (j < value.size() && is_whitespace(value[j])) j++;
                if (j < value.size() && value[j] == ')') j++;

                if (parse_ok && channel_idx + 1 >= (is_hsla ? 4 : 3)) {
                    // Convert HSL to RGB
                    int channels[4];
                    double dh = (double)h / 360.0;
                    double ds = (double)s / 100.0;
                    double dl = (double)l / 100.0;

                    if (s == 0) {
                        int gray = (int)(dl * 255 + 0.5);
                        channels[0] = channels[1] = channels[2] = gray;
                    } else {
                        auto hue2rgb = [](double p, double q, double t) -> double {
                            if (t < 0) t += 1;
                            if (t > 1) t -= 1;
                            if (t < 1.0/6.0) return p + (q - p) * 6 * t;
                            if (t < 0.5) return q;
                            if (t < 2.0/3.0) return p + (q - p) * (2.0/3.0 - t) * 6;
                            return p;
                        };
                        double q = dl < 0.5 ? dl * (1 + ds) : dl + ds - dl * ds;
                        double p = 2 * dl - q;
                        channels[0] = (int)(hue2rgb(p, q, dh + 1.0/3.0) * 255 + 0.5);
                        channels[1] = (int)(hue2rgb(p, q, dh) * 255 + 0.5);
                        channels[2] = (int)(hue2rgb(p, q, dh - 1.0/3.0) * 255 + 0.5);
                    }
                    channels[0] = std::max(0, std::min(255, channels[0]));
                    channels[1] = std::max(0, std::min(255, channels[1]));
                    channels[2] = std::max(0, std::min(255, channels[2]));
                    channels[3] = a;

                    // Build hex output (shared with rgb/rgba logic)
                    static const char hexdigits[] = "0123456789abcdef";
                    char hex[10];
                    hex[0] = '#';
                    hex[1] = hexdigits[(channels[0] >> 4) & 0xF];
                    hex[2] = hexdigits[channels[0] & 0xF];
                    hex[3] = hexdigits[(channels[1] >> 4) & 0xF];
                    hex[4] = hexdigits[channels[1] & 0xF];
                    hex[5] = hexdigits[(channels[2] >> 4) & 0xF];
                    hex[6] = hexdigits[channels[2] & 0xF];

                    if (is_hsla && channels[3] < 255) {
                        hex[7] = hexdigits[(channels[3] >> 4) & 0xF];
                        hex[8] = hexdigits[channels[3] & 0xF];
                        hex[9] = '\0';
                        if (hex[1] == hex[2] && hex[3] == hex[4] && hex[5] == hex[6] && hex[7] == hex[8]) {
                            result += '#';
                            result += hex[1];
                            result += hex[3];
                            result += hex[5];
                            result += hex[7];
                        } else {
                            result += hex;
                        }
                    } else {
                        hex[7] = '\0';
                        if (hex[1] == hex[2] && hex[3] == hex[4] && hex[5] == hex[6]) {
                            std::string hex6 = {hex[1], hex[1], hex[3], hex[3], hex[5], hex[5]};
                            auto nc = NAMED_COLOR.find(hex6);
                            if (nc != NAMED_COLOR.end() && nc->second.size() < 4) {
                                result += nc->second;
                            } else {
                                result += '#';
                                result += hex[1];
                                result += hex[3];
                                result += hex[5];
                            }
                        } else {
                            std::string hex6(hex + 1, 6);
                            auto nc = NAMED_COLOR.find(hex6);
                            if (nc != NAMED_COLOR.end() && nc->second.size() < 7) {
                                result += nc->second;
                            } else {
                                result += hex;
                            }
                        }
                    }
                    i = j;
                    continue;
                }
            }
        }
        if (is_ident_char(value[i]) && !is_digit(value[i]) &&
            (i == 0 || !is_ident_char(value[i-1]))) {
            size_t token_end = i;
            while (token_end < value.size() && is_ident_char(value[token_end])) token_end++;
            std::string_view token = std::string_view(value).substr(i, token_end - i);
            // Map named colors to 3-digit hex when the hex form is shorter
            static const std::unordered_map<std::string_view, std::string_view> NAMED_TO_HEX = {
                // CSS4 named colors → canonical hex (148 entries)
                // Only applied when hex is shorter than name (checked below)
                {"aliceblue","#f0f8ff"},{"antiquewhite","#faebd7"},{"aqua","#0ff"},
                {"aquamarine","#7fffd4"},{"azure","#f0ffff"},{"beige","#f5f5dc"},
                {"bisque","#ffe4c4"},{"black","#000"},{"blanchedalmond","#ffebcd"},
                {"blue","#00f"},{"blueviolet","#8a2be2"},{"brown","#a52a2a"},
                {"burlywood","#deb887"},{"cadetblue","#5f9ea0"},{"chartreuse","#7fff00"},
                {"chocolate","#d2691e"},{"coral","#ff7f50"},{"cornflowerblue","#6495ed"},
                {"cornsilk","#fff8dc"},{"crimson","#dc143c"},{"cyan","#0ff"},
                {"darkblue","#00008b"},{"darkcyan","#008b8b"},{"darkgoldenrod","#b8860b"},
                {"darkgray","#a9a9a9"},{"darkgreen","#006400"},{"darkgrey","#a9a9a9"},
                {"darkkhaki","#bdb76b"},{"darkmagenta","#8b008b"},{"darkolivegreen","#556b2f"},
                {"darkorange","#ff8c00"},{"darkorchid","#9932cc"},{"darkred","#8b0000"},
                {"darksalmon","#e9967a"},{"darkseagreen","#8fbc8f"},{"darkslateblue","#483d8b"},
                {"darkslategray","#2f4f4f"},{"darkslategrey","#2f4f4f"},{"darkturquoise","#00ced1"},
                {"darkviolet","#9400d3"},{"deeppink","#ff1493"},{"deepskyblue","#00bfff"},
                {"dimgray","#696969"},{"dimgrey","#696969"},{"dodgerblue","#1e90ff"},
                {"firebrick","#b22222"},{"floralwhite","#fffaf0"},{"forestgreen","#228b22"},
                {"fuchsia","#f0f"},{"gainsboro","#dcdcdc"},{"ghostwhite","#f8f8ff"},
                {"gold","#ffd700"},{"goldenrod","#daa520"},{"gray","#808080"},
                {"green","#008000"},{"greenyellow","#adff2f"},{"grey","#808080"},
                {"honeydew","#f0fff0"},{"hotpink","#ff69b4"},{"indianred","#cd5c5c"},
                {"indigo","#4b0082"},{"ivory","#fffff0"},{"khaki","#f0e68c"},
                {"lavender","#e6e6fa"},{"lavenderblush","#fff0f5"},{"lawngreen","#7cfc00"},
                {"lemonchiffon","#fffacd"},{"lightblue","#add8e6"},{"lightcoral","#f08080"},
                {"lightcyan","#e0ffff"},{"lightgoldenrodyellow","#fafad2"},{"lightgray","#d3d3d3"},
                {"lightgreen","#90ee90"},{"lightgrey","#d3d3d3"},{"lightpink","#ffb6c1"},
                {"lightsalmon","#ffa07a"},{"lightseagreen","#20b2aa"},{"lightskyblue","#87cefa"},
                {"lightslategray","#789"},{"lightslategrey","#789"},{"lightsteelblue","#b0c4de"},
                {"lightyellow","#ffffe0"},{"lime","#0f0"},{"limegreen","#32cd32"},
                {"linen","#faf0e6"},{"magenta","#f0f"},{"maroon","#800000"},
                {"mediumaquamarine","#66cdaa"},{"mediumblue","#0000cd"},{"mediumorchid","#ba55d3"},
                {"mediumpurple","#9370db"},{"mediumseagreen","#3cb371"},{"mediumslateblue","#7b68ee"},
                {"mediumspringgreen","#00fa9a"},{"mediumturquoise","#48d1cc"},{"mediumvioletred","#c71585"},
                {"midnightblue","#191970"},{"mintcream","#f5fffa"},{"mistyrose","#ffe4e1"},
                {"moccasin","#ffe4b5"},{"navajowhite","#ffdead"},{"navy","#000080"},
                {"oldlace","#fdf5e6"},{"olive","#808000"},{"olivedrab","#6b8e23"},
                {"orange","#ffa500"},{"orangered","#ff4500"},{"orchid","#da70d6"},
                {"palegoldenrod","#eee8aa"},{"palegreen","#98fb98"},{"paleturquoise","#afeeee"},
                {"palevioletred","#db7093"},{"papayawhip","#ffefd5"},{"peachpuff","#ffdab9"},
                {"peru","#cd853f"},{"pink","#ffc0cb"},{"plum","#dda0dd"},
                {"powderblue","#b0e0e6"},{"purple","#800080"},{"rebeccapurple","#639"},
                {"red","#f00"},{"rosybrown","#bc8f8f"},{"royalblue","#4169e1"},
                {"saddlebrown","#8b4513"},{"salmon","#fa8072"},{"sandybrown","#f4a460"},
                {"seagreen","#2e8b57"},{"seashell","#fff5ee"},{"sienna","#a0522d"},
                {"silver","#c0c0c0"},{"skyblue","#87ceeb"},{"slateblue","#6a5acd"},
                {"slategray","#708090"},{"slategrey","#708090"},{"snow","#fffafa"},
                {"springgreen","#00ff7f"},{"steelblue","#4682b4"},{"tan","#d2b48c"},
                {"teal","#008080"},{"thistle","#d8bfd8"},{"tomato","#ff6347"},
                 {"turquoise","#40e0d0"},{"violet","#ee82ee"},{"wheat","#f5deb3"},
                 {"white","#fff"},{"whitesmoke","#f5f5f5"},{"yellow","#ff0"},
                 {"yellowgreen","#9acd32"},
                 {"transparent","#0000"},
             };
            auto nc = NAMED_TO_HEX.find(token);
            if (nc != NAMED_TO_HEX.end() && nc->second.size() < token.size()) {
                result += nc->second;
                i = token_end;
                continue;
            }
        }

        // Check for "0px", "0em", "0%", etc. — remove unit from zero
        if (value[i] == '0' && (i == 0 || !is_digit(value[i-1]))) {
            size_t end = i + 1;
            while (end < value.size() && is_ident_char(value[end])) end++;
            if (end > i + 1) {
                // Check if next char is a unit
                std::string_view unit = std::string_view(value).substr(i + 1, end - i - 1);
                if (unit == "px" || unit == "em" || unit == "rem" || unit == "%" ||
                    unit == "vh" || unit == "vw" || unit == "pt" || unit == "cm" ||
                    unit == "mm" || unit == "in" || unit == "pc" || unit == "ex" ||
                    unit == "ch" || unit == "vmin" || unit == "vmax" || unit == "Q" ||
                    unit == "deg" || unit == "rad" || unit == "grad" || unit == "turn" ||
                    unit == "s" || unit == "ms" || unit == "Hz" || unit == "kHz" ||
                    unit == "dpi" || unit == "dpcm" || unit == "dppx") {
                    result += '0';
                    i = end;
                    continue;
                }
            }
        }

        // Check for ".5" — leading zero removal
        if (value[i] == '0' && i + 1 < value.size() && value[i+1] == '.') {
            result += '.';
            i += 2;
            continue;
        }

        // Check for "X.0" -> "X" — trailing zero removal
        // e.g., "1.0" -> "1", "2.00" -> "2", "10.0" -> "10"
        if (is_digit(value[i]) && (i == 0 || !is_ident_char(value[i-1]))) {
            size_t j = i;
            while (j < value.size() && is_digit(value[j])) j++;
            if (j < value.size() && value[j] == '.') {
                size_t k = j + 1;
                while (k < value.size() && is_digit(value[k])) k++;
                // Check if all digits after the dot are zeros
                bool all_zeros = true;
                for (size_t z = j + 1; z < k; z++) {
                    if (value[z] != '0') { all_zeros = false; break; }
                }
                if (all_zeros && k > j + 1) {
                    // Copy just the integer part
                    for (size_t z = i; z < j; z++) result += value[z];
                    i = k;
                    continue;
                }
            }
        }

        // Remove unnecessary whitespace
        if (is_whitespace(value[i])) {
            // Don't add space after comma
            if (!result.empty() && !is_whitespace(result.back()) && result.back() != ',') {
                result += ' ';
                // If the next non-whitespace is a comma, the space we just added is unnecessary
                size_t peek = i + 1;
                while (peek < value.size() && is_whitespace(value[peek])) peek++;
                if (peek < value.size() && value[peek] == ',') {
                    result.pop_back(); // remove the space since it's before a comma
                }
            }
            i++;
            continue;
        }

        result += value[i];
        i++;
    }

    // Trim trailing whitespace
    while (!result.empty() && is_whitespace(result.back()))
        result.pop_back();

    return result;
}

// Check if a partial merge is cascade-safe: ensure no later rule with the
// same first selector sets one of the missing longhands that we'd fill with
// "0". Without this, the merged shorthand (setting all sides) would override
// a later rule that sets a specific side on the same element.
static bool partial_merge_cascade_safe(
    const std::vector<CSSRule>& rules, size_t rule_idx,
    const std::unordered_map<std::string_view, size_t>& prop_map,
    const std::vector<std::string_view>& longhands)
{
    // Identify which longhands are missing (would be filled with defaults)
    bool any_missing = false;
    for (auto lh : longhands) {
        if (prop_map.find(lh) == prop_map.end()) {
            any_missing = true;
            break;
        }
    }
    if (!any_missing) return true;  // full merge, always safe

    // Get current rule's first-selector text
    if (rules[rule_idx].selectors().empty()) return true;
    std::string sel_text = rules[rule_idx].selector_text();
    if (sel_text.empty()) return true;

    // Check later rules with the same first-selector text
    for (size_t j = rule_idx + 1; j < rules.size(); j++) {
        const auto& later = rules[j];
        if (later.selectors().empty()) continue;
        if (later.selector_text() != sel_text) continue;
        // Same selector – if it sets a missing longhand, we'd override it
        for (const auto& decl : later.declarations()) {
            for (auto lh : longhands) {
                if (prop_map.find(lh) == prop_map.end() && decl.property == lh)
                    return false;
            }
        }
    }
    return true;
}

bool Optimizer::pass_css_shorthand(UnifiedDocument& doc) {
    bool changed = false;

    // Single longhand → shorthand promotion map.
    // When only ONE longhand from a shorthand family is present (no other
    // longhands in the same rule), promote it to the shorthand to save bytes.
    // Safe because the shorthand resets missing sub-properties to initial,
    // and they are already at initial when no other longhands exist.
    static const std::unordered_map<std::string_view, std::string_view> LONGHAND_PROMOTE = {
        {"background-color", "background"},
        {"background-image", "background"},
        {"list-style-type", "list-style"},
        {"text-decoration-line", "text-decoration"},
        {"border-top-left-radius", "border-radius"},
        {"border-top-right-radius", "border-radius"},
        {"border-bottom-right-radius", "border-radius"},
        {"border-bottom-left-radius", "border-radius"},
        {"border-width", "border"},
        {"border-style", "border"},
        {"border-color", "border"},
        {"outline-width", "outline"},
        {"outline-style", "outline"},
        {"outline-color", "outline"},
        {"row-gap", "gap"},
        {"column-gap", "gap"},
        {"flex-direction", "flex-flow"},
        {"flex-wrap", "flex-flow"},
        {"column-width", "columns"},
        {"column-count", "columns"},
        {"top", "inset"},
        {"right", "inset"},
        {"bottom", "inset"},
        {"left", "inset"},
        {"align-content", "place-content"},
        {"justify-content", "place-content"},
        {"align-items", "place-items"},
        {"justify-items", "place-items"},
        {"align-self", "place-self"},
        {"justify-self", "place-self"},
        {"transition-property", "transition"},
        {"transition-duration", "transition"},
        {"transition-timing-function", "transition"},
        {"transition-delay", "transition"},
        {"overflow-x", "overflow"},
        {"overflow-y", "overflow"},
        {"inset-block-start", "inset-block"},
        {"inset-block-end", "inset-block"},
        {"inset-inline-start", "inset-inline"},
        {"inset-inline-end", "inset-inline"},
        {"text-emphasis-style", "text-emphasis"},
        {"text-emphasis-color", "text-emphasis"},
        {"scroll-margin-top", "scroll-margin"},
        {"scroll-margin-right", "scroll-margin"},
        {"scroll-margin-bottom", "scroll-margin"},
        {"scroll-margin-left", "scroll-margin"},
        {"scroll-padding-top", "scroll-padding"},
        {"scroll-padding-right", "scroll-padding"},
        {"scroll-padding-bottom", "scroll-padding"},
        {"scroll-padding-left", "scroll-padding"},
        {"mask-image", "mask"},
        {"mask-position", "mask"},
        {"mask-size", "mask"},
        {"mask-repeat", "mask"},
        {"mask-origin", "mask"},
        {"mask-clip", "mask"},
        {"mask-mode", "mask"},
        {"mask-composite", "mask"},
        {"overflow-block-start", "overflow-block"},
        {"overflow-block-end", "overflow-block"},
        {"overflow-inline-start", "overflow-inline"},
        {"overflow-inline-end", "overflow-inline"},
        {"grid-row-start", "grid-row"},
        {"grid-row-end", "grid-row"},
        {"grid-column-start", "grid-column"},
        {"grid-column-end", "grid-column"},
        {"grid-row-start", "grid-area"},
        {"grid-column-start", "grid-area"},
        {"grid-row-end", "grid-area"},
        {"grid-column-end", "grid-area"},
        {"font-variant-caps", "font-variant"},
        {"font-variant-numeric", "font-variant"},
        {"font-variant-alternates", "font-variant"},
        {"font-variant-ligatures", "font-variant"},
        {"font-variant-east-asian", "font-variant"},
        {"font-variant-position", "font-variant"},
        {"font-variant-emoji", "font-variant"},
    };

    auto& rules = const_cast<std::vector<CSSRule>&>(doc.stylesheets());
    for (size_t rule_idx = 0; rule_idx < rules.size(); rule_idx++) {
        auto& rule = rules[rule_idx];
        auto& decls = const_cast<std::vector<CSSRule::Declaration>&>(rule.declarations());

        // Build a map of property -> index
        std::unordered_map<std::string_view, size_t> prop_map;
        for (size_t i = 0; i < decls.size(); i++) {
            prop_map[decls[i].property] = i;
        }

        // --- Single longhand → shorthand promotion ---
        for (size_t i = 0; i < decls.size(); i++) {
            auto promo_it = LONGHAND_PROMOTE.find(decls[i].property);
            if (promo_it == LONGHAND_PROMOTE.end()) continue;

            std::string_view shorthand = promo_it->second;
            // Check if any *other* longhand from this shorthand family exists
            auto sh_it = SHORTHAND_MAP.find(shorthand);
            if (sh_it == SHORTHAND_MAP.end()) continue;

            bool has_other_longhand = false;
            for (auto lh : sh_it->second) {
                if (lh == decls[i].property) continue;
                if (prop_map.find(lh) != prop_map.end()) {
                    has_other_longhand = true;
                    break;
                }
            }
            if (has_other_longhand) continue;

            // Safe to promote — rename property to shorthand
            decls[i].property = doc.string_pool().intern(shorthand);
            changed = true;
        }
        // Rebuild prop_map after promotions
        prop_map.clear();
        for (size_t i = 0; i < decls.size(); i++) {
            prop_map[decls[i].property] = i;
        }

        // Check each shorthand (multi-longhand merging)
        // Process larger shorthands first so grid-area (4 longhands) beats
        // grid-row (2 longhands) when all longhands are present.
        // SHORTHAND_MAP is unordered_map; build a sorted view by longhand count.
        std::vector<std::pair<std::string_view, const std::vector<std::string_view>*>> sorted_sh;
        for (const auto& p : SHORTHAND_MAP)
            sorted_sh.emplace_back(p.first, &p.second);
        std::stable_sort(sorted_sh.begin(), sorted_sh.end(),
            [](const auto& a, const auto& b) { return a.second->size() > b.second->size(); });
        for (const auto& [shorthand, longhands_ptr] : sorted_sh) {
            const auto& longhands = *longhands_ptr;
            // font is handled separately — requires only font-size + font-family
            if (shorthand == "font") continue;
            // Check if we have all longhand properties
            bool has_all = true;
            for (auto lh : longhands) {
                if (prop_map.find(lh) == prop_map.end()) {
                    has_all = false;
                    break;
                }
            }

            if (has_all && longhands.size() >= 2) {
                // Build the shorthand value
                // shorthands where size must be preceded by "/"
                static const std::unordered_set<std::string_view> SLASH_BEFORE_SIZE = {
                    "background", "mask"
                };
                // shorthands where ALL longhand values are separated by "/" instead of space
                static const std::unordered_set<std::string_view> SLASH_BETWEEN_ALL = {
                    "grid-row", "grid-column", "grid-area"
                };
                // mask: deduplicate origin/clip when equal
                bool dedup_origin_clip = (shorthand == "mask");
                std::string_view origin_val, clip_val;

                std::string sh_value;
                for (size_t li = 0; li < longhands.size(); li++) {
                    auto it = prop_map.find(longhands[li]);
                    if (it == prop_map.end()) continue;

                    // Deduplicate mask-origin / mask-clip
                    if (dedup_origin_clip && longhands[li] == "mask-origin") {
                        origin_val = decls[it->second].value;
                        auto clip_it = prop_map.find("mask-clip");
                        if (clip_it != prop_map.end()) {
                            clip_val = decls[clip_it->second].value;
                        }
                        if (!sh_value.empty()) sh_value += ' ';
                        sh_value += origin_val;
                        continue;
                    }
                    if (dedup_origin_clip && longhands[li] == "mask-clip") {
                        if (!clip_val.empty() && origin_val == clip_val) {
                            // origin and clip are equal — already output origin above
                            continue;
                        }
                        // origin and clip differ — output clip
                        if (!sh_value.empty()) sh_value += ' ';
                        sh_value += decls[it->second].value;
                        continue;
                    }

                    // Separator character
                    bool slash_all = SLASH_BETWEEN_ALL.count(shorthand);
                    bool slash_before = SLASH_BEFORE_SIZE.count(shorthand) &&
                        (longhands[li] == "background-size" || longhands[li] == "mask-size");

                    if (!sh_value.empty()) {
                        if (slash_all || slash_before) {
                            sh_value += " / ";
                        } else {
                            sh_value += ' ';
                        }
                    }
                    sh_value += decls[it->second].value;
                }

                // Try to simplify: if all values are the same, use fewer
                // For 4-value: top right bottom left
                // If top == bottom && left == right, collapse to 2 values
                // If all equal, collapse to 1 value
                // Skip for /-separated shorthands where position has semantic meaning
                // (e.g., grid-area: 1 / 1 / 1 / 1 ≠ grid-area: 1)
                if (longhands.size() == 4 && !SLASH_BETWEEN_ALL.count(shorthand)) {
                    std::string_view v0 = decls[prop_map[longhands[0]]].value;
                    std::string_view v1 = decls[prop_map[longhands[1]]].value;
                    std::string_view v2 = decls[prop_map[longhands[2]]].value;
                    std::string_view v3 = decls[prop_map[longhands[3]]].value;

                    if (v0 == v1 && v1 == v2 && v2 == v3) {
                        sh_value = std::string(v0);
                    } else if (v0 == v2 && v1 == v3) {
                        sh_value = std::string(v0) + " " + std::string(v1);
                    } else if (v1 == v3) {
                        sh_value = std::string(v0) + " " + std::string(v1) + " " + std::string(v2);
                    }
                } else if (longhands.size() == 2 && !SLASH_BETWEEN_ALL.count(shorthand)) {
                    std::string_view v0 = decls[prop_map[longhands[0]]].value;
                    std::string_view v1 = decls[prop_map[longhands[1]]].value;
                    if (v0 == v1) {
                        sh_value = std::string(v0);
                    }
                }

                // Minify the value
                sh_value = minify_css_value(sh_value);

                // Add shorthand declaration
                CSSRule::Declaration sh_decl;
                sh_decl.property = doc.string_pool().intern(shorthand);
                sh_decl.value = doc.string_pool().intern(sh_value);
                decls.push_back(sh_decl);

                // Remove longhands (mark for removal — do it in reverse)
                for (auto it = longhands.rbegin(); it != longhands.rend(); ++it) {
                    auto pit = prop_map.find(*it);
                    if (pit != prop_map.end() && pit->second < decls.size()) {
                        decls.erase(decls.begin() + pit->second);
                        // Update indices (they shifted)
                        prop_map.clear();
                        for (size_t i = 0; i < decls.size(); i++) {
                            prop_map[decls[i].property] = i;
                        }
                    }
                }

                changed = true;
            }
            // Partial merge: for 4-value families (margin, padding, inset, border-radius,
            // border-width), merge when ≥2 longhands are present, using "0" for missing
            // sides (initial values are 0). Skip border-style and border-color where
            // initial is not 0.
            else if (!has_all && longhands.size() == 4) {
                static const std::unordered_set<std::string_view> PARTIAL_4_UNSAFE = {
                    "border-style", "border-color", "grid-area"
                };
                if (PARTIAL_4_UNSAFE.find(shorthand) == PARTIAL_4_UNSAFE.end()) {
                    // Count how many longhands are present and build the 4-value string
                    std::string_view values[4];
                    size_t present_count = 0;
                    for (size_t li = 0; li < 4; li++) {
                        auto it = prop_map.find(longhands[li]);
                        if (it != prop_map.end()) {
                            values[li] = decls[it->second].value;
                            present_count++;
                        } else {
                            values[li] = "0";
                        }
                    }

                    // Need at least 2 longhands for the merge to be worthwhile
                    if (present_count >= 2) {
                        // Cascade-safe check: don't merge if a later rule
                        // targeting the same selector sets a missing side
                        if (!partial_merge_cascade_safe(rules, rule_idx, prop_map, longhands)) {
                            // skip: merge would override later declarations
                        } else {
                            std::string sh_value = std::string(values[0]) + " " + std::string(values[1]) +
                                                   " " + std::string(values[2]) + " " + std::string(values[3]);

                            sh_value = minify_css_value(sh_value);

                            CSSRule::Declaration sh_decl;
                            sh_decl.property = doc.string_pool().intern(shorthand);
                            sh_decl.value = doc.string_pool().intern(sh_value);
                            decls.push_back(sh_decl);

                            // Remove merged longhands (reverse order for stable indices)
                            for (auto it = longhands.rbegin(); it != longhands.rend(); ++it) {
                                auto pit = prop_map.find(*it);
                                if (pit != prop_map.end() && pit->second < decls.size()) {
                                    decls.erase(decls.begin() + pit->second);
                                    prop_map.clear();
                                    for (size_t j = 0; j < decls.size(); j++) {
                                        prop_map[decls[j].property] = j;
                                    }
                                }
                            }

                            changed = true;
                        }
                    }
                }
            }
            // Partial merge: for 3-component shorthands (border/outline/border-*),
            // merge when border-style (required) plus at least width or color are present.
            // Missing components keep their initial values, so the shorthand is equivalent.
            else if (!has_all && longhands.size() == 3) {
                auto w_it = prop_map.find(longhands[0]); // width
                auto s_it = prop_map.find(longhands[1]); // style (required)
                auto c_it = prop_map.find(longhands[2]); // color

                bool has_style = (s_it != prop_map.end());
                bool has_width = (w_it != prop_map.end());
                bool has_color = (c_it != prop_map.end());

                if (has_style && (has_width || has_color)) {
                    // Cascade-safe check: don't merge if a later rule
                    // targeting the same selector sets a missing longhand
                    // (e.g., border-width + border-style → border would
                    //  override a later border-color on the same selector).
                    if (!partial_merge_cascade_safe(rules, rule_idx, prop_map, longhands)) {
                        // skip: merge would override later declarations
                    } else {
                        std::string sh_value;
                        if (has_width) {
                            sh_value = std::string(decls[w_it->second].value);
                        }
                        if (!sh_value.empty()) sh_value += ' ';
                        sh_value += std::string(decls[s_it->second].value);
                        if (has_color) {
                            sh_value += ' ';
                            sh_value += std::string(decls[c_it->second].value);
                        }

                        sh_value = minify_css_value(sh_value);

                        CSSRule::Declaration sh_decl;
                        sh_decl.property = doc.string_pool().intern(shorthand);
                        sh_decl.value = doc.string_pool().intern(sh_value);
                        decls.push_back(sh_decl);

                        // Remove merged longhands
                        for (auto it = longhands.rbegin(); it != longhands.rend(); ++it) {
                            auto pit = prop_map.find(*it);
                            if (pit != prop_map.end() && pit->second < decls.size()) {
                                decls.erase(decls.begin() + pit->second);
                                prop_map.clear();
                                for (size_t i = 0; i < decls.size(); i++) {
                                    prop_map[decls[i].property] = i;
                                }
                            }
                        }

                        changed = true;
                    }
                }
            }
            // Generic partial merge: for shorthands with >4 longhands
            // (e.g., font-variant 7, mask 8), merge when ≥2 longhands
            // are present. Handles SLASH_BEFORE_SIZE for mask/background
            // (prepends "/" before size longhand). Skips SLASH_BETWEEN_ALL
            // shorthands (can't partially merge /-separated values).
            else if (!has_all && longhands.size() > 4) {
                // Skip slash-between-all shorthands (e.g., grid-area)
                if (shorthand == "grid-row" || shorthand == "grid-column" || shorthand == "grid-area") {
                    // can't partially merge; fall through
                } else {
                    // Determine if this shorthand has a /-before-size longhand
                    bool has_slash_size = (shorthand == "mask" || shorthand == "background");
                    // Collect present values with their longhand indices
                    struct LhValue { size_t longhand_idx; std::string_view value; };
                    std::vector<LhValue> present;
                    for (size_t li = 0; li < longhands.size(); li++) {
                        auto it = prop_map.find(longhands[li]);
                        if (it != prop_map.end()) {
                            present.push_back({li, decls[it->second].value});
                        }
                    }

                    // For SLASH_BEFORE_SIZE shorthands, skip the partial
                    // merge if size is present without position — the
                    // "/" separator requires preceding position.
                    if (has_slash_size) {
                        bool has_position = false;
                        bool has_size = false;
                        for (auto& lh : present) {
                            if (longhands[lh.longhand_idx] == "mask-position" ||
                                longhands[lh.longhand_idx] == "background-position") {
                                has_position = true;
                            }
                            if (longhands[lh.longhand_idx] == "mask-size" ||
                                longhands[lh.longhand_idx] == "background-size") {
                                has_size = true;
                            }
                        }
                        if (has_size && !has_position) {
                            // Can't place /-separated size without position;
                            // skip the partial merge.
                            present.clear();
                        }
                    }

                    // Need at least 2 longhands for the merge to be worthwhile
                    if (present.size() >= 2) {
                        // Cascade-safe check
                        if (!partial_merge_cascade_safe(rules, rule_idx, prop_map, longhands)) {
                            // skip: merge would override later declarations
                        } else {
                            std::string sh_value;
                            for (size_t pi = 0; pi < present.size(); pi++) {
                                if (pi > 0) {
                                    // Prepend "/ " before size longhands in
                                    // SLASH_BEFORE_SIZE shorthands
                                    bool before_slash = has_slash_size &&
                                        (longhands[present[pi].longhand_idx] == "mask-size" ||
                                         longhands[present[pi].longhand_idx] == "background-size");
                                    if (before_slash) {
                                        sh_value += "/ ";
                                    } else {
                                        sh_value += ' ';
                                    }
                                }
                                sh_value += std::string(present[pi].value);
                            }

                            sh_value = minify_css_value(sh_value);

                            CSSRule::Declaration sh_decl;
                            sh_decl.property = doc.string_pool().intern(shorthand);
                            sh_decl.value = doc.string_pool().intern(sh_value);
                            decls.push_back(sh_decl);

                            // Remove merged longhands (reverse order for stable indices)
                            for (auto it = longhands.rbegin(); it != longhands.rend(); ++it) {
                                auto pit = prop_map.find(*it);
                                if (pit != prop_map.end() && pit->second < decls.size()) {
                                    decls.erase(decls.begin() + pit->second);
                                    prop_map.clear();
                                    for (size_t j = 0; j < decls.size(); j++) {
                                        prop_map[decls[j].property] = j;
                                    }
                                }
                            }

                            changed = true;
                        }
                    }
                }
            }
            // Generic 2-value partial merge: when exactly 1 of 2 longhands is
            // present, promote to shorthand with 1 value (sets both sub-properties
            // to the same value — aggressive but valid for extreme minification).
            // This handles overflow, gap, flex-flow, columns, place-*, and any
            // future 2-value shorthands generically without per-property entries.
            else if (!has_all && longhands.size() == 2) {
                // Count present longhands, find the one that IS present
                size_t present_count = 0;
                std::string_view present_value;
                for (size_t li = 0; li < 2; li++) {
                    auto it = prop_map.find(longhands[li]);
                    if (it != prop_map.end()) {
                        present_count++;
                        present_value = decls[it->second].value;
                    }
                }

                // Only merge if exactly 1 longhand is present.
                // (2 present = full merge already handled above.)
                if (present_count == 1) {
                    // Cascade-safe check: don't merge if a later rule
                    // targeting the same selector sets the missing longhand.
                    if (!partial_merge_cascade_safe(rules, rule_idx, prop_map, longhands)) {
                        // skip: merge would override later declarations
                    } else {
                        std::string sh_value = minify_css_value(std::string(present_value));

                        CSSRule::Declaration sh_decl;
                        sh_decl.property = doc.string_pool().intern(shorthand);
                        sh_decl.value = doc.string_pool().intern(sh_value);
                        decls.push_back(sh_decl);

                        // Remove the single merged longhand
                        for (auto it = longhands.rbegin(); it != longhands.rend(); ++it) {
                            auto pit = prop_map.find(*it);
                            if (pit != prop_map.end() && pit->second < decls.size()) {
                                decls.erase(decls.begin() + pit->second);
                                prop_map.clear();
                                for (size_t i = 0; i < decls.size(); i++) {
                                    prop_map[decls[i].property] = i;
                                }
                            }
                        }

                        changed = true;
                    }
                }
            }
        }

        // --- Special: font shorthand (minimum: font-size + font-family) ---
        auto sz_it = prop_map.find("font-size");
        auto fam_it = prop_map.find("font-family");
        if (sz_it != prop_map.end() && fam_it != prop_map.end()) {
            // font shorthand already present? Skip to avoid duplicate.
            if (prop_map.find("font") == prop_map.end()) {
                std::string sh_value;

                // Optional: font-style
                auto it = prop_map.find("font-style");
                if (it != prop_map.end()) { sh_value += std::string(decls[it->second].value) + ' '; }

                // Optional: font-variant
                it = prop_map.find("font-variant");
                if (it != prop_map.end()) { sh_value += std::string(decls[it->second].value) + ' '; }

                // Optional: font-weight
                it = prop_map.find("font-weight");
                if (it != prop_map.end()) { sh_value += std::string(decls[it->second].value) + ' '; }

                // Required: font-size
                sh_value += std::string(decls[sz_it->second].value);

                // Optional: line-height (separated by /)
                it = prop_map.find("line-height");
                if (it != prop_map.end()) { sh_value += '/'; sh_value += decls[it->second].value; }

                // Required: font-family
                sh_value += ' ';
                sh_value += decls[fam_it->second].value;

                sh_value = minify_css_value(sh_value);

                // Add shorthand
                CSSRule::Declaration sh_decl;
                sh_decl.property = doc.string_pool().intern("font");
                sh_decl.value = doc.string_pool().intern(sh_value);
                decls.push_back(sh_decl);

                // Remove subsumed font longhands
                static const std::string_view font_longhands[] = {
                    "font-style","font-variant","font-weight",
                    "font-size","line-height","font-family"
                };
                for (auto lh : font_longhands) {
                    auto pit = prop_map.find(lh);
                    if (pit != prop_map.end() && pit->second < decls.size()) {
                        decls.erase(decls.begin() + pit->second);
                        // Rebuild prop_map after each removal
                        prop_map.clear();
                        for (size_t j = 0; j < decls.size(); j++) {
                            prop_map[decls[j].property] = j;
                        }
                    }
                }

                changed = true;
            }
        }
    }

    return changed;
}

// Box-model value folding: collapse redundant space-separated values.
// Applies to properties that take 1-4 values in top/right/bottom/left order.
// e.g., margin: 0 0 0 0  →  margin: 0
//       padding: 1px 2px 1px 2px  →  padding: 1px 2px
//       border-radius: 10px 10px  →  border-radius: 10px
bool Optimizer::pass_css_value_fold(UnifiedDocument& doc) {
    bool changed = false;

    // Properties that use the CSS box-model value syntax (1-4 space-separated values)
    static const std::unordered_set<std::string_view> BOX_MODEL_PROPS = {
        "margin", "padding",
        "border-width", "border-style", "border-color",
        "border-image-width", "border-image-outset",
        "margin-inline", "margin-block", "padding-inline", "padding-block",
        "outline-width", "outline-style", "outline-color",
        "border-radius", "inset", "inset-block", "inset-inline", "gap",
        "scroll-margin", "scroll-padding",
        "mask-border-width", "mask-border-outset",
    };

    for (auto& rule : const_cast<std::vector<CSSRule>&>(doc.stylesheets())) {
        auto& decls = const_cast<std::vector<CSSRule::Declaration>&>(rule.declarations());
        for (auto& decl : decls) {
            if (BOX_MODEL_PROPS.find(decl.property) == BOX_MODEL_PROPS.end()) continue;

            std::string_view val = decl.value;
            if (val.empty()) continue;

            // Tokenize: split on whitespace, but bail on complex values
            std::vector<std::string_view> tokens;
            size_t pos = 0;
            while (pos < val.size()) {
                while (pos < val.size() && is_whitespace(val[pos])) pos++;
                if (pos >= val.size()) break;

                // Bail on '/' (border-radius splits radii) or '(' (function calls)
                if (val[pos] == '/' || val[pos] == '(') { tokens.clear(); break; }

                size_t start = pos;
                while (pos < val.size() && !is_whitespace(val[pos]) && val[pos] != ',' && val[pos] != '/') pos++;
                tokens.push_back(val.substr(start, pos - start));

                while (pos < val.size() && is_whitespace(val[pos])) pos++;
                if (pos < val.size() && (val[pos] == ',' || val[pos] == '/')) {
                    tokens.clear();
                    break;
                }
            }

            if (tokens.size() < 2 || tokens.size() > 4) continue;

            std::string folded;
            switch (tokens.size()) {
                case 4:
                    if (tokens[0] == tokens[1] && tokens[1] == tokens[2] && tokens[2] == tokens[3]) {
                        folded = std::string(tokens[0]);
                    } else if (tokens[0] == tokens[2] && tokens[1] == tokens[3]) {
                        folded = std::string(tokens[0]) + " " + std::string(tokens[1]);
                    } else if (tokens[1] == tokens[3]) {
                        folded = std::string(tokens[0]) + " " + std::string(tokens[1]) + " " + std::string(tokens[2]);
                    }
                    break;
                case 3:
                    if (tokens[0] == tokens[1] && tokens[1] == tokens[2]) {
                        folded = std::string(tokens[0]);
                    } else if (tokens[0] == tokens[2]) {
                        folded = std::string(tokens[0]) + " " + std::string(tokens[1]);
                    }
                    break;
                case 2:
                    if (tokens[0] == tokens[1]) {
                        folded = std::string(tokens[0]);
                    }
                    break;
            }

            if (!folded.empty()) {
                decl.value = doc.string_pool().intern(folded);
                changed = true;
            }
        }
    }

    return changed;
}

// Fold CSS math functions: min(), max(), clamp()
// e.g., max(10px, 20px) → 20px, min(1em, 2em) → 1em
//       clamp(0px, 50px, 100px) → 50px
// Only folds when all arguments have the same explicit CSS unit
// and are simple numeric values (no calc(), var(), nested functions).
bool Optimizer::pass_css_math_fold(UnifiedDocument& doc) {
    bool changed = false;

    for (auto& rule : const_cast<std::vector<CSSRule>&>(doc.stylesheets())) {
        auto& decls = const_cast<std::vector<CSSRule::Declaration>&>(rule.declarations());

        for (auto& decl : decls) {
            std::string_view val = decl.value;
            std::string result;
            bool decl_changed = false;
            result.reserve(val.size());

            size_t i = 0;
            while (i < val.size()) {
                // Look for "min(", "max(", "clamp("
                size_t next_func = std::string_view::npos;
                const char* func_name = nullptr;
                int func_len = 0;

                for (size_t k = i; k < val.size(); k++) {
                    if (tolower(val[k]) == 'm') {
                        if (k + 3 < val.size() && val[k+1] == 'i' && val[k+2] == 'n' && val[k+3] == '(') {
                            next_func = k; func_name = "min"; func_len = 3; break;
                        }
                        if (k + 3 < val.size() && val[k+1] == 'a' && val[k+2] == 'x' && val[k+3] == '(') {
                            next_func = k; func_name = "max"; func_len = 3; break;
                        }
                    }
                    if (tolower(val[k]) == 'c') {
                        if (k + 5 < val.size() && tolower(val[k+1]) == 'l' && tolower(val[k+2]) == 'a' &&
                            tolower(val[k+3]) == 'm' && tolower(val[k+4]) == 'p' && val[k+5] == '(') {
                            next_func = k; func_name = "clamp"; func_len = 5; break;
                        }
                    }
                }

                if (next_func == std::string_view::npos) {
                    result.append(val.data() + i, val.size() - i);
                    break;
                }

                // Copy everything before the function
                result.append(val.data() + i, next_func - i);
                i = next_func + func_len + 1; // past "func("

                // Find matching closing paren
                size_t paren_depth = 1;
                size_t close = i;
                while (close < val.size() && paren_depth > 0) {
                    if (val[close] == '(') paren_depth++;
                    else if (val[close] == ')') paren_depth--;
                    close++;
                }
                if (paren_depth != 0) {
                    // Malformed — copy raw rest and bail
                    result.append(val.data() + i - func_len - 1, val.size() - (i - func_len - 1));
                    i = val.size();
                    break;
                }
                // close is one past ')'
                size_t args_end = close - 1;

                // Parse comma-separated arguments
                std::vector<std::string_view> args;
                size_t arg_start = i;
                size_t arg_paren = 0;
                for (size_t p = i; p < args_end; p++) {
                    if (val[p] == '(') arg_paren++;
                    else if (val[p] == ')') arg_paren--;
                    else if (val[p] == ',' && arg_paren == 0) {
                        // Trim whitespace
                        size_t s = arg_start, e = p;
                        while (s < e && is_whitespace(val[s])) s++;
                        while (e > s && is_whitespace(val[e-1])) e--;
                        if (s < e) args.push_back(val.substr(s, e - s));
                        arg_start = p + 1;
                    }
                }
                // Last arg
                {
                    size_t s = arg_start, e = args_end;
                    while (s < e && is_whitespace(val[s])) s++;
                    while (e > s && is_whitespace(val[e-1])) e--;
                    if (s < e) args.push_back(val.substr(s, e - s));
                }

                // Extract numeric values with units
                struct NumVal {
                    double num;
                    std::string_view unit;
                    bool valid;
                };
                std::vector<NumVal> parsed;
                bool all_valid = true;
                std::string_view common_unit;

                for (auto& arg : args) {
                    if (arg.empty()) { all_valid = false; break; }
                    // Check for signs
                    size_t pos = 0;
                    bool negative = false;
                    if (arg[pos] == '-' || arg[pos] == '+') {
                        negative = (arg[pos] == '-');
                        pos++;
                    }
                    // Parse number
                    double num = 0;
                    bool has_digits = false;
                    if (pos < arg.size() && is_digit(arg[pos])) {
                        has_digits = true;
                        while (pos < arg.size() && is_digit(arg[pos])) {
                            num = num * 10 + (arg[pos] - '0');
                            pos++;
                        }
                    }
                    if (pos < arg.size() && arg[pos] == '.') {
                        pos++;
                        double frac = 0, div = 1;
                        while (pos < arg.size() && is_digit(arg[pos])) {
                            frac = frac * 10 + (arg[pos] - '0');
                            div *= 10;
                            has_digits = true;
                            pos++;
                        }
                        num += frac / div;
                    }
                    if (!has_digits) { all_valid = false; break; }
                    if (negative) num = -num;

                    // Extract unit
                    std::string_view unit = arg.substr(pos);
                    if (unit.empty()) {
                        // Unitless zero — treat as any unit; only valid if value is 0
                    }

                    if (!parsed.empty() && unit != common_unit) {
                        // Unitless zero matches any unit; empty common_unit accepts first real unit
                        if (!(common_unit.empty() || (num == 0 && unit.empty()))) {
                            all_valid = false;
                            break;
                        }
                    }
                    if (!unit.empty()) common_unit = unit;
                    parsed.push_back({num, unit, true});
                }

                if (!all_valid || parsed.size() < 2 || parsed.size() > 10) {
                    // Can't fold — output original
                    result.append(val.data() + next_func, close - next_func);
                    i = close;
                    continue;
                }

                // Apply the function
                double result_num;
                if (strcmp(func_name, "min") == 0) {
                    result_num = parsed[0].num;
                    for (size_t k = 1; k < parsed.size(); k++)
                        if (parsed[k].num < result_num) result_num = parsed[k].num;
                } else if (strcmp(func_name, "max") == 0) {
                    result_num = parsed[0].num;
                    for (size_t k = 1; k < parsed.size(); k++)
                        if (parsed[k].num > result_num) result_num = parsed[k].num;
                } else { // clamp
                    if (parsed.size() != 3) {
                        result.append(val.data() + next_func, close - next_func);
                        i = close;
                        continue;
                    }
                    double clamp_lo = parsed[0].num;
                    double clamp_val = parsed[1].num;
                    double clamp_hi = parsed[2].num;
                    if (clamp_val < clamp_lo) result_num = clamp_lo;
                    else if (clamp_val > clamp_hi) result_num = clamp_hi;
                    else result_num = clamp_val;
                }

                // Format the result
                // Strip trailing zeros from the fractional part
                char buf[64];
                 snprintf(buf, sizeof(buf), "%.17g", result_num);
                std::string num_str(buf);
                // Handle "0." — if just "0", keep it
                // Append unit
                if (!common_unit.empty()) {
                    num_str += common_unit;
                }
                result += num_str;

                i = close;
                decl_changed = true;
            }

            if (decl_changed) {
                decl.value = doc.string_pool().intern(result);
                changed = true;
            }
        }
    }

    return changed;
}

} // namespace tinyizer
