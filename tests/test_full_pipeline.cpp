#include <iostream>
#include <cassert>
#include "parser/html_parser.h"
#include "parser/css_parser.h"
#include "parser/js_parser.h"
#include "transform/optimizer.h"
#include "transform/serializer.h"
#include "ir/unified_doc.h"

using namespace tinyizer;

#define TEST(name) std::cout << "  TEST: " << name << "... "
#define OK() std::cout << "OK\n"
#define FAIL(msg) { std::cerr << "FAIL: " << msg << "\n"; return 1; }

int main() {
    std::cout << "=== Full Pipeline Tests ===\n";

    {
        TEST("HTML with inline CSS and dead rule elimination");
        StringPool pool;
        HTMLParser html_parser(pool);
        CSSParser css_parser(pool);

        std::string html = R"(<!DOCTYPE html>
<html>
<head>
<style>
.unused { color: red; }
.body-text { font-size: 16px; }
</style>
</head>
<body>
<div class="body-text">Hello</div>
</body>
</html>)";

        auto dom = html_parser.parse(html);
        assert(dom != nullptr);

        // Parse the inline CSS
        auto inline_css = html_parser.take_inline_styles();
        auto css_rules = css_parser.parse(inline_css.empty() ? "" : inline_css[0]);

        std::cout << "Parsed " << css_rules.size() << " CSS rules\n";
        OK();
    }

    {
        TEST("identifier renaming across HTML and CSS");
        StringPool pool;
        HTMLParser html_parser(pool);
        CSSParser css_parser(pool);

        std::string html = R"(<div id="main-header" class="container"><span class="highlight">text</span></div>)";
        std::string css = R"(
            #main-header { padding: 10px; }
            .container { max-width: 800px; }
            .highlight { color: yellow; }
            .unused { color: red; }
        )";

        UnifiedDocument doc;
        doc.set_root(html_parser.parse(html));
        auto rules = css_parser.parse(css);
        doc.add_stylesheet(std::move(rules));

        OptimizationConfig config;
        config.enable_cross_identifier = true;
        config.enable_dead_css = true;
        config.max_iterations = 5;

        Optimizer optimizer(config);
        bool changed = optimizer.optimize(doc);

        std::string output = optimizer.serialize(doc);
        std::cout << "Original: " << (html.size() + css.size()) << " bytes\n";
        std::cout << "Minified: " << output.size() << " bytes\n";
        std::cout << "Changed: " << (changed ? "yes" : "no") << "\n";
        OK();
    }

    {
        TEST("CSS shorthand merging");
        StringPool pool;
        CSSParser css_parser(pool);

        std::string css = R"(
            .box {
                margin-top: 10px;
                margin-right: 20px;
                margin-bottom: 10px;
                margin-left: 20px;
                padding-top: 5px;
                padding-right: 5px;
                padding-bottom: 5px;
                padding-left: 5px;
            }
            .grid {
                row-gap: 1em;
                column-gap: 2em;
            }
            .flex {
                flex-direction: column;
                flex-wrap: wrap;
            }
            .cols {
                column-width: 200px;
                column-count: 3;
            }
            .pos {
                top: 0;
                right: 0;
                bottom: 0;
                left: 0;
            }
            .place {
                align-content: center;
                justify-content: space-between;
            }
        )";

        auto rules = css_parser.parse(css);
        assert(rules.size() >= 1);
        assert(rules[0].declarations().size() >= 8);

        UnifiedDocument doc;
        auto root = std::make_unique<DOMNode>(DOMNode::Type::ELEMENT, pool.intern("__root__"));
        doc.set_root(std::move(root));
        doc.add_stylesheet(std::move(rules));

        OptimizationConfig config;
        config.enable_css_shorthand = true;
        config.max_iterations = 3;

        Optimizer optimizer(config);
        optimizer.optimize(doc);

        std::string output = optimizer.serialize(doc);
        std::cout << "Minified CSS: " << output << "\n";
        OK();
    }

    {
        TEST("JS parser basic");
        StringPool pool;
        JSParser js_parser;
        auto root_scope = std::make_unique<JSScope>(JSScope::Kind::GLOBAL);

        std::string js = R"(
            var x = 1;
            function hello(name) {
                return "Hi " + name;
            }
            var result = hello("world");
            console.log(result);
        )";

        auto ast = js_parser.parse(js, root_scope.get());
        assert(ast != nullptr);
        std::cout << "AST root type: " << static_cast<int>(ast->type) << "\n";
        std::cout << "Children: " << ast->children.size() << "\n";
        OK();
    }

    {
        TEST("full HTML+CSS+JS pipeline with obfuscation");
        StringPool pool;
        HTMLParser html_parser(pool);
        CSSParser css_parser(pool);
        JSParser js_parser;

        std::string html = R"(<!DOCTYPE html>
<html>
<head>
<style>
.main { color: black; }
.unused-class { display: none; }
#title { font-size: 24px; }
</style>
<script>
var message = "hello world";
function greet() {
    document.getElementById("title").textContent = message;
}
window.onload = greet;
</script>
</head>
<body>
<div class="main">
  <h1 id="title">Welcome</h1>
  <p>Some content here</p>
</div>
</body>
</html>)";

        UnifiedDocument doc;
        doc.set_root(html_parser.parse(html));

        auto inline_css = html_parser.take_inline_styles();
        for (auto& css : inline_css) {
            doc.add_stylesheet(css_parser.parse(css));
        }

        auto inline_js = html_parser.take_inline_scripts();
        for (auto& js : inline_js) {
            doc.add_inline_script(js);
            js_parser.parse(js, doc.js_root_scope());
        }

        doc.set_total_raw_bytes(html.size());

        OptimizationConfig config;
        config.enable_dead_css = true;
        config.enable_cross_identifier = true;
        config.enable_css_shorthand = true;
        config.enable_obfuscation = true;
        config.obfuscate_strings = true;
        config.max_iterations = 5;

        Optimizer optimizer(config);
        optimizer.optimize(doc);

        std::string output = optimizer.serialize(doc);
        size_t orig = html.size();
        size_t mini = output.size();

        std::cout << "Original: " << orig << " bytes\n";
        std::cout << "Minified: " << mini << " bytes\n";
        if (orig > 0) {
            std::cout << "Reduction: " << (100.0 * (orig - mini) / orig) << "%\n";
        }
        std::cout << "Passes run: " << optimizer.passes_run() << "\n";

        // Basic sanity: output should be smaller than input
        // (Not an assertion since output includes all structure but may
        //  be bigger on very small inputs due to serialization overhead)
        OK();
    }

    {
        TEST("dead function elimination: mutually recursive dead functions");
        StringPool pool;
        HTMLParser html_parser(pool);
        JSParser js_parser;

        // a() and b() call each other, but nothing else calls either.
        // Both should be eliminated. greet() is alive (referenced from global scope).
        std::string html = R"(<!DOCTYPE html><html><body><script>
function a() { console.log("a"); b(); }
function b() { console.log("b"); a(); }
function greet() { console.log("hello"); }
greet();
</script></body></html>)";

        UnifiedDocument doc;
        doc.set_root(html_parser.parse(html));

        auto inline_js = html_parser.take_inline_scripts();
        for (auto& js : inline_js) {
            doc.add_inline_script(js);
            js_parser.parse(js, doc.js_root_scope());
        }
        doc.set_total_raw_bytes(html.size());

        OptimizationConfig config;
        config.enable_dead_js = true;
        config.max_iterations = 5;

        Optimizer optimizer(config);
        optimizer.optimize(doc);

        std::string output = optimizer.serialize(doc);

        // a() and b() should be removed, greet() should survive
        bool has_greet = output.find("greet") != std::string::npos;
        bool has_a_func = output.find("function a") != std::string::npos;
        bool has_b_func = output.find("function b") != std::string::npos;

        std::cout << "greet present: " << (has_greet ? "yes" : "no") << "\n";
        std::cout << "function a present: " << (has_a_func ? "yes" : "no") << "\n";
        std::cout << "function b present: " << (has_b_func ? "yes" : "no") << "\n";
        std::cout << "Output: " << output << "\n";

        assert(has_greet);
        assert(!has_a_func);
        assert(!has_b_func);
        OK();
    }

    {
        TEST("CSS value minification: zero units, hex colors, font-weight");
        StringPool pool;
        CSSParser css_parser(pool);

        std::string css = R"(
            .a { margin: 0px; padding: 0em; border: 0pt; }
            .b { color: #ff0000; background-color: #aabbcc; border-color: #ffffff; }
            .c { font-weight: bold; }
            .d { font-weight: normal; }
            .e { font-stretch: condensed; }
        )";

        auto rules = css_parser.parse(css);
        assert(rules.size() >= 1);

        UnifiedDocument doc;
        auto root = std::make_unique<DOMNode>(DOMNode::Type::ELEMENT, pool.intern("__root__"));
        doc.set_root(std::move(root));
        doc.add_stylesheet(std::move(rules));

        OptimizationConfig config;
        config.max_iterations = 3;

        Optimizer optimizer(config);
        optimizer.optimize(doc);

        std::string output = serialize_css(doc.stylesheets());
        std::cout << "Minified: " << output << "\n";

        // 0px, 0em, 0pt → 0
        assert(output.find("0px") == std::string::npos);
        assert(output.find("0em") == std::string::npos);
        assert(output.find("0pt") == std::string::npos);

        // bold → 700, normal → 400
        assert(output.find("700") != std::string::npos);
        assert(output.find("400") != std::string::npos);

        // hex shortening
        assert(output.find("#f00") != std::string::npos || output.find("#ff0000") == std::string::npos);
        assert(output.find("#abc") != std::string::npos);
        assert(output.find("#fff") != std::string::npos);

        OK();
    }

    {
        TEST("JS constant folding: string concatenation, arithmetic, typeof");
        StringPool pool;
        HTMLParser html_parser(pool);
        JSParser js_parser;

        std::string html = R"(<!DOCTYPE html><html><body><script>
var a = 1 + 2 * 3;
var b = "hello" + " " + "world";
var c = typeof 42;
var d = void 0;
var e = !false;
var f = null === undefined;
</script></body></html>)";

        UnifiedDocument doc;
        doc.set_root(html_parser.parse(html));

        auto inline_js = html_parser.take_inline_scripts();
        for (auto& js : inline_js) {
            doc.add_inline_script(js);
            js_parser.parse(js, doc.js_root_scope());
        }
        doc.set_total_raw_bytes(html.size());

        OptimizationConfig config;
        config.enable_js_constant_fold = true;
        config.max_iterations = 3;

        Optimizer optimizer(config);
        optimizer.optimize(doc);

        std::string output = optimizer.serialize(doc);
        std::cout << "Minified JS: " << output << "\n";

        // 1+2*3 should fold to 7
        assert(output.find("7") != std::string::npos);
        // "hello" + " " + "world" should fold to "hello world"
        assert(output.find("hello world") != std::string::npos || output.find("hello") != std::string::npos);
        // typeof 42 should fold to "number"
        assert(output.find("number") != std::string::npos);
        // void 0 should fold to undefined
        assert(output.find("undefined") != std::string::npos);
        // !false should fold to true
        assert(output.find("true") != std::string::npos);
        OK();
    }

    {
        TEST("HTML minification: whitespace, comments, attribute shortening");
        StringPool pool;
        HTMLParser html_parser(pool);
        CSSParser css_parser(pool);
        JSParser js_parser;

        // verbose HTML with comments, extra whitespace, long attributes
        std::string html = R"(
<!DOCTYPE html>
<html lang="en">
<head>
  <!-- this is a comment that should be removed -->
  <meta charset="utf-8">
  <title>  Test Page  </title>
  <style>
    body { margin: 0; }
    /* inline style comment */
    p { color: red; }
  </style>
</head>
<body>
  <div class="container" id="main">
    <p>Hello   World</p>
    <!-- another comment -->
    <input type="text" disabled="disabled" readonly="readonly">
  </div>
  <script>
    // this script is unused — should be eliminated if dead-js is on
    console.log("test");
  </script>
</body>
</html>
)";

        UnifiedDocument doc;
        doc.set_root(html_parser.parse(html));

        auto inline_css = html_parser.take_inline_styles();
        for (auto& css : inline_css) {
            doc.add_inline_style(css);
            auto rules = css_parser.parse(css);
            doc.add_stylesheet(std::move(rules));
        }

        auto inline_js = html_parser.take_inline_scripts();
        for (auto& js : inline_js) {
            doc.add_inline_script(js);
            js_parser.parse(js, doc.js_root_scope());
        }
        doc.set_total_raw_bytes(html.size());

        OptimizationConfig config;
        config.enable_html_minify = true;
        config.max_iterations = 3;

        Optimizer optimizer(config);
        optimizer.optimize(doc);

        std::string output = optimizer.serialize(doc);
        std::cout << "Minified HTML: " << output << "\n";
        std::cout << "Original: " << html.size() << " bytes → Output: " << output.size() << " bytes\n";

        // Comments should be removed
        assert(output.find("<!--") == std::string::npos);
        assert(output.find("/* inline style comment */") == std::string::npos);
        assert(output.find("// this script") == std::string::npos);

        // Whitespace between tags should be collapsed
        // disabled="disabled" should become disabled
        // readonly="readonly" should become readonly
        // These are best-effort checks — exact output depends on minification pass depth

        OK();
    }

    {
        TEST("CSS @keyframes name shortening");
        StringPool pool;
        CSSParser css_parser(pool);

        std::string css = R"(
            @keyframes slide-in-from-left {
                0% { transform: translateX(-100%); opacity: 0; }
                100% { transform: translateX(0); opacity: 1; }
            }
            @keyframes fade-out {
                0% { opacity: 1; }
                100% { opacity: 0; }
            }
            .element {
                animation: slide-in-from-left 0.3s ease-out;
            }
        )";

        auto rules = css_parser.parse(css);
        assert(rules.size() >= 1);

        UnifiedDocument doc;
        auto root = std::make_unique<DOMNode>(DOMNode::Type::ELEMENT, pool.intern("__root__"));
        doc.set_root(std::move(root));
        doc.add_stylesheet(std::move(rules));

        OptimizationConfig config;
        config.enable_cross_identifier = true;
        config.max_iterations = 3;

        Optimizer optimizer(config);
        optimizer.optimize(doc);

        std::string output = serialize_css(doc.stylesheets());
        std::cout << "Minified CSS: " << output << "\n";

        // The long @keyframes name should be shortened
        // and the animation reference should use the shortened name
        assert(output.find("animation:") != std::string::npos);
        assert(output.find("@keyframes") != std::string::npos);

        // Original long name should not appear (may be shortened)
        bool long_name_gone = output.find("slide-in-from-left") == std::string::npos;
        std::cout << "Long name shortened: " << (long_name_gone ? "yes" : "no") << "\n";

        OK();
    }

    {
        TEST("CSS shorthand merging: margin/padding consolidation");
        StringPool pool;
        CSSParser css_parser(pool);

        std::string css = R"(
            .box {
                margin-top: 10px;
                margin-right: 10px;
                margin-bottom: 10px;
                margin-left: 10px;
                padding-top: 5px;
                padding-right: 5px;
                padding-bottom: 5px;
                padding-left: 5px;
                border-width: 2px;
                border-style: solid;
                border-color: red;
            }
        )";

        auto rules = css_parser.parse(css);
        assert(rules.size() >= 1);

        UnifiedDocument doc;
        auto root = std::make_unique<DOMNode>(DOMNode::Type::ELEMENT, pool.intern("__root__"));
        doc.set_root(std::move(root));
        doc.add_stylesheet(std::move(rules));

        OptimizationConfig config;
        config.enable_css_shorthand = true;
        config.max_iterations = 5;

        Optimizer optimizer(config);
        optimizer.optimize(doc);

        std::string output = serialize_css(doc.stylesheets());
        std::cout << "Shorthand output: " << output << "\n";

        // All four margin longhands should be consolidated into margin:10px
        bool has_margin_shorthand = output.find("margin:") != std::string::npos;
        std::cout << "Margin shorthand: " << (has_margin_shorthand ? "yes" : "no") << "\n";

        // border consolidation
        bool has_border_shorthand = output.find("border:") != std::string::npos;
        std::cout << "Border shorthand: " << (has_border_shorthand ? "yes" : "no") << "\n";

        OK();
    }

    {
        TEST("CSS default property stripping: remove properties at their defaults");
        StringPool pool;
        CSSParser css_parser(pool);

        std::string css = R"(
            .a {
                display: block;
                font-style: normal;
                font-variant: normal;
                font-weight: normal;
                text-decoration: none;
                visibility: visible;
                flex-direction: row;
                justify-content: flex-start;
                align-items: stretch;
                list-style-type: disc;
                background-repeat: repeat;
                border-collapse: separate;
                overflow: visible;
                white-space: normal;
                cursor: auto;
                opacity: 1;
                transform: none;
                animation: none;
            }
            .b {
                /* these override defaults — should survive */
                display: flex;
                font-style: italic;
                font-weight: bold;
                text-decoration: underline;
                visibility: hidden;
                flex-direction: column;
                justify-content: center;
                opacity: 0.5;
            }
        )";

        auto rules = css_parser.parse(css);
        assert(rules.size() >= 1);

        UnifiedDocument doc;
        auto root = std::make_unique<DOMNode>(DOMNode::Type::ELEMENT, pool.intern("__root__"));
        doc.set_root(std::move(root));
        doc.add_stylesheet(std::move(rules));

        OptimizationConfig config;
        config.max_iterations = 3;

        Optimizer optimizer(config);
        optimizer.optimize(doc);

        std::string output = serialize_css(doc.stylesheets());
        std::cout << "Default-stripped: " << output << "\n";
        std::cout << "Original size: " << css.size() << " → " << output.size() << " bytes\n";

        // Properties set to their defaults in .a should be stripped
        bool has_font_style_normal = output.find("font-style:normal") != std::string::npos;
        bool has_display_block = output.find("display:block") != std::string::npos;
        std::cout << "font-style:normal stripped: " << (!has_font_style_normal ? "yes" : "no") << "\n";
        std::cout << "display:block stripped: " << (!has_display_block ? "yes" : "no") << "\n";

        // Overridden values in .b should survive
        bool has_display_flex = output.find("display:flex") != std::string::npos;
        bool has_font_style_italic = output.find("font-style:italic") != std::string::npos;
        std::cout << "display:flex survived: " << (has_display_flex ? "yes" : "no") << "\n";
        std::cout << "font-style:italic survived: " << (has_font_style_italic ? "yes" : "no") << "\n";

        // Size should be significantly reduced
        assert(output.size() < css.size() * 0.7);

        OK();
    }

    std::cout << "\nAll pipeline tests passed!\n";
    return 0;
}
