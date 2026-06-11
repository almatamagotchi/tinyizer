#include "js_parser.h"
#include <cctype>
#include <unordered_set>
#include <algorithm>
#include <memory>

namespace tinyizer {

// JS keywords (reserved words)
static const std::unordered_set<std::string_view> JS_KEYWORDS = {
    "break", "case", "catch", "continue", "debugger", "default", "delete",
    "do", "else", "finally", "for", "function", "if", "in", "instanceof",
    "new", "return", "switch", "this", "throw", "try", "typeof", "var",
    "void", "while", "with", "class", "const", "enum", "export", "extends",
    "import", "super", "implements", "interface", "let", "package", "private",
    "protected", "public", "static", "yield", "async", "await", "of"
};

// Operator precedence (higher = binds tighter)
static int op_precedence(std::string_view op) {
    if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=" ||
        op == "%=" || op == "&=" || op == "|=" || op == "^=" || op == "&&=" ||
        op == "||=" || op == "??=" || op == "<<=" || op == ">>=" || op == ">>>=" ||
        op == "**=") return 0;
    if (op == "||" || op == "??") return 1;
    if (op == "&&") return 2;
    if (op == "|") return 3;
    if (op == "^") return 4;
    if (op == "&") return 5;
    if (op == "==" || op == "!=" || op == "===" || op == "!==") return 6;
    if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "instanceof" || op == "in") return 7;
    if (op == "<<" || op == ">>" || op == ">>>") return 8;
    if (op == "+" || op == "-") return 9;
    if (op == "*" || op == "/" || op == "%") return 10;
    if (op == "**") return 11;
    return -1; // unknown
}

int JSParser::precedence_of(std::string_view op) {
    return op_precedence(op);
}

bool JSParser::is_keyword(std::string_view word) const {
    return JS_KEYWORDS.count(word) > 0;
}

JSParser::JSParser() {}

std::unique_ptr<JSNode> JSParser::parse(std::string_view js, JSScope* root_scope) {
    tok_ = std::make_unique<Tokenizer>(js);
    root_scope_ = root_scope;
    scope_ = root_scope;

    auto program = std::make_unique<JSNode>(JSNodeType::PROGRAM);
    program->scope = scope_;

    while (!tok_->eof()) {
        skip_comments_and_whitespace();
        if (tok_->eof()) break;

        auto stmt = parse_statement();
        if (stmt) {
            program->children.push_back(std::move(stmt));
        }
    }

    return stamp_node(std::move(program), 0);
}

void JSParser::skip_comments_and_whitespace() {
    while (!tok_->eof()) {
        tok_->skip_whitespace();

        // Line comment
        if (tok_->peek() == '/' && tok_->peek_ahead(1) == '/') {
            tok_->eat_line();
            continue;
        }

        // Block comment
        if (tok_->peek() == '/' && tok_->peek_ahead(1) == '*') {
            tok_->skip(2);
            while (!tok_->eof()) {
                if (tok_->peek() == '*' && tok_->peek_ahead(1) == '/') {
                    tok_->skip(2);
                    break;
                }
                tok_->advance();
            }
            continue;
        }

        break;
    }
}

