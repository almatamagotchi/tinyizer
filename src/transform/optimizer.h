#pragma once
#include "../ir/unified_doc.h"
#include <string>
#include <unordered_map>

namespace tinyizer {

struct OptimizationConfig {
    // Pass control
    bool enable_html_minify = true;
    bool enable_css_minify = true;
    bool enable_js_minify = true;
    bool enable_cross_identifier = true;
    bool enable_dead_css = true;
    bool enable_dead_js = true;
    bool enable_css_shorthand = true;
    bool enable_js_constant_fold = true;
    bool enable_brotli_reorder = false;  // experimental
    bool enable_obfuscation = false;

    // Debug flags
    bool debug_dead_js = false;

    // Max iterations for fixed-point optimization loop
    int max_iterations = 10;

    // Obfuscation settings
    bool obfuscate_strings = false;
    bool obfuscate_control_flow = false;

    // Output formatting
    bool keep_license_comments = false;
    bool shorten_color_values = true;
    bool remove_optional_tags = true;
};

// The main optimization pipeline. Runs passes in a fixed-point loop.
class Optimizer {
public:
    explicit Optimizer(const OptimizationConfig& config = {});

    // Run all enabled optimization passes on the document.
    // Returns true if any changes were made.
    bool optimize(UnifiedDocument& doc);

    // After optimization, serialize back to a string
    std::string serialize(const UnifiedDocument& doc) const;

    // Stats
    size_t bytes_saved() const { return bytes_saved_; }
    int passes_run() const { return passes_run_; }

private:
    // Individual passes (each returns true if it changed anything)
    bool pass_html_minify(UnifiedDocument& doc);
    bool pass_css_minify(UnifiedDocument& doc);
    bool pass_js_minify(UnifiedDocument& doc);
    bool pass_cross_identifier(UnifiedDocument& doc);
    bool pass_dead_css(UnifiedDocument& doc);
    bool pass_dead_js(UnifiedDocument& doc);
    bool pass_css_shorthand(UnifiedDocument& doc);
    bool pass_css_value_fold(UnifiedDocument& doc);
    bool pass_css_math_fold(UnifiedDocument& doc);
    bool pass_css_default_strip(UnifiedDocument& doc);
    bool pass_js_constant_fold(UnifiedDocument& doc);
    bool pass_brotli_reorder(UnifiedDocument& doc);
    bool pass_obfuscation(UnifiedDocument& doc);

    OptimizationConfig config_;
    size_t bytes_saved_ = 0;
    int passes_run_ = 0;
};

} // namespace tinyizer
