#include "optimizer.h"
#include "serializer.h"
#include "../parser/tokenizer.h"
#include <algorithm>
#include <cctype>
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
    {"place-content", {"align-content", "justify-content"}},
    {"place-items",   {"align-items", "justify-items"}},
    {"place-self",    {"align-self", "justify-self"}},
};

// CSS value minification helpers
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

        // Named color → hex when shorter
        if (is_ident_char(value[i]) && !is_digit(value[i]) &&
            (i == 0 || !is_ident_char(value[i-1]))) {
            size_t token_end = i;
            while (token_end < value.size() && is_ident_char(value[token_end])) token_end++;
            std::string_view token = std::string_view(value).substr(i, token_end - i);
            // Map named colors to 3-digit hex when the hex form is shorter
            static const std::unordered_map<std::string_view, std::string_view> NAMED_TO_HEX = {
                {"white","#fff"}, {"black","#000"}, {"yellow","#ff0"},
                {"fuchsia","#f0f"}, {"magenta","#f0f"},
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
    };

    for (auto& rule : const_cast<std::vector<CSSRule>&>(doc.stylesheets())) {
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
        for (const auto& [shorthand, longhands] : SHORTHAND_MAP) {
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
                std::string sh_value;
                for (size_t li = 0; li < longhands.size(); li++) {
                    auto it = prop_map.find(longhands[li]);
                    if (it != prop_map.end()) {
                        if (li > 0) sh_value += ' ';
                        sh_value += decls[it->second].value;
                    }
                }

                // Try to simplify: if all values are the same, use fewer
                // For 4-value: top right bottom left
                // If top == bottom && left == right, collapse to 2 values
                // If all equal, collapse to 1 value
                if (longhands.size() == 4) {
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
                } else if (longhands.size() == 2) {
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
        "margin-inline", "margin-block", "padding-inline", "padding-block",
        "outline-width", "outline-style", "outline-color",
        "border-radius", "inset", "gap", "grid-gap",
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

} // namespace tinyizer