std::unique_ptr<JSNode> JSParser::parse_statement() {
    skip_comments_and_whitespace();
    if (tok_->eof()) return nullptr;

    char c = tok_->peek();
    std::string_view word;

    // Semicolons (empty statements)
    if (c == ';') {
        tok_->advance();
        return std::make_unique<JSNode>(JSNodeType::EXPR_STMT);
    }

    // Block
    if (c == '{') return parse_block();

    // Keyword-driven statements
    if (is_ident_start(c)) {
        size_t saved = tok_->pos();
        word = tok_->read_identifier();
        tok_->set_pos(saved);

        if (word == "var" || word == "let" || word == "const") {
            return parse_var_declaration();
        }
        if (word == "function") {
            return parse_function_declaration();
        }
        if (word == "return") {
            return parse_return_statement();
        }
        if (word == "if") {
            return parse_if_statement();
        }
        if (word == "for") {
            return parse_for_statement();
        }
        if (word == "while") {
            return parse_while_statement();
        }
        if (word == "export" || word == "import") {
            return parse_export_import();
        }
        if (word == "class") {
            // Skip class declarations for now (parse the class body, track scope)
            // Simplified: consume until matching closing brace
            tok_->skip(word.size());
            skip_comments_and_whitespace();
            tok_->read_identifier(); // class name
            // Maybe extends
            skip_comments_and_whitespace();
            if (tok_->peek_match("extends")) {
                tok_->skip(7);
                skip_comments_and_whitespace();
                tok_->read_identifier(); // base class
            }
            // Class body
            if (tok_->peek() == '{') {
                int depth = 0;
                while (!tok_->eof()) {
                    if (tok_->peek() == '{') depth++;
                    else if (tok_->peek() == '}') { depth--; if (depth < 0) { tok_->advance(); break; } }
                    tok_->advance();
                }
            }
            return std::make_unique<JSNode>(JSNodeType::EXPR_STMT);
        }
        if (word == "switch") {
            // Skip switch
            tok_->skip(6);
            skip_comments_and_whitespace();
            if (tok_->peek() == '(') {
                int pd = 0;
                while (!tok_->eof()) { if (tok_->peek() == '(') pd++; else if (tok_->peek() == ')') { pd--; if (pd == 0) { tok_->advance(); break; } } tok_->advance(); }
            }
            if (tok_->peek() == '{') {
                int depth = 0;
                while (!tok_->eof()) {
                    if (tok_->peek() == '{') depth++;
                    else if (tok_->peek() == '}') { depth--; if (depth < 0) { tok_->advance(); break; } }
                    tok_->advance();
                }
            }
            return std::make_unique<JSNode>(JSNodeType::EXPR_STMT);
        }
        if (word == "try" || word == "throw" || word == "do") {
            // Skip
            tok_->skip(word.size());
            if (word == "do") {
                parse_statement();
                skip_comments_and_whitespace();
                if (tok_->peek_match("while")) {
                    tok_->skip(5);
                    skip_comments_and_whitespace();
                    if (tok_->peek() == '(') {
                        int pd = 1; tok_->advance();
                        while (!tok_->eof()) { if (tok_->peek() == '(') pd++; else if (tok_->peek() == ')') { pd--; if (pd == 0) { tok_->advance(); break; } } tok_->advance(); }
                    }
                }
            }
            tok_->match(';');
            return std::make_unique<JSNode>(JSNodeType::EXPR_STMT);
        }
    }

    // Expression statement
    auto expr = parse_expression();
    if (expr) {
        auto stmt = std::make_unique<JSNode>(JSNodeType::EXPR_STMT);
        stmt->children.push_back(std::move(expr));
        tok_->match(';'); // optional semicolon
        return stmt;
    }

    // Fallback: skip one char
    tok_->advance();
    return std::make_unique<JSNode>(JSNodeType::EXPR_STMT);
}

std::unique_ptr<JSNode> JSParser::parse_block() {
    size_t start = tok_->pos();
    tok_->match('{');
    auto block = std::make_unique<JSNode>(JSNodeType::BLOCK_STMT);

    // Enter new scope
    auto block_scope = std::make_unique<JSScope>(JSScope::Kind::BLOCK, scope_);
    enter_scope(std::move(block_scope));

    while (!tok_->eof()) {
        skip_comments_and_whitespace();
        if (tok_->eof()) break;
        if (tok_->peek() == '}') break;

        auto stmt = parse_statement();
        if (stmt) block->children.push_back(std::move(stmt));
    }

    exit_scope();

    tok_->match('}');
    return stamp_node(std::move(block), start);
}

