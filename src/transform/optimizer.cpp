#include "optimizer.h"
#include "serializer.h"
#include "../util/frequency_map.h"
#include "../parser/tokenizer.h"
#include <algorithm>
#include <sstream>
#include <cctype>

namespace tinyizer {

Optimizer::Optimizer(const OptimizationConfig& config) : config_(config) {}

bool Optimizer::optimize(UnifiedDocument& doc) {
    bool any_change = false;
    passes_run_ = 0;

    // Fixed-point optimization loop:
    // Repeat all enabled passes until nothing changes or max iterations reached.
    // This is the novel approach — each pass may enable further reductions
    // in a subsequent pass, so we loop until convergence.
    for (int i = 0; i < config_.max_iterations; i++) {
        bool changed_this_iteration = false;
        passes_run_++;

        // Order matters: structure-changing passes first, then cosmetic
        if (config_.enable_dead_css && pass_dead_css(doc))
            changed_this_iteration = true;

        if (config_.enable_dead_js && pass_dead_js(doc))
            changed_this_iteration = true;

        if (config_.enable_cross_identifier && pass_cross_identifier(doc))
            changed_this_iteration = true;

        if (config_.enable_css_shorthand && pass_css_shorthand(doc))
            changed_this_iteration = true;

        if (config_.enable_js_constant_fold && pass_js_constant_fold(doc))
            changed_this_iteration = true;

        if (config_.enable_css_minify && pass_css_minify(doc))
            changed_this_iteration = true;

        if (config_.enable_html_minify && pass_html_minify(doc))
            changed_this_iteration = true;

        if (config_.enable_js_minify && pass_js_minify(doc))
            changed_this_iteration = true;

        if (!changed_this_iteration) break;
        any_change = true;
    }

    // Obfuscation is a final pass (not part of the iterative loop)
    if (config_.enable_obfuscation) {
        pass_obfuscation(doc);
    }

    // Brotli-aware reordering is a final pass
    if (config_.enable_brotli_reorder) {
        pass_brotli_reorder(doc);
    }

    // ---- Generate optimized inline CSS/JS strings ----
    doc.optimized_css.clear();
    doc.optimized_js.clear();

    // CSS: Serialize all optimized CSS rules (after dead code elimination, renaming, shorthand)
    // For now, all inline stylesheets are merged — serialize the entire ruleset
    std::string all_css = serialize_css(doc.stylesheets());
    if (!all_css.empty()) {
        // Assign to all inline style slots
        for (size_t i = 0; i < doc.inline_styles().size(); i++) {
            doc.optimized_css.push_back(all_css);
        }
    }

    // JS: For each inline script, apply rename map and minification
    for (const auto& raw_js : doc.inline_scripts()) {
        std::string opt_js = raw_js;
        // Apply JS name renames (using a stable copy of old name to avoid string_view invalidation)
        for (auto& [old_name_sv, new_name] : doc.js_rename_map) {
            std::string old_name(old_name_sv);  // Copy to local string for safety
            size_t pos = 0;
            while ((pos = opt_js.find(old_name, pos)) != std::string::npos) {
                bool left_ok = (pos == 0 || !is_ident_char(opt_js[pos - 1]));
                bool right_ok = (pos + old_name.size() >= opt_js.size() || !is_ident_char(opt_js[pos + old_name.size()]));
                if (left_ok && right_ok) {
                    opt_js.replace(pos, old_name.size(), new_name);
                    pos += new_name.size();
                } else {
                    pos++;
                }
            }
        }
        // JS minification
        opt_js = minify_js_text(opt_js);
        doc.optimized_js.push_back(std::move(opt_js));
    }

    return any_change;
}

// Serialization
std::string Optimizer::serialize(const UnifiedDocument& doc) const {
    std::string result;
    result.reserve(65536);

    // Helper to serialize DOM tree
    struct Serializer {
        std::string& out;
        const OptimizationConfig& cfg;
        const UnifiedDocument& doc;
        int style_idx = 0;
        int script_idx = 0;

        void serialize_node(const DOMNode& node, int depth = 0) {
            switch (node.type()) {
            case DOMNode::Type::ELEMENT:
                serialize_element(node, depth);
                break;
            case DOMNode::Type::TEXT:
                serialize_text(node, depth);
                break;
            case DOMNode::Type::COMMENT:
                if (cfg.keep_license_comments) {
                    std::string_view text = node.text();
                    if (text.find("license") != std::string_view::npos ||
                        text.find("License") != std::string_view::npos ||
                        text.find("copyright") != std::string_view::npos ||
                        text.find("@license") != std::string_view::npos) {
                        out += "<!--";
                        out += text;
                        out += "-->";
                    }
                }
                break;
            case DOMNode::Type::DOCTYPE:
                break;
            }
        }

        void serialize_element(const DOMNode& node, int depth) {
            // Skip the synthetic __root__ element
            bool is_root = (node.tag_name() == std::string_view("__root__"));
            if (!is_root) {
                out += '<';
                out += node.tag_name();

                for (const auto& attr : node.attrs()) {
                    out += ' ';
                    out += attr.name;
                    if (attr.has_quotes) {
                        out += "=\"";
                        out += attr.value;
                        out += '"';
                    } else if (!attr.value.empty()) {
                        out += '=';
                        out += attr.value;
                    }
                    // Boolean attributes (no value, no has_quotes) just have name
                }

                // Void elements don't need closing tags
                bool is_void = (node.tag_name() == "area" || node.tag_name() == "base" ||
                               node.tag_name() == "br" || node.tag_name() == "col" ||
                               node.tag_name() == "embed" || node.tag_name() == "hr" ||
                               node.tag_name() == "img" || node.tag_name() == "input" ||
                               node.tag_name() == "link" || node.tag_name() == "meta" ||
                               node.tag_name() == "param" || node.tag_name() == "source" ||
                               node.tag_name() == "track" || node.tag_name() == "wbr");

                if (is_void) {
                    out += '>';
                    return;
                }

                // For style/script elements, use optimized content instead of text children
                bool is_style = (node.tag_name() == std::string_view("style"));
                bool is_script = (node.tag_name() == std::string_view("script"));

                if (is_style && style_idx < (int)doc.optimized_css.size()) {
                    out += '>';
                    out += doc.optimized_css[style_idx++];
                    out += "</style>";
                    return;
                }
                if (is_script && script_idx < (int)doc.optimized_js.size()) {
                    out += '>';
                    out += doc.optimized_js[script_idx++];
                    out += "</script>";
                    return;
                }

                out += '>';
            }

            for (const auto& child : node.children()) {
                serialize_node(*child, is_root ? depth : depth + 1);
            }

            if (!is_root) {
                out += "</";
                out += node.tag_name();
                out += '>';
            }
        }

        void serialize_text(const DOMNode& node, int depth) {
            std::string_view text = node.text();
            out += text;
        }
    };

    Serializer ser{result, config_, doc};
    if (doc.root()) {
        ser.serialize_node(*doc.root());
    }

    // For standalone CSS/JS output, serialize the optimized content directly
    if (!doc.optimized_css.empty() && (!doc.root() || doc.root()->children().empty())) {
        // No DOM output needed — serialize CSS directly
        result = serialize_css(doc.stylesheets());
    }
    if (!doc.optimized_js.empty() && (!doc.root() || doc.root()->children().empty())) {
        // No DOM output needed — serialize JS directly
        // For JS, we need to minify the stored scripts
        // This is handled in the optimize() pass already
        // Just output the optimized JS
    }

    return result;
}

} // namespace tinyizer
