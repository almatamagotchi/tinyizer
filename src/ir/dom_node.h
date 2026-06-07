#pragma once
#include <string_view>
#include <vector>
#include <memory>
#include <unordered_map>

namespace tinyizer {

// A DOM node in the unified tree
class DOMNode {
public:
    enum class Type : uint8_t {
        ELEMENT,
        TEXT,
        COMMENT,
        DOCTYPE,
    };

    DOMNode(Type type, std::string_view tag_or_text);

    Type type() const { return type_; }
    std::string_view tag_name() const { return tag_name_; }  // for elements
    std::string_view text() const { return text_; }           // for text nodes

    // Attributes
    struct Attr {
        std::string_view name;
        std::string_view value;
        bool has_quotes = true;  // whether original had quotes
    };
    const std::vector<Attr>& attrs() const { return attrs_; }
    void add_attr(std::string_view name, std::string_view value, bool has_quotes = true);

    // Tree navigation
    DOMNode* parent() const { return parent_; }
    const std::vector<std::unique_ptr<DOMNode>>& children() const { return children_; }
    DOMNode* add_child(std::unique_ptr<DOMNode> child);

    // Walk entire tree in document order
    template <typename Func>
    void walk(Func&& f) {
        f(*this);
        for (auto& child : children_) {
            child->walk(std::forward<Func>(f));
        }
    }

    template <typename Func>
    void walk(Func&& f) const {
        f(*this);
        for (auto& child : children_) {
            child->walk(std::forward<Func>(f));
        }
    }

private:
    Type type_;
    std::string_view tag_name_;
    std::string_view text_;
    std::vector<Attr> attrs_;
    std::vector<std::unique_ptr<DOMNode>> children_;
    DOMNode* parent_ = nullptr;
};

} // namespace tinyizer