std::unique_ptr<JSNode> JSParser::parse_var_declaration() {
    size_t start = tok_->pos();
    std::string_view keyword = tok_->read_identifier(); // var, let, const
    bool is_const = (keyword == "const");
    auto decl = std::make_unique<JSNode>(JSNodeType::VAR_DECL);
    decl->value = keyword;

    skip_comments_and_whitespace();

    // Parse comma-separated bindings
    while (!tok_->eof()) {
        skip_comments_and_whitespace();
        if (tok_->eof() || tok_->peek() == ';') break;

        std::string_view last_name;

        // Destructuring: [a, b] = ... or {a, b} = ...
        if (tok_->peek() == '[') {
            // Skip array destructuring
            int depth = 1; tok_->advance();
            while (!tok_->eof()) { if (tok_->peek() == '[') depth++; else if (tok_->peek() == ']') { depth--; if (depth == 0) { tok_->advance(); break; } } tok_->advance(); }
        } else if (tok_->peek() == '{') {
            // Skip object destructuring
            int depth = 1; tok_->advance();
            while (!tok_->eof()) { if (tok_->peek() == '{') depth++; else if (tok_->peek() == '}') { depth--; if (depth == 0) { tok_->advance(); break; } } tok_->advance(); }
        } else {
            // Simple identifier
            size_t id_start = tok_->pos();
            last_name = tok_->read_identifier();
            auto id = std::make_unique<JSNode>(JSNodeType::IDENTIFIER, last_name);
            id->is_declaration = true;
            decl->children.push_back(stamp_node(std::move(id), id_start));

            if (scope_) scope_->declare_variable(last_name);
        }

        skip_comments_and_whitespace();

        // Initializer
        if (tok_->peek() == '=') {
            tok_->advance();
            skip_comments_and_whitespace();
            size_t init_start = tok_->pos();
            auto init = parse_expression(1);
            if (init) {
                // Track const literal values for cross-scope propagation
                if (is_const && !last_name.empty() && scope_ &&
                    init->type == JSNodeType::LITERAL) {
                    scope_->set_const_value(last_name, init->value);
                }
                decl->children.push_back(stamp_node(std::move(init), init_start));
            }
        }

        // Comma or semicolon
        skip_comments_and_whitespace();
        if (tok_->peek() == ',') {
            tok_->advance();
        } else {
            break;
        }
    }

    tok_->match(';');
    return stamp_node(std::move(decl), start);
}

std::unique_ptr<JSNode> JSParser::parse_function_declaration() {
    size_t start = tok_->pos();
    tok_->skip(8); // "function"
    skip_comments_and_whitespace();

    // Optional name (may be anonymous for default exports)
    std::string_view name;
    if (is_ident_start(tok_->peek())) {
        name = tok_->read_identifier();
        if (scope_) scope_->declare_function(name, {});
    }

    auto func = std::make_unique<JSNode>(JSNodeType::FUNC_DECL, name);

    // Parameters
    if (tok_->peek() == '(') {
        tok_->advance();
        while (!tok_->eof() && tok_->peek() != ')') {
            skip_comments_and_whitespace();
            if (tok_->peek() == ')') break;
            std::string_view param = tok_->read_identifier();
            if (!param.empty()) {
                auto p = std::make_unique<JSNode>(JSNodeType::IDENTIFIER, param);
                p->is_declaration = true;
                func->children.push_back(std::move(p));
            }
            skip_comments_and_whitespace();
            tok_->match(',');
        }
        tok_->match(')');
    }

    // Body
    skip_comments_and_whitespace();

    // Enter function scope for the body
    auto func_scope = std::make_unique<JSScope>(JSScope::Kind::FUNCTION, scope_);
    if (!name.empty()) func_scope->set_function_name(std::string(name));
    // Add parameters to scope
    for (auto& child : func->children) {
        if (child->is_declaration && child->type == JSNodeType::IDENTIFIER) {
            func_scope->declare_variable(child->value);
        }
    }
    enter_scope(std::move(func_scope));

    auto body = parse_block();
    if (body) func->children.push_back(std::move(body));

    exit_scope();

    return stamp_node(std::move(func), start);
}

