#include "optimizer.h"

namespace tinyizer {

// Dead JS elimination via scope analysis.
//
// Walks the JSScope tree and collects original names that are:
//   - Declared but never referenced
//   - Not exported (no export keyword)
//
// Populates doc.dead_js_names (original names before renaming);
// actual removal happens in optimize().

static void collect_dead_names(const JSScope* scope, std::unordered_set<std::string>& dead) {
    if (!scope) return;

    // Variables: declared but not referenced and not exported
    for (const auto& [name, var] : scope->variables()) {
        if (var.is_declared && !var.is_referenced && !var.is_exported) {
            dead.insert(name);
        }
    }

    // Functions: not referenced and not exported
    for (const auto& [name, fn] : scope->functions()) {
        if (!fn.is_referenced && !fn.is_exported) {
            dead.insert(name);
        }
    }

    for (const auto& child : scope->children()) {
        collect_dead_names(child.get(), dead);
    }
}

bool Optimizer::pass_dead_js(UnifiedDocument& doc) {
    if (!doc.js_root_scope()) return false;

    std::unordered_set<std::string> dead;
    collect_dead_names(doc.js_root_scope(), dead);

    if (dead == doc.dead_js_names) {
        return false;
    }

    doc.dead_js_names = std::move(dead);
    return true;
}

} // namespace tinyizer
