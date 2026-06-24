#pragma once
#include "dom_node.h"
#include "css_rule.h"
#include "js_scope.h"
#include "../parser/js_parser.h"
#include "../util/string_pool.h"
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>

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
    std::vector<CSSRule>& stylesheets() { return stylesheets_; }
    void add_stylesheet(std::vector<CSSRule> rules) {
        for (auto& rule : rules) stylesheets_.push_back(std::move(rule));
    }

    const std::vector<std::string>& inline_scripts() const { return inline_scripts_; }
    void add_inline_script(std::string code) { inline_scripts_.push_back(std::move(code)); }
    void set_inline_script(size_t idx, std::string code) { if (idx < inline_scripts_.size()) inline_scripts_[idx] = std::move(code); }

    // Store raw text of inline styles for re-embedding
    const std::vector<std::string>& inline_styles() const { return inline_styles_; }
    void add_inline_style(std::string css) { inline_styles_.push_back(std::move(css)); }

    // After optimization, these hold the serialized optimized CSS/JS
    std::vector<std::string> optimized_css;
    std::vector<std::string> optimized_js;

    // Per-script AST roots (populated during parsing, used by source-range passes)
    const std::vector<std::unique_ptr<JSNode>>& js_script_asts() const { return js_script_asts_; }
    void add_js_script_ast(std::unique_ptr<JSNode> ast) { js_script_asts_.push_back(std::move(ast)); }

    // Rename maps from cross-identifier pass (old name → new name)
    std::unordered_map<std::string_view, std::string> css_rename_map;
    std::unordered_map<std::string_view, std::string> js_rename_map;

    // Dead JS names (from pass_dead_js) — these declarations should be stripped
    std::unordered_set<std::string> dead_js_names;

    // CSS classes detected in JS (e.g., classList.add("foo")), original names
    std::unordered_set<std::string> js_touched_classes;

    // IDs detected in JS string literals (e.g., getElementById("foo")), original names
    std::unordered_set<std::string> js_touched_ids;

    // Cumulative rename: original identifier → current short name
    std::unordered_map<std::string, std::string> cumulative_rename;

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
    std::vector<std::unique_ptr<JSNode>> js_script_asts_;
    std::unique_ptr<JSScope> js_root_scope_;
    StringPool string_pool_;
    size_t total_raw_bytes_ = 0;
    size_t total_minified_bytes_ = 0;
};

} // namespace tinyizer