std::unique_ptr<JSNode> JSParser::parse_function_expression(bool is_arrow, size_t start) {
    std::vector<std::string_view> params;

    if (!is_arrow) {
        // Already consumed 'function' keyword
        if (is_ident_start(tok_->peek())) {
            tok_->read_identifier(); // optional name
        }

        if (tok_->peek() == '(') {
            tok_->advance();
            while (!tok_->eof() && tok_->peek() != ')') {
                skip_comments_and_whitespace();
                if (tok_->peek() == ')') break;
                std::string_view p = tok_->read_identifier();
                if (!p.empty()) params.push_back(p);
                skip_comments_and_whitespace();
                tok_->match(',');
            }
            tok_->match(')');
        }
    }

    skip_comments_and_whitespace();

    // Enter function scope
    auto func_scope = std::make_unique<JSScope>(JSScope::Kind::FUNCTION, scope_);
    for (auto& p : params) func_scope->declare_variable(p);
    enter_scope(std::move(func_scope));

    std::unique_ptr<JSNode> body;
    if (tok_->peek() == '{') {
        body = parse_block();
    } else {
        // Arrow function with expression body
        if (is_arrow) {
            auto expr = parse_expression();
            auto stmt = std::make_unique<JSNode>(JSNodeType::EXPR_STMT);
            stmt->children.push_back(std::move(expr));
            auto block = std::make_unique<JSNode>(JSNodeType::BLOCK_STMT);
            block->children.push_back(std::move(stmt));
            body = std::move(block);
        }
    }

    exit_scope();

    auto func = std::make_unique<JSNode>(JSNodeType::FUNC_EXPR);
    if (body) func->children.push_back(std::move(body));
    return stamp_node(std::move(func), start);
}

std::unique_ptr<JSNode> JSParser::parse_return_statement() {
    tok_->skip(6); // "return"
    auto ret = std::make_unique<JSNode>(JSNodeType::RETURN_STMT);

    skip_comments_and_whitespace();
    if (tok_->peek() != ';' && tok_->peek() != '}' && !tok_->eof()) {
        auto expr = parse_expression();
        if (expr) ret->children.push_back(std::move(expr));
    }

    tok_->match(';');
    return ret;
}

std::unique_ptr<JSNode> JSParser::parse_if_statement() {
    tok_->skip(2); // "if"
    auto stmt = std::make_unique<JSNode>(JSNodeType::IF_STMT);

    // Condition
    skip_comments_and_whitespace();
    if (tok_->peek() == '(') {
        tok_->advance();
        auto cond = parse_expression();
        if (cond) stmt->children.push_back(std::move(cond));
        tok_->match(')');
    }

    // Then branch
    auto then_branch = parse_statement();
    if (then_branch) stmt->children.push_back(std::move(then_branch));

    // Else branch
    skip_comments_and_whitespace();
    if (tok_->peek_match("else")) {
        tok_->skip(4);
        skip_comments_and_whitespace();
        auto else_branch = parse_statement();
        if (else_branch) stmt->children.push_back(std::move(else_branch));
    }

    return stmt;
}

std::unique_ptr<JSNode> JSParser::parse_for_statement() {
    tok_->skip(3); // "for"
    auto stmt = std::make_unique<JSNode>(JSNodeType::FOR_STMT);

    skip_comments_and_whitespace();
    if (tok_->peek() == '(') {
        tok_->advance();

        // Init
        skip_comments_and_whitespace();
        if (tok_->peek() != ';') {
            auto init = parse_statement();
            if (init) stmt->children.push_back(std::move(init));
        }
        tok_->match(';');

        // Test
        skip_comments_and_whitespace();
        if (tok_->peek() != ';') {
            auto test = parse_expression();
            if (test) stmt->children.push_back(std::move(test));
        }
        tok_->match(';');

        // Update
        skip_comments_and_whitespace();
        if (tok_->peek() != ')') {
            auto update = parse_expression();
            if (update) stmt->children.push_back(std::move(update));
        }
        tok_->match(')');
    }

    // Body
    auto body = parse_statement();
    if (body) stmt->children.push_back(std::move(body));

    return stmt;
}

