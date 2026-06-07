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

} // namespace tinyizer
