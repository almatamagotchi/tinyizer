#include "optimizer.h"
#include <algorithm>

namespace tinyizer {

// CSS minification: value-level optimizations that happen during serialization.
// This pass handles things best done with a CSS-aware view.

bool Optimizer::pass_css_minify(UnifiedDocument& doc) {
    bool changed = false;

    for (auto& rule : const_cast<std::vector<CSSRule>&>(doc.stylesheets())) {
        for (auto& decl : const_cast<std::vector<CSSRule::Declaration>&>(rule.declarations())) {
            // Minify zero values: 0px -> 0, 0em -> 0, etc.
            std::string_view val = decl.value;

            // Color minification
            if (decl.property == std::string_view("color") ||
                decl.property == std::string_view("background-color") ||
                decl.property == std::string_view("border-color") ||
                decl.property == std::string_view("outline-color")) {

                // Try to shorten hex colors
                if (val.size() == 7 && val[0] == '#') {
                    if (val[1] == val[2] && val[3] == val[4] && val[5] == val[6]) {
                        // #rrggbb -> #rgb
                        char shortened[5] = {'#', val[1], val[3], val[5], '\0'};
                        decl.value = doc.string_pool().intern(std::string_view(shortened, 4));
                        changed = true;
                    }
                }

                // Named color shortening: e.g., #ff0000 -> red
                // (large table — could implement for common colors)
            }

            // Minify font-weight: "bold" -> "700", "normal" -> "400"
            if (decl.property == std::string_view("font-weight")) {
                if (val == "bold") {
                    decl.value = doc.string_pool().intern("700");
                    changed = true;
                } else if (val == "normal") {
                    decl.value = doc.string_pool().intern("400");
                    changed = true;
                }
            }

            // Map "none" → "0" for border/outline shorthands (equivalent rendering, saves 2 chars)
            // Safe: border:0 sets width to 0, same visual result as border:none
            // Not applied to border-style sub-properties — "0" is not a valid style value
            if (val == "none") {
                if (decl.property == std::string_view("border") ||
                    decl.property == std::string_view("border-top") ||
                    decl.property == std::string_view("border-right") ||
                    decl.property == std::string_view("border-bottom") ||
                    decl.property == std::string_view("border-left") ||
                    decl.property == std::string_view("outline")) {
                    decl.value = doc.string_pool().intern("0");
                    changed = true;
                }
            }
        }
    }

    return changed;
}

// Strip CSS declarations whose value matches the CSS initial value for that property.
// Only safe for non-inherited properties where UA stylesheets don't set non-initial values.
bool Optimizer::pass_css_default_strip(UnifiedDocument& doc) {
    // Properties that are NOT inherited AND safe to strip when set to their initial value.
    // No UA stylesheet sets these to anything other than the initial value for any element.
    //
    // A property-value pair is only stripped if the value exactly matches the initial value
    // (after prior minification passes have normalized the value — e.g., font-weight:400
    // was already normalized from "normal" by pass_css_minify).
    //
    // Note: does NOT handle shorthands like overflow or background — only longhand/simple
    // properties to avoid cascade-side-effect complexity.
    static const std::unordered_map<std::string_view, std::string_view> INITIAL_VALUES = {
        {"position", "static"},
        {"float", "none"},
        {"clear", "none"},
        {"opacity", "1"},
        {"transform", "none"},
        {"filter", "none"},
        {"resize", "none"},
        {"perspective", "none"},
        {"backface-visibility", "visible"},
        {"text-decoration-line", "none"},
        {"text-decoration-style", "solid"},
        {"text-decoration-color", "currentcolor"},
        {"text-overflow", "clip"},
        {"mix-blend-mode", "normal"},
        {"isolation", "auto"},
    };

    bool changed = false;

    for (auto& rule : const_cast<std::vector<CSSRule>&>(doc.stylesheets())) {
        auto& decls = const_cast<std::vector<CSSRule::Declaration>&>(rule.declarations());

        // Track which properties we strip — needed because the value comparison
        // must use the post-minification value (e.g., "normal" → "400" for font-weight
        // was already done by pass_css_minify before this pass runs).
        std::vector<size_t> to_remove;

        for (size_t i = 0; i < decls.size(); i++) {
            auto it = INITIAL_VALUES.find(decls[i].property);
            if (it != INITIAL_VALUES.end() && decls[i].value == it->second) {
                to_remove.push_back(i);
            }
        }

        // Remove in reverse order to preserve indices
        for (size_t j = to_remove.size(); j > 0; j--) {
            decls.erase(decls.begin() + to_remove[j - 1]);
            changed = true;
        }
    }

    return changed;
}

} // namespace tinyizer
