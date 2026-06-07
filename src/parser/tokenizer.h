#pragma once
#include <string_view>
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace tinyizer {

// Shared tokenizer for HTML, CSS, and JS parsing.
// Operates on the raw input in a single scan.
class Tokenizer {
public:
    explicit Tokenizer(std::string_view input);

    // Navigation
    char peek() const;
    char peek_ahead(size_t n) const;
    char advance();  // consume and return one char
    void skip(size_t n = 1);
    bool eof() const { return pos_ >= input_.size(); }
    size_t pos() const { return pos_; }
    void set_pos(size_t p) { if (p <= input_.size()) pos_ = p; }

    // Skip whitespace (space, tab, newline, carriage return)
    void skip_whitespace();

    // Read helpers
    bool match(char c);           // consume if next char matches
    bool match(std::string_view s); // consume if next chars match
    bool match_ic(std::string_view s); // case-insensitive match

    // Read until delimiter or predicate
    std::string_view read_until(char delim);
    std::string_view read_until_any(std::string_view delims);
    std::string_view read_while(bool (*pred)(char));

    // Read a quoted string (handles escape sequences)
    // Returns the content inside quotes. Advances past closing quote.
    std::string read_quoted(char quote_char);

    // Read an identifier: [a-zA-Z_][a-zA-Z0-9_-]*
    std::string_view read_identifier();

    // Read a number (integer or float)
    std::string_view read_number();

    // Peek a string without consuming
    bool peek_match(std::string_view s) const;

    void eat_line();  // consume to end of line

    std::string_view rest() const { return input_.substr(pos_); }
    std::string_view substr(size_t start, size_t end) const {
        return input_.substr(start, end - start);
    }

    // Get the full source
    std::string_view source() const { return input_; }

private:
    std::string_view input_;
    size_t pos_ = 0;
};

// Character classification helpers
inline bool is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

inline bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
}

inline bool is_ident_char(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

inline bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

inline bool is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

} // namespace tinyizer
