#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace tinyizer {

// Represents a unique identifier in the document — could be an HTML id,
// class name, CSS selector component, JS variable/function name, etc.
enum class IdentifierKind : uint8_t {
    HTML_ID,          // id="foo"
    HTML_CLASS,       // class="foo"
    HTML_ATTR,        // data-foo, aria-label, etc.
    CSS_CLASS,        // .foo
    CSS_ID,           // #foo
    CSS_ELEMENT,      // div, span, etc.
    CSS_PROPERTY,     // color, margin, etc.
    CSS_VALUE,        // red, none, etc. (keyword values)
    CSS_CUSTOM_PROP,  // --foo
    JS_VAR,           // let/var/const foo
    JS_FUNCTION,      // function foo()
    JS_PROPERTY,      // obj.foo
    JS_GLOBAL,        // window.foo, document.foo
};

struct IdentifierInfo {
    IdentifierKind kind;
    std::string_view original_name;  // pointer into string pool
    std::string squeezed_name;       // the optimal short name
    uint32_t frequency = 0;
    uint32_t squeezed_length = 0;
};

} // namespace tinyizer
