#include <iostream>
#include <cassert>
#include "parser/css_parser.h"

using namespace tinyizer;

#define TEST(name) std::cout << "  TEST: " << name << "... "
#define OK() std::cout << "OK\n"
#define FAIL(msg) { std::cerr << "FAIL: " << msg << "\n"; return 1; }

int main() {
    std::cout << "=== CSS Parser Tests ===\n";

    {
        TEST("simple rule");
        StringPool pool;
        CSSParser parser(pool);
        auto rules = parser.parse("body { color: red; }");

        assert(rules.size() >= 1);
        auto& rule = rules[0];
        assert(rule.selector_parts().size() > 0);
        assert(rule.declarations().size() == 1);
        assert(rule.declarations()[0].property == "color");
         assert(rule.declarations()[0].value == "red");
        OK();
    }

    {
        TEST("class selector");
        StringPool pool;
        CSSParser parser(pool);
        auto rules = parser.parse(".container { width: 100%; }");

        assert(rules.size() >= 1);
        auto& parts = rules[0].selector_parts();
        // Should have a CLASS selector
        bool found_class = false;
        for (auto& p : parts) {
            if (p.type == CSSRule::SelectorPart::Type::CLASS) {
                found_class = true;
                assert(p.value == "container");
            }
        }
        assert(found_class);
        OK();
    }

    {
        TEST("id selector");
        StringPool pool;
        CSSParser parser(pool);
        auto rules = parser.parse("#header { height: 60px; }");

        auto& parts = rules[0].selector_parts();
        bool found_id = false;
        for (auto& p : parts) {
            if (p.type == CSSRule::SelectorPart::Type::ID) {
                found_id = true;
                assert(p.value == "header");
            }
        }
        assert(found_id);
        OK();
    }

    {
        TEST("multiple declarations");
        StringPool pool;
        CSSParser parser(pool);
        auto rules = parser.parse("div { color: red; font-size: 14px; margin: 0; }");

        assert(rules[0].declarations().size() == 3);
        assert(rules[0].declarations()[0].property == "color");
        assert(rules[0].declarations()[1].property == "font-size");
        assert(rules[0].declarations()[2].property == "margin");
        OK();
    }

    {
        TEST("multiple rules");
        StringPool pool;
        CSSParser parser(pool);
        auto rules = parser.parse("div { color: red; } span { color: blue; }");

        assert(rules.size() >= 2);
        OK();
    }

    {
        TEST("media query at-rule");
        StringPool pool;
        CSSParser parser(pool);
        auto rules = parser.parse("@media (max-width: 600px) { body { font-size: 12px; } }");

        assert(rules.size() >= 1);
        assert(rules[0].is_at_rule());
        OK();
    }

    {
        TEST("!important");
        StringPool pool;
        CSSParser parser(pool);
        auto rules = parser.parse("p { color: red !important; }");

        assert(rules[0].declarations().size() == 1);
        assert(rules[0].declarations()[0].important);
        OK();
    }

    {
        TEST("pseudo selectors");
        StringPool pool;
        CSSParser parser(pool);
        auto rules = parser.parse("a:hover { color: blue; }");

        auto& parts = rules[0].selector_parts();
        bool found_pseudo = false;
        for (auto& p : parts) {
            if (p.type == CSSRule::SelectorPart::Type::PSEUDO) {
                found_pseudo = true;
            }
        }
        assert(found_pseudo);
        OK();
    }

    {
        TEST("attribute selector");
        StringPool pool;
        CSSParser parser(pool);
        auto rules = parser.parse("input[type=\"text\"] { border: 1px solid; }");

        auto& parts = rules[0].selector_parts();
        bool found_attr = false;
        for (auto& p : parts) {
            if (p.type == CSSRule::SelectorPart::Type::ATTR) {
                found_attr = true;
                assert(p.value == "type");
            }
        }
        assert(found_attr);
        OK();
    }

    std::cout << "\nAll CSS parser tests passed!\n";
    return 0;
}
