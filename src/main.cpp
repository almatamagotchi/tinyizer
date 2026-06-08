#include "parser/html_parser.h"
#include "parser/css_parser.h"
#include "parser/js_parser.h"
#include "transform/optimizer.h"
#include "transform/serializer.h"
#include "ir/unified_doc.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <vector>
#include <chrono>

using namespace tinyizer;

// Read entire file into string
static std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "Error: cannot open " << path << "\n";
        return {};
    }
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::string content(size, '\0');
    file.read(content.data(), size);
    return content;
}

// Write string to file
static void write_file(const std::string& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "Error: cannot write " << path << "\n";
        return;
    }
    file.write(content.data(), content.size());
}

// Detect file type from extension
enum class FileType { HTML, CSS, JS, AUTO };
static FileType detect_type(const std::string& path) {
    std::string ext;
    auto dot = path.rfind('.');
    if (dot != std::string::npos) ext = path.substr(dot);

    if (ext == ".html" || ext == ".htm" || ext == ".xhtml" || ext == ".shtml") return FileType::HTML;
    if (ext == ".css") return FileType::CSS;
    if (ext == ".js" || ext == ".mjs" || ext == ".cjs") return FileType::JS;
    return FileType::AUTO;
}

static void print_usage(const char* prog) {
    std::cout << "tinyizer — cross-language HTML/CSS/JS minifier with novel optimization\n\n"
              << "Usage: " << prog << " [options] <input-file> [-o <output-file>]\n\n"
              << "Options:\n"
              << "  -o <file>       Output file (default: stdout)\n"
              << "  --html          Force HTML mode\n"
              << "  --css           Force CSS mode\n"
              << "  --js            Force JS mode\n"
              << "  --obfuscate     Enable obfuscation\n"
              << "  --obfuscate-strings  Encode string literals\n"
              << "  --no-dead-css   Disable dead CSS elimination\n"
              << "  --no-rename     Disable cross-language identifier renaming\n"
              << "  --no-shorthand  Disable CSS shorthand merging\n"
              << "  --brotli-order  Enable experimental Brotli-aware reordering\n"
              << "  --stats         Print compression statistics\n"
              << "  --max-passes N  Max optimization iterations (default: 10)\n"
              << "  -h, --help      Show this help\n"
              << "\n"
              << "Algorithmic passes (run iteratively to fixed point):\n"
              << "  1. Dead CSS elimination (cascade simulation on DOM tree)\n"
              << "  2. Dead JS elimination (scope-aware unreferenced removal)\n"
              << "  3. Cross-language identifier squeezing (Huffman-optimal naming)\n"
              << "  4. CSS shorthand merging (longhand → shorthand)\n"
              << "  5. JS constant folding (compile-time evaluation)\n"
              << "  6. CSS value minification (color shortening, unit removal)\n"
              << "  7. HTML whitespace/comment/attribute optimization\n"
              << "  8. Brotli/DEFLATE-aware output reordering (experimental)\n"
              << "  9. Obfuscation (string encoding, control flow flattening)\n"
              << "\n"
              << "Novel approach: By jointly analyzing HTML, CSS, and JS in a unified\n"
              << "IR, tinyizer performs cross-language optimizations no other minifier can.\n";
}

