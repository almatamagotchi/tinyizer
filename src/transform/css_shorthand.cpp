#include "optimizer.h"
#include "../parser/tokenizer.h"
#include <algorithm>
#include <unordered_map>

// CSS shorthand property merging and value minification
namespace tinyizer {

// Known shorthand properties and their longhand components
static const std::unordered_map<std::string_view, std::vector<std::string_view>> SHORTHAND_MAP = {
    {"margin",       {"margin-top", "margin-right", "margin-bottom", "margin-left"}},
    {"padding",      {"padding-top", "padding-right", "padding-bottom", "padding-left"}},
    {"border-width", {"border-top-width", "border-right-width", "border-bottom-width", "border-left-width"}},
    {"border-style", {"border-top-style", "border-right-style", "border-bottom-style", "border-left-style"}},
    {"border-color", {"border-top-color", "border-right-color", "border-bottom-color", "border-left-color"}},
    {"border-radius",{"border-top-left-radius", "border-top-right-radius", "border-bottom-right-radius", "border-bottom-left-radius"}},
    {"background",   {"background-color", "background-image", "background-repeat",
                      "background-attachment", "background-position", "background-size"}},
    {"font",         {"font-style", "font-variant", "font-weight", "font-size",
                      "line-height", "font-family"}},
    {"list-style",   {"list-style-type", "list-style-position", "list-style-image"}},
    {"flex",         {"flex-grow", "flex-shrink", "flex-basis"}},
    {"transition",   {"transition-property", "transition-duration", "transition-timing-function", "transition-delay"}},
    {"animation",    {"animation-name", "animation-duration", "animation-timing-function",
                      "animation-delay", "animation-iteration-count", "animation-direction",
                      "animation-fill-mode", "animation-play-state"}},
};

// CSS value minification helpers
static std::string minify_css_value(const std::string& value) {
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
                // Check if we can shorten: #aabbcc -> #abc
                std::string hex = value.substr(i + 1, 6);
                if (hex[0] == hex[1] && hex[2] == hex[3] && hex[4] == hex[5]) {
                    result += '#';
                    result += hex[0];
                    result += hex[2];
                    result += hex[4];
                    i += 7;
                    continue;
                }
                result += value.substr(i, 7);
                i += 7;
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
                    unit == "ch" || unit == "vmin" || unit == "vmax" || unit == "deg" ||
                    unit == "rad" || unit == "s" || unit == "ms" || unit == "Hz" ||
                    unit == "kHz" || unit == "dpi" || unit == "dpcm" || unit == "dppx") {
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

        // Check for "1.0" -> "1"
        // (simplified — skip for now)

        // Remove unnecessary whitespace
        if (is_whitespace(value[i])) {
            if (!result.empty() && !is_whitespace(result.back())) {
                result += ' ';
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

    for (auto& rule : const_cast<std::vector<CSSRule>&>(doc.stylesheets())) {
        auto& decls = const_cast<std::vector<CSSRule::Declaration>&>(rule.declarations());

        // Build a map of property -> index
        std::unordered_map<std::string_view, size_t> prop_map;
        for (size_t i = 0; i < decls.size(); i++) {
            prop_map[decls[i].property] = i;
        }

        // Check each shorthand
        for (const auto& [shorthand, longhands] : SHORTHAND_MAP) {
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
    }

    return changed;
}

} // namespace tinyizer
