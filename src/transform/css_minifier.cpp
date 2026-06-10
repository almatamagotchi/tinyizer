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

} // namespace tinyizer