int main(int argc, char* argv[]) {
    std::string input_path;
    std::string output_path;
    OptimizationConfig config;
    bool show_stats = false;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        std::string_view arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-o" && i + 1 < argc) {
            output_path = argv[++i];
        } else if (arg == "--obfuscate") {
            config.enable_obfuscation = true;
            config.obfuscate_strings = true;
            config.obfuscate_control_flow = true;
        } else if (arg == "--obfuscate-strings") {
            config.enable_obfuscation = true;
            config.obfuscate_strings = true;
        } else if (arg == "--no-dead-css") {
            config.enable_dead_css = false;
        } else if (arg == "--no-rename") {
            config.enable_cross_identifier = false;
        } else if (arg == "--no-shorthand") {
            config.enable_css_shorthand = false;
        } else if (arg == "--brotli-order") {
            config.enable_brotli_reorder = true;
        } else if (arg == "--stats") {
            show_stats = true;
        } else if (arg == "--debug-dead-js") {
            config.debug_dead_js = true;
        } else if (arg == "--max-passes" && i + 1 < argc) {
            config.max_iterations = std::atoi(argv[++i]);
        } else if (arg[0] != '-') {
            input_path = arg;
        }
    }

    if (input_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    // Read input
    auto start_time = std::chrono::high_resolution_clock::now();

    std::string input = read_file(input_path);
    if (input.empty()) {
        std::cerr << "Error: empty or unreadable input file\n";
        return 1;
    }

    size_t input_size = input.size();
    FileType type = detect_type(input_path);

    // Disable DOM-dependent passes for standalone CSS/JS files
    if (type == FileType::CSS || type == FileType::JS) {
        config.enable_dead_css = false;
        config.enable_dead_js = false;
        config.enable_html_minify = false;
    }

    // Build unified document and run optimization pipeline
    UnifiedDocument doc;
    doc.set_total_raw_bytes(input_size);

    HTMLParser html_parser(doc.string_pool());
    CSSParser css_parser(doc.string_pool());
    JSParser js_parser;

    // Parse based on type
    if (type == FileType::HTML || type == FileType::AUTO) {
        // Parse HTML
        auto dom = html_parser.parse(input);
        doc.set_root(std::move(dom));

        // Extract inline styles and scripts
        auto inline_css = html_parser.take_inline_styles();
        for (auto& css : inline_css) {
            doc.add_inline_style(css);
            auto rules = css_parser.parse(css);
            doc.add_stylesheet(std::move(rules));
        }

        auto inline_js = html_parser.take_inline_scripts();
        for (auto& js : inline_js) {
            doc.add_inline_script(js);
            auto ast = js_parser.parse(js, doc.js_root_scope());
            doc.add_js_script_ast(std::move(ast));
        }
    } else if (type == FileType::CSS) {
        auto rules = css_parser.parse(input);
        doc.add_stylesheet(std::move(rules));
        // Create minimal DOM root for reference
        auto root = std::make_unique<DOMNode>(DOMNode::Type::ELEMENT, doc.string_pool().intern("__root__"));
        doc.set_root(std::move(root));
    } else if (type == FileType::JS) {
        doc.add_inline_script(input);
        auto ast = js_parser.parse(input, doc.js_root_scope());
        doc.add_js_script_ast(std::move(ast));
        // Create minimal DOM root for reference
        auto root = std::make_unique<DOMNode>(DOMNode::Type::ELEMENT, doc.string_pool().intern("__root__"));
        doc.set_root(std::move(root));
    }

    // Run optimization pipeline
    Optimizer optimizer(config);

    auto opt_start = std::chrono::high_resolution_clock::now();
    bool optimized = optimizer.optimize(doc);
    auto opt_end = std::chrono::high_resolution_clock::now();

    // Serialize output
    std::string output;

    if (type == FileType::CSS) {
        // Standalone CSS output — serialize all stylesheets
        output = serialize_css(doc.stylesheets());
    } else if (type == FileType::JS) {
        // Standalone JS output — combine all optimized scripts
        for (auto& js : doc.inline_scripts()) {
            std::string opt_js = js;
            // Apply JS renames
            for (auto& [old_name_sv, new_name] : doc.js_rename_map) {
                std::string old_name(old_name_sv);
                size_t pos = 0;
                while ((pos = opt_js.find(old_name, pos)) != std::string::npos) {
                    bool left_ok = (pos == 0 || !is_ident_char(opt_js[pos - 1]));
                    bool right_ok = (pos + old_name.size() >= opt_js.size() || !is_ident_char(opt_js[pos + old_name.size()]));
                    if (left_ok && right_ok) {
                        opt_js.replace(pos, old_name.size(), new_name);
                        pos += new_name.size();
                    } else {
                        pos++;
                    }
                }
            }
            // Minify
            opt_js = minify_js_text(opt_js);
            if (!output.empty() && !opt_js.empty()) output += ';';
            output += opt_js;
        }
    } else {
        // HTML output (includes inline CSS/JS re-embedding)
        output = optimizer.serialize(doc);
    }

    auto end_time = std::chrono::high_resolution_clock::now();

    // Output
    if (output_path.empty()) {
        std::cout << output;
    } else {
        write_file(output_path, output);
    }

    // Stats
    if (show_stats) {
        auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
        auto opt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(opt_end - opt_start).count();

        std::cerr << "\n--- tinyizer stats ---\n"
                  << "Input:    " << input_size << " bytes\n"
                  << "Output:   " << output.size() << " bytes\n"
                  << "Reduced:  " << (input_size > 0 ? (100.0 * (input_size - output.size()) / input_size) : 0)
                  << "% (" << (input_size - output.size()) << " bytes)\n"
                  << "Passes:   " << optimizer.passes_run() << "\n"
                  << "Time:     " << total_ms << " ms (opt: " << opt_ms << " ms)\n";
    }

    return 0;
}
