#pragma once
#include "../ir/unified_doc.h"
#include <string>

namespace tinyizer {

class JSNode;
class JSScope;

// Serialize a collection of CSS rules to a minified CSS string.
// Used both for standalone .css output and for re-embedding
// optimized inline styles back into HTML.
std::string serialize_css(const std::vector<CSSRule>& rules);

// Serialize a JS scope tree (AST) to a minified JavaScript string.
// Used both for standalone .js output and for re-embedding
// optimized inline scripts back into HTML.
std::string serialize_js(const JSScope* root_scope, const JSNode* ast_root);

// Minify JS text: strip whitespace and comments, preserving semantics.
std::string minify_js_text(const std::string& js);

// Minify CSS text: strip whitespace and comments from raw CSS.
// Used as fallback when CSS parser/serializer pipeline is not available.
std::string minify_css_text(const std::string& css);

// Fold constant expressions in JS text: 2+3 → 5, "a"+"b" → "ab", !0 → true.
// Used by both the iterative pass loop and the final serialization step.
std::string fold_constants_in_text(const std::string& js);

// Minify a single CSS value: compress colors, drop zero units, remove leading zeros.
// Applied during CSS serialization and shorthand collapse.
std::string minify_css_value(const std::string& value);

} // namespace tinyizer