std::unique_ptr<JSNode> JSParser::parse_while_statement() {
    tok_->skip(5); // "while"
    auto stmt = std::make_unique<JSNode>(JSNodeType::WHILE_STMT);

    skip_comments_and_whitespace();
    if (tok_->peek() == '(') {
        tok_->advance();
        auto cond = parse_expression();
        if (cond) stmt->children.push_back(std::move(cond));
        tok_->match(')');
    }

    auto body = parse_statement();
    if (body) stmt->children.push_back(std::move(body));

    return stmt;
}

std::unique_ptr<JSNode> JSParser::parse_export_import() {
    std::string_view kw = tok_->read_identifier(); // export or import
    auto decl = std::make_unique<JSNode>(
        kw == "export" ? JSNodeType::EXPORT_DECL : JSNodeType::IMPORT_DECL);
    decl->value = kw;

    // Default export
    if (kw == "export" && tok_->peek_match("default")) {
        tok_->skip(7);
        skip_comments_and_whitespace();
        auto expr = parse_statement();
        if (expr) decl->children.push_back(std::move(expr));
        return decl;
    }

    // Skip rest of import/export statement
    while (!tok_->eof() && tok_->peek() != ';' && tok_->peek() != '}') {
        tok_->advance();
    }
    tok_->match(';');

    return decl;
}

// Expression parsing with precedence climbing
std::unique_ptr<JSNode> JSParser::parse_expression(int precedence) {
    skip_comments_and_whitespace();
    auto left = parse_primary();

    while (!tok_->eof()) {
        skip_comments_and_whitespace();

        char c = tok_->peek();

        // Detect operator (binary or assignment)
        std::string_view op;
        bool is_assignment = false;

        // 3-char operators
        if (tok_->peek_match("===") || tok_->peek_match("!==") || tok_->peek_match(">>=") ||
            tok_->peek_match("<<=") || tok_->peek_match(">>>") || tok_->peek_match("**")) {
            op = tok_->substr(tok_->pos(), tok_->pos() + 3);
            is_assignment = (op == ">>=" || op == "<<=");
        }
        // 2-char comparison / binary operators
        else if (tok_->peek_match("==") || tok_->peek_match("!=") || tok_->peek_match("<=") ||
                 tok_->peek_match(">=") || tok_->peek_match("<<") || tok_->peek_match(">>") ||
                 tok_->peek_match("&&") || tok_->peek_match("||") || tok_->peek_match("??")) {
            op = tok_->substr(tok_->pos(), tok_->pos() + 2);
            is_assignment = false;
        }
        // 2-char assignment operators
        else if (tok_->peek_match("+=") || tok_->peek_match("-=") || tok_->peek_match("*=") ||
                 tok_->peek_match("/=") || tok_->peek_match("%=") || tok_->peek_match("&=") ||
                 tok_->peek_match("|=") || tok_->peek_match("^=") || tok_->peek_match("&&=") ||
                 tok_->peek_match("||=") || tok_->peek_match("??=") || tok_->peek_match("**=")) {
            op = tok_->substr(tok_->pos(), tok_->pos() + 2);
            is_assignment = true;
        }
        // single-char operators
        else if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
                 c == '^' || c == '&' || c == '|' || c == '=') {
            op = tok_->substr(tok_->pos(), tok_->pos() + 1);
            is_assignment = (c == '=');
        }

        if (!op.empty()) {
            int prec = precedence_of(op);
            if (prec < 0) break;
            // Right-associative for assignment: a = b = c → a = (b = c)
            if (is_assignment ? prec < precedence : prec <= precedence) break;

            tok_->skip(op.size());
            skip_comments_and_whitespace();
            auto right = parse_expression(prec);
            auto node = std::make_unique<JSNode>(
                is_assignment ? JSNodeType::ASSIGN_EXPR : JSNodeType::BINARY_EXPR, op);
            node->children.push_back(std::move(left));
            if (right) node->children.push_back(std::move(right));
            left = std::move(node);
        } else if (c == '?' || c == '.') {
            break; // handled in primary/member
        } else {
            break;
        }
    }

    return left;
}

