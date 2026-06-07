#include <iostream>
#include <cassert>
#include "parser/html_parser.h"

using namespace tinyizer;

#define TEST(name) std::cout << "  TEST: " << name << "... "
#define OK() std::cout << "OK\n"
#define FAIL(msg) { std::cerr << "FAIL: " << msg << "\n"; return 1; }

int main() {
    std::cout << "=== HTML Parser Tests ===\n";

    {
        TEST("basic tag");
        StringPool pool;
        HTMLParser parser(pool);
        auto dom = parser.parse("<div></div>");

        assert(dom != nullptr);
        assert(dom->children().size() > 0);
        assert(dom->children()[0]->type() == DOMNode::Type::ELEMENT);
        assert(dom->children()[0]->tag_name() == "div");
        OK();
    }

    {
        TEST("nested tags");
        StringPool pool;
        HTMLParser parser(pool);
        auto dom = parser.parse("<div><span>text</span></div>");

        assert(dom->children().size() > 0);
        auto& div = dom->children()[0];
        assert(div->tag_name() == "div");
        assert(div->children().size() > 0);
        assert(div->children()[0]->tag_name() == "span");
        assert(div->children()[0]->children().size() > 0);
        assert(div->children()[0]->children()[0]->type() == DOMNode::Type::TEXT);
        OK();
    }

    {
        TEST("attributes with quotes");
        StringPool pool;
        HTMLParser parser(pool);
        auto dom = parser.parse("<div class=\"main\" id=\"top\"></div>");

        auto& el = dom->children()[0];
        assert(el->attrs().size() == 2);
        assert(el->attrs()[0].name == "class");
        assert(el->attrs()[0].value == "main");
        assert(el->attrs()[1].name == "id");
        assert(el->attrs()[1].value == "top");
        OK();
    }

    {
        TEST("boolean attributes");
        StringPool pool;
        HTMLParser parser(pool);
        auto dom = parser.parse("<input disabled>");

        auto& el = dom->children()[0];
        assert(el->attrs().size() >= 1);
        bool found_disabled = false;
        for (auto& attr : el->attrs()) {
            if (attr.name == "disabled") found_disabled = true;
        }
        assert(found_disabled);
        OK();
    }

    {
        TEST("self-closing tags");
        StringPool pool;
        HTMLParser parser(pool);
        auto dom = parser.parse("<br><img src=\"x.png\"><hr>");

        assert(dom->children().size() >= 3);
        assert(dom->children()[0]->tag_name() == "br");
        assert(dom->children()[1]->tag_name() == "img");
        assert(dom->children()[2]->tag_name() == "hr");
        OK();
    }

    {
        TEST("text nodes");
        StringPool pool;
        HTMLParser parser(pool);
        auto dom = parser.parse("<p>Hello, World!</p>");

        auto& p = dom->children()[0];
        assert(p->children().size() > 0);
        auto& text = p->children()[0];
        assert(text->type() == DOMNode::Type::TEXT);
        OK();
    }

    {
        TEST("comments are captured");
        StringPool pool;
        HTMLParser parser(pool);
        auto dom = parser.parse("<!-- license comment --><div></div>");

        // Comments are now preserved in the tree
        bool has_comment = false;
        for (auto& child : dom->children()) {
            if (child->type() == DOMNode::Type::COMMENT) has_comment = true;
        }
        assert(has_comment);
        OK();
    }

    {
        TEST("script tag content");
        StringPool pool;
        HTMLParser parser(pool);
        auto dom = parser.parse("<script>var x = 1;</script>");

        auto& script = dom->children()[0];
        assert(script->tag_name() == "script");

        auto inline_scripts = parser.take_inline_scripts();
        assert(inline_scripts.size() >= 1);
        OK();
    }

    {
        TEST("style tag content");
        StringPool pool;
        HTMLParser parser(pool);
        auto dom = parser.parse("<style>body { color: red; }</style>");

        auto& style = dom->children()[0];
        assert(style->tag_name() == "style");
        OK();
    }

    std::cout << "\nAll HTML parser tests passed!\n";
    return 0;
}
