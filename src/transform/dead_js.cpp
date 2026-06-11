#include "optimizer.h"
#include <queue>

namespace tinyizer {

// Dead JS elimination via scope analysis.
//
// Walks the JSScope tree and collects original names that are:
//   - Declared but never referenced
//   - Not exported (no export keyword)
//
// For functions, uses reachability analysis: a function is alive if
// it is reachable from global scope or from an exported/live function.
// This correctly handles mutually-recursive dead functions (functions
// that only reference each other but are never called from outside
// the dead group).
//
// Populates doc.dead_js_names (original names before renaming);
// actual removal happens in optimize().

static void collect_all_functions(const JSScope* scope,
                                   std::unordered_map<std::string, const JSScope::FunctionInfo*>& all) {
    if (!scope) return;
    for (const auto& [name, fn] : scope->functions()) {
        all[name] = &fn;
    }
    for (const auto& child : scope->children()) {
        collect_all_functions(child.get(), all);
    }
}

static void collect_dead_names(const JSScope* scope, std::unordered_set<std::string>& dead) {
    if (!scope) return;

    // Step 1: collect ALL function infos across the scope tree
    std::unordered_map<std::string, const JSScope::FunctionInfo*> all_fns;
    collect_all_functions(scope, all_fns);

    // Step 2: reachability-based liveness for functions
    // A function is alive if it's exported, called from global scope,
    // or reachable from a live function.
    std::queue<std::string> work;
    std::unordered_set<std::string> live;

    for (const auto& [name, fn] : all_fns) {
        if (fn->is_exported) {
            live.insert(name);
            work.push(name);
        }
    }

    // Also add functions directly called from global scope
    for (const auto& [name, fn] : all_fns) {
        if (live.count(name)) continue;
        if (fn->callers.count("")) {  // empty string = global scope
            live.insert(name);
            work.push(name);
        }
    }

    // Propagate: any function called by a live function is also live
    while (!work.empty()) {
        std::string current = work.front();
        work.pop();
        // Find all functions that list 'current' as one of their callers
        for (const auto& [name, fn] : all_fns) {
            if (live.count(name)) continue;
            if (fn->callers.count(current)) {
                live.insert(name);
                work.push(name);
            }
        }
    }

    // Variables: declared but not referenced and not exported (unchanged)
    for (const auto& [name, var] : scope->variables()) {
        if (var.is_declared && !var.is_referenced && !var.is_exported) {
            dead.insert(name);
        }
    }

    // Functions: NOT in the live set → dead
    for (const auto& [name, fn] : scope->functions()) {
        if (!live.count(name)) {
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