// Parse comma-separated arguments in parentheses.
// Precondition: '(' has already been consumed.
// Parses each argument as a full expression, tracking references.
void JSParser::parse_call_arguments(std::unique_ptr<JSNode>& call) {
    int depth = 1;
    while (!tok_->eof() && depth > 0) {
        skip_comments_and_whitespace();
        if (tok_->peek() == ')') {
            tok_->advance();
            depth--;
            if (depth == 0) break;
            continue;
        }
        if (tok_->peek() == ',' && depth == 1) {
            tok_->advance();
            continue;
        }

        // Parse one argument expression
        auto arg = parse_expression();
        if (arg) {
            call->children.push_back(std::move(arg));
        }

        // If next char is ',', we'll handle it at top of loop
        // If next char is ')', it's the last arg — loop handles it
    }
}

std::unique_ptr<JSNode> JSParser::parse_primary() {
    skip_comments_and_whitespace();
    if (tok_->eof()) return nullptr;

    char c = tok_->peek();

    // Literals
    if (is_digit(c) || (c == '.' && is_digit(tok_->peek_ahead(1)))) {
        size_t start = tok_->pos();
        std::string_view num = tok_->read_number();
        return stamp_node(std::make_unique<JSNode>(JSNodeType::LITERAL, num), start);
    }

    if (c == '"' || c == '\'') {
        size_t start = tok_->pos();
        std::string str = tok_->read_quoted(c);
        // Store as literal
        auto lit = std::make_unique<JSNode>(JSNodeType::LITERAL, tok_->substr(tok_->pos() - str.size() - 2, tok_->pos()));
        return stamp_node(std::move(lit), start);
    }

    if (c == '`') {
        size_t tl_start = tok_->pos();
        // Template literal — consume until closing `
        tok_->advance();
        while (!tok_->eof() && tok_->peek() != '`') {
            if (tok_->peek() == '\\') tok_->advance();
            if (tok_->peek() == '$' && tok_->peek_ahead(1) == '{') {
                tok_->skip(2); // ${
                int depth = 1;
                while (!tok_->eof() && depth > 0) {
                    if (tok_->peek() == '{') depth++;
                    if (tok_->peek() == '}') depth--;
                    tok_->advance();
                }
                continue;
            }
            tok_->advance();
        }
        tok_->match('`');
        return stamp_node(std::make_unique<JSNode>(JSNodeType::TEMPLATE_LITERAL), tl_start);
    }

    // Keywords: true, false, null, undefined, this, new
    if (is_ident_start(c)) {
        std::string_view word = tok_->read_identifier();

        if (word == "true" || word == "false" || word == "null" || word == "undefined" || word == "this") {
            return stamp_node(std::make_unique<JSNode>(JSNodeType::LITERAL, word), tok_->pos() - word.size());
        }

        if (word == "new") {
            size_t new_start = tok_->pos() - word.size();
            auto new_expr = std::make_unique<JSNode>(JSNodeType::NEW_EXPR);
            skip_comments_and_whitespace();
            auto callee = parse_primary();
            if (callee) new_expr->children.push_back(std::move(callee));
            // Arguments
            if (tok_->peek() == '(') {
                tok_->advance(); // (
                parse_call_arguments(new_expr);
            }
            return stamp_node(std::move(new_expr), new_start);
        }

        if (word == "function") {
            return parse_function_expression(false, tok_->pos() - word.size());
        }

        if (word == "typeof" || word == "delete" || word == "void") {
            size_t unary_start = tok_->pos() - word.size();
            auto unary = std::make_unique<JSNode>(JSNodeType::UNARY_EXPR, word);
            skip_comments_and_whitespace();
            auto operand = parse_expression(15);
            if (operand) unary->children.push_back(std::move(operand));
            return stamp_node(std::move(unary), unary_start);
        }

        // Identifier — check if it's a function call
        size_t id_start = tok_->pos() - word.size();
        auto id = std::make_unique<JSNode>(JSNodeType::IDENTIFIER, word);
        id->src_start = id_start;
        id->is_reference = true;
        if (scope_) scope_->reference_variable(word);

        // Check for member access (. or [) or call (
        skip_comments_and_whitespace();
        if (tok_->peek() == '(') {
            // This is a function call — mark the function as referenced
            if (scope_) scope_->reference_function(word);
            auto call = std::make_unique<JSNode>(JSNodeType::CALL_EXPR, word);
            call->children.push_back(std::move(id));
            tok_->advance(); // (
            parse_call_arguments(call);
            maybe_track_dom_access(call);
            return stamp_node(std::move(call), id_start);
        }

        if (tok_->peek() == '.') {
            auto member = std::make_unique<JSNode>(JSNodeType::MEMBER_EXPR);
            member->children.push_back(std::move(id));
            size_t member_start = id_start;

            while (tok_->peek() == '.') {
                tok_->advance();
                std::string_view prop = tok_->read_identifier();
                auto prop_node = std::make_unique<JSNode>(JSNodeType::IDENTIFIER, prop);
                prop_node->src_start = tok_->pos() - prop.size(); // start of prop after .
                prop_node->is_reference = true;
                member->children.push_back(std::move(prop_node));

                // Check for call
                if (tok_->peek() == '(') {
                    auto call = std::make_unique<JSNode>(JSNodeType::CALL_EXPR);
                    call->children.push_back(std::move(member));
                    tok_->advance(); // (
                    parse_call_arguments(call);
                    maybe_track_dom_access(call);
                    // After call, check for further chaining (e.g., .foo afterwards)
                    skip_comments_and_whitespace();
                    if (tok_->peek() == '.') {
                        member = std::make_unique<JSNode>(JSNodeType::MEMBER_EXPR);
                        member->children.push_back(std::move(call));
                        continue;
                    }
                    return stamp_node(std::move(call), member_start);
                }
            }
            return stamp_node(std::move(member), member_start);
        }

        if (tok_->peek() == '[') {
            // Bracket notation access: a[b] — skip
            tok_->advance();
            int depth = 1;
            while (!tok_->eof() && depth > 0) {
                if (tok_->peek() == '[') depth++;
                if (tok_->peek() == ']') { depth--; if (depth == 0) { tok_->advance(); break; } }
                tok_->advance();
            }
            auto member = std::make_unique<JSNode>(JSNodeType::MEMBER_EXPR);
            member->children.push_back(std::move(id));
            return stamp_node(std::move(member), id_start);
        }

        id->src_end = tok_->pos();
        return id;
    }

    // Unary operators
    if (c == '!' || c == '~' || c == '-' || c == '+') {
        tok_->advance();
        size_t unary_start = tok_->pos() - 1; // one char consumed
        auto unary = std::make_unique<JSNode>(JSNodeType::UNARY_EXPR, std::string_view(&c, 1));
        auto operand = parse_expression(15);
        if (operand) unary->children.push_back(std::move(operand));
        return stamp_node(std::move(unary), unary_start);
    }

    // Parenthesized expression
    if (c == '(') {
        size_t paren_start = tok_->pos();
        tok_->advance();
        auto expr = parse_expression();
        tok_->match(')');

        // Arrow function?
        if (expr && expr->type == JSNodeType::IDENTIFIER && tok_->peek_match("=>")) {
            tok_->skip(2);
            skip_comments_and_whitespace();
            auto func_scope = std::make_unique<JSScope>(JSScope::Kind::FUNCTION, scope_);
            func_scope->declare_variable(expr->value);
            enter_scope(std::move(func_scope));

            std::unique_ptr<JSNode> body;
            if (tok_->peek() == '{') {
                body = parse_block();
            } else {
                auto ret_expr = parse_expression();
                auto ret = std::make_unique<JSNode>(JSNodeType::RETURN_STMT);
                ret->children.push_back(std::move(ret_expr));
                auto block = std::make_unique<JSNode>(JSNodeType::BLOCK_STMT);
                block->children.push_back(std::move(ret));
                body = std::move(block);
            }
            exit_scope();

            auto arrow = std::make_unique<JSNode>(JSNodeType::FUNC_EXPR);
            arrow->children.push_back(std::move(body));
            return stamp_node(std::move(arrow), paren_start);
        }

        return expr;
    }

    // Object literal
    if (c == '{') {
        return parse_object_literal();
    }

    // Array literal
    if (c == '[') {
        return parse_array_literal();
    }

    // Regex literal
    if (c == '/') {
        // Skip regex
        tok_->advance();
        while (!tok_->eof()) {
            if (tok_->peek() == '\\') { tok_->advance(); tok_->advance(); continue; }
            if (tok_->peek() == '/') { tok_->advance(); break; }
            tok_->advance();
        }
        // Skip flags
        while (!tok_->eof() && is_ident_char(tok_->peek())) tok_->advance();
        return std::make_unique<JSNode>(JSNodeType::LITERAL, std::string_view("/regex/"));
    }

    // Fallback
    tok_->advance();
    return std::make_unique<JSNode>(JSNodeType::EXPR_STMT);
}

