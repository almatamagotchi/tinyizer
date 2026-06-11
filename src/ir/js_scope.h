#pragma once
#include <string_view>
#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace tinyizer {

// Represents a JavaScript scope (global, function, block)
class JSScope {
public:
    enum class Kind : uint8_t {
        GLOBAL,
        FUNCTION,
        BLOCK,
    };

    struct Variable {
        std::string name;
        bool is_declared = false;   // let/var/const/function declared here
        bool is_referenced = false;  // actually used somewhere
        bool is_exported = false;    // exported or attached to global
    };

    struct FunctionInfo {
        std::string name;
        std::vector<std::string> param_names;
        bool is_referenced = false;
        bool is_exported = false;
        // Which functions/scopes reference this function.
        // Empty string key "" = referenced from global (non-function) scope.
        std::unordered_set<std::string> callers;
    };

    JSScope(Kind kind, JSScope* parent = nullptr);

    Kind kind() const { return kind_; }
    JSScope* parent() const { return parent_; }

    // Variable tracking — takes string_view but copies to internal std::string
    void declare_variable(std::string_view name, bool exported = false);
    void reference_variable(std::string_view name);
    Variable* find_variable(std::string_view name);

    // Function tracking
    void declare_function(std::string_view name, const std::vector<std::string_view>& params, bool exported = false);
    void reference_function(std::string_view name);
    FunctionInfo* find_function(std::string_view name);

    // Returns the name of the nearest enclosing FUNCTION scope, or empty if global.
    std::string enclosing_function_name() const;

    // Walk all declared names (for identifier squeezing)
    std::vector<std::string_view> all_declared_names() const;

    // Variables map access (for iteration in cross-identifier pass)
    const std::unordered_map<std::string, Variable>& variables() const { return variables_; }
    const std::unordered_map<std::string, FunctionInfo>& functions() const { return functions_; }

    JSScope* add_child(std::unique_ptr<JSScope> child);

    // Set the function name for FUNCTION-kind scopes (used for call-graph tracking)
    void set_function_name(std::string name) { func_name_ = std::move(name); }

    const std::vector<std::unique_ptr<JSScope>>& children() const { return children_; }

private:
    Kind kind_;
    JSScope* parent_ = nullptr;
    std::unordered_map<std::string, Variable> variables_;
    std::unordered_map<std::string, FunctionInfo> functions_;
    std::vector<std::unique_ptr<JSScope>> children_;
    // Names referenced before their declaration (handles function hoisting).
    // When declare_function/declare_variable encounters a name in these sets,
    // it marks the declaration as referenced.
    std::unordered_set<std::string> pending_variable_refs_;
    std::unordered_set<std::string> pending_function_refs_;
    // Callers for functions referenced before declaration (hoisting).
    // Keyed by function name, value = set of caller names.
    std::unordered_map<std::string, std::unordered_set<std::string>> pending_function_callers_;
    // Name of the enclosing function (only set for FUNCTION-kind scopes).
    std::string func_name_;
};

} // namespace tinyizer
