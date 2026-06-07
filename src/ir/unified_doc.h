#pragma once
#include "dom_node.h"
#include "css_rule.h"
#include "js_scope.h"
#include "../util/string_pool.h"
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

namespace tinyizer {

// The complete parsed document — HTML with inline and external CSS/JS.
// This is the central IR that all optimization passes operate on.
class UnifiedDocument {
public:
    UnifiedDocument() : js_root_scope_(std::make_unique<JSScope>(JSScope::Kind::GLOBAL)) {}

    // Accessors
    DOMNode* root() const { return root_.get(); }
    void set_root(std::unique_ptr<DOMNode> root) { root_ = std::move(root); }

    const std::vector<CSSRule>& stylesheets() const { return stylesheets_; }
    void add_stylesheet(std::vector<CSSRule> rules) {
        for (auto& rule : rules) stylesheets_.push_back(std::move(rule));
    }

    const std::vector<std::string>& inline_scripts() const { return inline_scripts_; }
    void add_inline_script(std::string code) { inline_scripts_.push_back(std::move(code)); }

    // Store raw text of inline styles for re-embedding
    const std::vector<std::string>& inline_styles() const { return inline_styles_; }
    void add_inline_style(std::string css) { inline_styles_.push_back(std::move(css)); }

    // After optimization, these hold the serialized optimized CSS/JS
    std::vector<std::string> optimized_css;
    std::vector<std::string> optimized_js;

    // Rename maps from cross-identifier pass (old name → new name)
    std::unordered_map<std::string_view, std::string> css_rename_map;
    std::unordered_map<std::string_view, std::string> js_rename_map;

    JSScope* js_root_scope() const { return js_root_scope_.get(); }

    StringPool& string_pool() { return string_pool_; }
    const StringPool& string_pool() const { return string_pool_; }

    // Statistics
    size_t total_raw_bytes() const { return total_raw_bytes_; }
    void set_total_raw_bytes(size_t n) { total_raw_bytes_ = n; }

    size_t total_minified_bytes() const { return total_minified_bytes_; }
    void set_total_minified_bytes(size_t n) { total_minified_bytes_ = n; }

private:
    std::unique_ptr<DOMNode> root_;
    std::vector<CSSRule> stylesheets_;
    std::vector<std::string> inline_scripts_;
    std::vector<std::string> inline_styles_;
    std::unique_ptr<JSScope> js_root_scope_;
    StringPool string_pool_;
    size_t total_raw_bytes_ = 0;
    size_t total_minified_bytes_ = 0;
};

} // namespace tinyizer
