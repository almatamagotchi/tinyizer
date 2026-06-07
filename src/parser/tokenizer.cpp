#include "tokenizer.h"
#include <cctype>
#include <cstring>

namespace tinyizer {

Tokenizer::Tokenizer(std::string_view input) : input_(input), pos_(0) {}

char Tokenizer::peek() const {
    if (pos_ >= input_.size()) return '\0';
    return input_[pos_];
}

char Tokenizer::peek_ahead(size_t n) const {
    if (pos_ + n >= input_.size()) return '\0';
    return input_[pos_ + n];
}

char Tokenizer::advance() {
    if (pos_ >= input_.size()) return '\0';
    return input_[pos_++];
}

void Tokenizer::skip(size_t n) {
    pos_ = std::min(pos_ + n, input_.size());
}

void Tokenizer::skip_whitespace() {
    while (pos_ < input_.size() && is_whitespace(input_[pos_])) {
        pos_++;
    }
}

bool Tokenizer::match(char c) {
    if (pos_ < input_.size() && input_[pos_] == c) {
        pos_++;
        return true;
    }
    return false;
}

bool Tokenizer::match(std::string_view s) {
    if (pos_ + s.size() > input_.size()) return false;
    if (input_.substr(pos_, s.size()) == s) {
        pos_ += s.size();
        return true;
    }
    return false;
}

// Manually inlined case-insensitive compare to avoid <cctype> locale issues
static bool ic_eq(char a, char b) {
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    return a == b;
}

bool Tokenizer::match_ic(std::string_view s) {
    if (pos_ + s.size() > input_.size()) return false;
    for (size_t i = 0; i < s.size(); i++) {
        if (!ic_eq(input_[pos_ + i], s[i])) return false;
    }
    pos_ += s.size();
    return true;
}

std::string_view Tokenizer::read_until(char delim) {
    size_t start = pos_;
    while (pos_ < input_.size() && input_[pos_] != delim) {
        pos_++;
    }
    return input_.substr(start, pos_ - start);
}

std::string_view Tokenizer::read_until_any(std::string_view delims) {
    size_t start = pos_;
    while (pos_ < input_.size()) {
        for (char d : delims) {
            if (input_[pos_] == d) goto done;
        }
        pos_++;
    }
done:
    return input_.substr(start, pos_ - start);
}

std::string_view Tokenizer::read_while(bool (*pred)(char)) {
    size_t start = pos_;
    while (pos_ < input_.size() && pred(input_[pos_])) {
        pos_++;
    }
    return input_.substr(start, pos_ - start);
}

std::string Tokenizer::read_quoted(char quote_char) {
    std::string result;
    result.reserve(32);
    // Skip opening quote
    if (peek() == quote_char) advance();
    while (!eof()) {
        char c = advance();
        if (c == '\\') {
            char next = advance();
            if (next == '\0') break;
            switch (next) {
                case 'n': result += '\n'; break;
                case 't': result += '\t'; break;
                case 'r': result += '\r'; break;
                case '\\': result += '\\'; break;
                case '\'': result += '\''; break;
                case '"': result += '"'; break;
                case '0': result += '\0'; break;
                default:
                    // For hex escapes like \x41, handle simply
                    if (next == 'x' && is_hex_digit(peek()) && is_hex_digit(peek_ahead(1))) {
                        char hi = advance(), lo = advance();
                        auto hex_val = [](char h) -> int {
                            if (h >= '0' && h <= '9') return h - '0';
                            if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                            if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                            return 0;
                        };
                        result += static_cast<char>((hex_val(hi) << 4) | hex_val(lo));
                    } else {
                        result += next;
                    }
                    break;
            }
        } else if (c == quote_char) {
            break;
        } else {
            result += c;
        }
    }
    return result;
}

std::string_view Tokenizer::read_identifier() {
    if (eof() || !is_ident_start(peek())) return {};
    size_t start = pos_;
    while (pos_ < input_.size() && is_ident_char(input_[pos_])) {
        pos_++;
    }
    return input_.substr(start, pos_ - start);
}

std::string_view Tokenizer::read_number() {
    size_t start = pos_;
    // Optional minus
    if (peek() == '-') advance();
    // Integer part
    while (pos_ < input_.size() && is_digit(input_[pos_])) pos_++;
    // Fractional part
    if (peek() == '.' && is_digit(peek_ahead(1))) {
        advance(); // dot
        while (pos_ < input_.size() && is_digit(input_[pos_])) pos_++;
    }
    // Exponent
    if (peek() == 'e' || peek() == 'E') {
        advance();
        if (peek() == '+' || peek() == '-') advance();
        while (pos_ < input_.size() && is_digit(input_[pos_])) pos_++;
    }
    return input_.substr(start, pos_ - start);
}

bool Tokenizer::peek_match(std::string_view s) const {
    if (pos_ + s.size() > input_.size()) return false;
    return input_.substr(pos_, s.size()) == s;
}

void Tokenizer::eat_line() {
    while (pos_ < input_.size() && input_[pos_] != '\n') pos_++;
    if (pos_ < input_.size()) pos_++; // skip newline
}

} // namespace tinyizer