std::unique_ptr<JSNode> JSParser::parse_object_literal() {
    tok_->match('{');
    auto obj = std::make_unique<JSNode>(JSNodeType::OBJECT_LITERAL);

    int depth = 1;
    while (!tok_->eof() && depth > 0) {
        skip_comments_and_whitespace();
        if (tok_->peek() == '}') { depth--; if (depth == 0) { tok_->advance(); break; } }
        if (tok_->peek() == '{') { depth++; }
        tok_->advance();
    }

    return obj;
}

std::unique_ptr<JSNode> JSParser::parse_array_literal() {
    tok_->match('[');
    auto arr = std::make_unique<JSNode>(JSNodeType::ARRAY_LITERAL);

    int depth = 1;
    while (!tok_->eof() && depth > 0) {
        if (tok_->peek() == '[') depth++;
        else if (tok_->peek() == ']') { depth--; if (depth == 0) { tok_->advance(); break; } }
        tok_->advance();
    }

    return arr;
}

void JSParser::maybe_track_dom_access(std::unique_ptr<JSNode>& node) {
    if (!node || node->children.empty()) return;

    // Check for document.getElementById("..."), element.classList.add("..."), etc.
    auto& first = node->children[0];
    if (first->type != JSNodeType::CALL_EXPR && first->type != JSNodeType::MEMBER_EXPR) return;

    // Walk member chain to find property name
    // e.g., document.getElementById -> property = "getElementById"
    if (first->children.size() >= 2) {
        auto& prop = first->children.back();
        if (prop->type == JSNodeType::IDENTIFIER) {
            // Check if any argument is a string literal containing an id/class
            // (We don't have the actual arg values easily, but we can note the access)
            DOMAccess access;
            access.property = prop->value;
            dom_accesses_.push_back(access);
        }
    }
}

void JSParser::enter_scope(std::unique_ptr<JSScope> scope) {
    JSScope* raw = scope.get();
    if (scope_) {
        scope_ = scope_->add_child(std::move(scope));
    } else {
        scope_ = raw;
    }
}

void JSParser::exit_scope() {
    if (scope_ && scope_->parent()) {
        scope_ = scope_->parent();
    }
}

std::unique_ptr<JSNode> JSParser::stamp_node(std::unique_ptr<JSNode> node, size_t start) {
    if (node) {
        node->src_start = start;
        node->src_end = tok_ ? tok_->pos() : start;
    }
    return node;
}

} // namespace tinyizer
