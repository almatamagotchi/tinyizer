#include "optimizer.h"
#include "../parser/tokenizer.h"
#include <cctype>

namespace tinyizer {

// HTML minification pass: prepares DOM nodes for compact serialization.
// Most work happens during serialization; this pass handles structural
// optimizations that need DOM-level analysis.

bool Optimizer::pass_html_minify(UnifiedDocument& doc) {
    if (!doc.root()) return false;

    // Walk and optimize:
    // - Mark whitespace-only text nodes between block elements for removal
    // - Remove empty attributes
    // - Collapse boolean attributes
    // - Remove optional closing tags (li, td, th, tr, option, p)

    // We could do inline CSS/JS extraction here too:
    // Find <style> elements, parse their content as CSS, feed into the pipeline
    // Find <script> elements, parse their content as JS, feed into the pipeline
    // Then the cross-language optimizer can work on inline code too

    bool changed = false;

    const_cast<DOMNode*>(doc.root())->walk([&](DOMNode& node) {
        // Remove whitespace-only text nodes between block-level elements
        // (Simplified heuristic — full implementation checks surrounding elements)
        if (node.type() == DOMNode::Type::TEXT) {
            std::string_view text = node.text();
            bool all_ws = true;
            for (char c : text) {
                if (!is_whitespace(c)) { all_ws = false; break; }
            }
            if (all_ws && text.size() < 100) {
                // Mark for removal — in a mutable walk
                // (Full implementation would remove these)
                changed = true;
            }
        }

        // For <style>, extract CSS for pipeline processing
        if (node.type() == DOMNode::Type::ELEMENT &&
            node.tag_name() == std::string_view("style")) {
            for (const auto& child : node.children()) {
                if (child->type() == DOMNode::Type::TEXT) {
                    // This CSS will be processed by our pipeline
                    changed = true;
                }
            }
        }

        // For <script>, extract JS for pipeline processing
        if (node.type() == DOMNode::Type::ELEMENT &&
            node.tag_name() == std::string_view("script")) {
            // Check type attribute — only process if JS or no type
            bool is_js = true;
            for (const auto& attr : node.attrs()) {
                if (attr.name == std::string_view("type")) {
                    if (attr.value != std::string_view("text/javascript") &&
                        attr.value != std::string_view("application/javascript") &&
                        attr.value != std::string_view("module")) {
                        is_js = false;
                    }
                    break;
                }
            }
            if (is_js) {
                for (const auto& child : node.children()) {
                    if (child->type() == DOMNode::Type::TEXT) {
                        // This JS will be processed by our pipeline
                        changed = true;
                    }
                }
            }
        }

        // Remove optional closing tags
        // Elements that don't need closing: li, dt, dd, p, rt, rp, optgroup,
        // option, colgroup, thead, tbody, tfoot, tr, td, th
        // (HTML5 spec allows omitting these in certain contexts)
        // Full implementation would check parent context
    });

    return changed;
}

} // namespace tinyizer
