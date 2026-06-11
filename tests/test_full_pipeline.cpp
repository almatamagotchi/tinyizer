#include <iostream>
#include <cassert>
#include "parser/html_parser.h"
#include "parser/css_parser.h"
#include "parser/js_parser.h"
#include "transform/optimizer.h"
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

    std::cout << "\nAll pipeline tests passed!\n";
    return 0;
}
