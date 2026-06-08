#pragma once
#include "tokenizer.h"
#include "../ir/js_scope.h"
#include <memory>
#include <vector>
#include <string>

namespace tinyizer {

// AST node types for JavaScript
enum class JSNodeType : uint8_t {
    PROGRAM,
    BLOCK_STMT,
    EXPR_STMT,
    VAR_DECL,        // let/var/const x = ...
    FUNC_DECL,       // function name() { ... }
    FUNC_EXPR,       // function() { ... } or () => { ... }
    RETURN_STMT,
    IF_STMT,
    FOR_STMT,
    WHILE_STMT,
    ASSIGN_EXPR,     // x = ...
    CALL_EXPR,       // f()
    MEMBER_EXPR,     // a.b, a[b]
    IDENTIFIER,
    LITERAL,         // number, string, true, false, null, undefined
    BINARY_EXPR,     // a + b
    UNARY_EXPR,      // !a, typeof a, -a
    OBJECT_LITERAL,
    ARRAY_LITERAL,
    TEMPLATE_LITERAL,
    CONDITIONAL_EXPR, // a ? b : c
    NEW_EXPR,
    EXPORT_DECL,
    IMPORT_DECL,
};

struct JSNode {
    JSNodeType type;
    std::string_view value;    // identifier name, literal value, operator
    std::vector<std::unique_ptr<JSNode>> children;
    JSScope* scope = nullptr;  // scope where this was parsed (for resolution)
    bool is_declaration = false;
    bool is_reference = false;
    size_t src_start = 0;      // byte offset in source
    size_t src_end = 0;        // byte offset in source (exclusive)

    JSNode(JSNodeType t) : type(t) {}
    JSNode(JSNodeType t, std::string_view v) : type(t), value(v) {}
};

// Parses JavaScript into an AST with scope analysis.
class JSParser {
public:
    JSParser();

    // Parse JS source. Returns root AST node with full scope tree.
    std::unique_ptr<JSNode> parse(std::string_view js, JSScope* root_scope);

    JSScope* scope() const { return scope_; }

    // For identifier collection: get all referenced DOM accesses
    struct DOMAccess {
        std::string_view property;  // e.g., "getElementById", "className"
        std::string_view argument;  // e.g., the string arg: "myId"
    };
    std::vector<DOMAccess> dom_accesses() const { return dom_accesses_; }

private:
    // Statement parsing
    std::unique_ptr<JSNode> parse_statement();
    std::unique_ptr<JSNode> parse_block();
    std::unique_ptr<JSNode> parse_var_declaration();
    std::unique_ptr<JSNode> parse_function_declaration();
    std::unique_ptr<JSNode> parse_return_statement();
    std::unique_ptr<JSNode> parse_if_statement();
    std::unique_ptr<JSNode> parse_for_statement();
    std::unique_ptr<JSNode> parse_while_statement();
    std::unique_ptr<JSNode> parse_export_import();

    // Expression parsing (precedence climbing)
    std::unique_ptr<JSNode> parse_expression(int precedence = 0);
    std::unique_ptr<JSNode> parse_primary();
    std::unique_ptr<JSNode> parse_member_expression();
    std::unique_ptr<JSNode> parse_call_expression();
    std::unique_ptr<JSNode> parse_object_literal();
    std::unique_ptr<JSNode> parse_array_literal();
    std::unique_ptr<JSNode> parse_function_expression(bool is_arrow = false, size_t start = 0);

    // Helpers
    void skip_comments_and_whitespace();
    void parse_call_arguments(std::unique_ptr<JSNode>& call);
    void enter_scope(std::unique_ptr<JSScope> scope);
    void exit_scope();
    static int precedence_of(std::string_view op);

    // Stamp source positions on a newly built node
    std::unique_ptr<JSNode> stamp_node(std::unique_ptr<JSNode> node, size_t start);

    // Track DOM accesses
    void maybe_track_dom_access(std::unique_ptr<JSNode>& node);

    std::unique_ptr<Tokenizer> tok_;
    JSScope* root_scope_ = nullptr;
    JSScope* scope_ = nullptr; // current scope during parsing
    std::vector<DOMAccess> dom_accesses_;

    // Keyword detection
    bool is_keyword(std::string_view word) const;
};

} // namespace tinyizer
