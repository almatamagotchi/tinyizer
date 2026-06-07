#include "dom_node.h"

namespace tinyizer {

DOMNode::DOMNode(Type type, std::string_view tag_or_text)
    : type_(type) {
    if (type == Type::ELEMENT) {
        tag_name_ = tag_or_text;
    } else {
        text_ = tag_or_text;
    }
}

void DOMNode::add_attr(std::string_view name, std::string_view value, bool has_quotes) {
    attrs_.push_back({name, value, has_quotes});
}

DOMNode* DOMNode::add_child(std::unique_ptr<DOMNode> child) {
    child->parent_ = this;
    DOMNode* raw = child.get();
    children_.push_back(std::move(child));
    return raw;
}

} // namespace tinyizer
