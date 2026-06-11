#include "js_scope.h"

namespace tinyizer {

JSScope::JSScope(Kind kind, JSScope* parent)
    : kind_(kind), parent_(parent) {}

void JSScope::declare_variable(std::string_view name, bool exported) {
    std::string key(name);
    auto& var = variables_[key];
    var.name = std::move(key);
    var.is_declared = true;
    if (exported) var.is_exported = true;
}

void JSScope::reference_variable(std::string_view name) {
    std::string key(name);
    JSScope* scope = this;
    while (scope) {
        auto it = scope->variables_.find(key);
        if (it != scope->variables_.end()) {
            it->second.is_referenced = true;
            return;
        }
        scope = scope->parent_;
    }
    // Not found — might be a global. Record at current scope.
    auto& var = variables_[key];
    if (var.name.empty()) var.name = key;
    var.is_referenced = true;
}

JSScope::Variable* JSScope::find_variable(std::string_view name) {
    std::string key(name);
    JSScope* scope = this;
    while (scope) {
        auto it = scope->variables_.find(key);
        if (it != scope->variables_.end()) return &it->second;
        scope = scope->parent_;
    }
    return nullptr;
}

void JSScope::declare_function(std::string_view name, const std::vector<std::string_view>& params, bool exported) {
    std::string key(name);
    auto& fn = functions_[key];
    fn.name = key;
    fn.param_names.clear();
    for (auto p : params) fn.param_names.push_back(std::string(p));
    if (exported) fn.is_exported = true;
    // If this function was referenced before its declaration (hoisting),
    // mark it as referenced and copy over callers.
    if (pending_function_refs_.count(key)) {
        fn.is_referenced = true;
        // Copy any callers recorded before the declaration
        auto cit = pending_function_callers_.find(key);
        if (cit != pending_function_callers_.end()) {
            fn.callers = std::move(cit->second);
            pending_function_callers_.erase(cit);
        }
        pending_function_refs_.erase(key);
    }
}

void JSScope::reference_function(std::string_view name) {
    std::string key(name);
    std::string caller = enclosing_function_name();
    JSScope* scope = this;
    while (scope) {
        auto it = scope->functions_.find(key);
        if (it != scope->functions_.end()) {
            it->second.is_referenced = true;
            it->second.callers.insert(caller);
            return;
        }
        scope = scope->parent_;
    }
    // Not found — add to pending set in the current scope.
    // When the function declaration is later processed (hoisting),
    // declare_function will check this set and mark it as referenced.
    pending_function_refs_.insert(std::move(key));
    // Also record the caller for the pending ref (applied when declared).
    // Store in a separate pending-callers map.
    pending_function_callers_[key].insert(caller);
}

JSScope::FunctionInfo* JSScope::find_function(std::string_view name) {
    std::string key(name);
    JSScope* scope = this;
    while (scope) {
        auto it = scope->functions_.find(key);
        if (it != scope->functions_.end()) return &it->second;
        scope = scope->parent_;
    }
    return nullptr;
}

std::vector<std::string_view> JSScope::all_declared_names() const {
    std::vector<std::string_view> result;
    for (const auto& [name, var] : variables_) {
        if (var.is_declared) result.push_back(name);
    }
    for (const auto& [name, fn] : functions_) {
        if (!fn.name.empty()) result.push_back(name);
    }
    for (const auto& child : children_) {
        auto child_names = child->all_declared_names();
        result.insert(result.end(), child_names.begin(), child_names.end());
    }
    return result;
}

JSScope* JSScope::add_child(std::unique_ptr<JSScope> child) {
    child->parent_ = this;
    JSScope* raw = child.get();
    children_.push_back(std::move(child));
    return raw;
}

std::string JSScope::enclosing_function_name() const {
    const JSScope* s = this;
    while (s) {
        if (s->kind_ == Kind::FUNCTION) {
            return s->func_name_;
        }
        s = s->parent_;
    }
    return "";  // global scope
}

} // namespace tinyizer
