// tinyizer — extreme HTML/CSS/JS minifier
// production CLI (v1 milestone 3)
// git SHA: TINYIZER_VERSION

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

// --- exit codes ---
enum ExitCode {
    EX_OK         = 0,  // success
    EX_USAGE      = 1,  // usage error (bad flags, missing input)
    EX_INPUT      = 3,  // input error (unreadable file, empty content)
    EX_PARSE      = 4,  // parse error (invalid HTML/CSS/JS syntax)
    EX_INTERNAL   = 2   // internal error (optimization failure, serialization failure)
};

// --- version ---
#ifndef TINYIZER_VERSION
#define TINYIZER_VERSION "unknown"
#endif

// Read entire file into string, return empty on failure
static std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::string content(size, '\0');
    file.read(content.data(), size);
    return content;
}

// Write string to file, return false on failure
static bool write_file(const std::string& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file.write(content.data(), content.size());
    return true;
}

// Detect file type from extension or explicit flag
enum class FileType { HTML, CSS, JS, AUTO };
static FileType detect_type(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return FileType::AUTO;
    std::string ext = path.substr(dot);
    if (ext == ".html" || ext == ".htm" || ext == ".xhtml" || ext == ".shtml") return FileType::HTML;
    if (ext == ".css") return FileType::CSS;
    if (ext == ".js" || ext == ".mjs" || ext == ".cjs") return FileType::JS;
    return FileType::AUTO;
}

static const char* file_type_name(FileType t) {
    switch (t) { case FileType::HTML: return "html"; case FileType::CSS: return "css";
                 case FileType::JS: return "js"; default: return "auto"; }
}

// --- optimization levels ---
// 0: whitespace-only (safe, no restructuring)
// 1: conservative (dead code, minification, constant folding — no renaming)
// 2: moderate (shorthand merging, identifier renaming — default)
// 3: aggressive (obfuscation, brotli reordering — may break debugging)
static void apply_level(OptimizationConfig& config, int level) {
    switch (level) {
    case 0:
        config.enable_dead_css           = false;
        config.enable_dead_js            = false;
        config.enable_cross_identifier   = false;
        config.enable_css_shorthand      = false;
        config.enable_js_constant_fold   = false;
        config.enable_remove_unused_custom_props = false;
        config.enable_brotli_reorder     = false;
        config.enable_obfuscation        = false;
        break;
    case 1:
        config.enable_dead_css           = true;
        config.enable_dead_js            = true;
        config.enable_cross_identifier   = false;
        config.enable_css_shorthand      = false;
        config.enable_js_constant_fold   = true;
        config.enable_remove_unused_custom_props = true;
        config.enable_brotli_reorder     = false;
        config.enable_obfuscation        = false;
        break;
    case 2:
        // moderate: everything safe (default)
        break; // config defaults are already level 2
    case 3:
        config.enable_brotli_reorder     = true;
        config.enable_obfuscation        = true;
        config.obfuscate_strings         = true;
        config.obfuscate_control_flow    = true;
        break;
    }
}

static void print_usage(const char* prog) {
    std::cout <<
        "tinyizer — extreme HTML/CSS/JS minifier\n"
        "beats cminify, Lightning CSS, Terser, and Google Closure Compiler on output size\n"
        "\n"
        "Usage: " << prog << " [options] <input-file> [-o <output-file>]\n"
        "\n"
        "Input mode (choose one; auto-detected from extension if omitted):\n"
        "  --html-only      Force HTML mode (parse embedded <style>/<script>)\n"
        "  --css-only       Force CSS mode (standalone stylesheet)\n"
        "  --js-only        Force JS mode (standalone script)\n"
        "\n"
        "Optimization level:\n"
        "  --level N        Optimization aggressiveness (0-3, default: 2)\n"
        "                     0 — whitespace-only (safe, no semantic changes)\n"
        "                     1 — conservative (dead code, value folding, no renaming)\n"
        "                     2 — moderate (shorthand, identifier renaming, all safe passes)\n"
        "                     3 — aggressive (obfuscation, brotli reorder, may break debugging)\n"
        "\n"
        "Fine-grained control:\n"
                      "  --no-dead-css    Disable dead CSS elimination\n"
                      "  --no-dead-js     Disable dead JS elimination\n"
                      "  --no-rename      Disable cross-language identifier renaming\n"
                      "  --no-shorthand   Disable CSS shorthand merging\n"
                      "  --obfuscate      Enable JS obfuscation (string encoding + control flow flattening)\n"
                      "  --brotli-order   Enable experimental Brotli-aware output reordering\n"
        "\n"
        "Output:\n"
        "  -o, --output <file>  Write to file (default: stdout)\n"
        "  --stats              Print compression statistics to stderr\n"
        "  --max-passes N       Max optimization iterations (default: 10)\n"
        "\n"
        "Other:\n"
        "  -V, --version   Print version (git SHA) and exit\n"
        "  -h, --help      Show this help and exit\n"
        "\n"
        "Exit codes: 0=success, 1=usage error, 2=internal error, 3=input error, 4=parse error\n"
        << std::flush;
}

int main(int argc, char* argv[]) {
    std::string input_path;
    std::string output_path;
    OptimizationConfig config;
    bool show_stats = false;
    FileType force_type = FileType::AUTO;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        std::string_view arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return EX_OK;
        }
        if (arg == "-V" || arg == "--version") {
            std::cout << "tinyizer " << TINYIZER_VERSION << "\n";
            return EX_OK;
        }
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            output_path = argv[++i];
            continue;
        }
        if (arg == "--html-only") { force_type = FileType::HTML; continue; }
        if (arg == "--css-only")  { force_type = FileType::CSS;  continue; }
        if (arg == "--js-only")   { force_type = FileType::JS;   continue; }

        if (arg == "--level" && i + 1 < argc) {
            int level = std::atoi(argv[++i]);
            if (level < 0 || level > 3) {
                std::cerr << "tinyizer: error: --level must be 0-3, got " << level << "\n";
                return EX_USAGE;
            }
            apply_level(config, level);
            continue;
        }

        if (arg == "--no-dead-css")             { config.enable_dead_css = false; continue; }
        if (arg == "--no-dead-js")              { config.enable_dead_js = false; continue; }
        if (arg == "--no-rename")               { config.enable_cross_identifier = false; continue; }
        if (arg == "--no-shorthand")            { config.enable_css_shorthand = false; continue; }
        if (arg == "--no-remove-unused-custom-props") { config.enable_remove_unused_custom_props = false; continue; }

        if (arg == "--obfuscate") {
            config.enable_obfuscation = true;
            config.obfuscate_strings = true;
            config.obfuscate_control_flow = true;
            continue;
        }
        if (arg == "--obfuscate-strings") {
            config.enable_obfuscation = true;
            config.obfuscate_strings = true;
            continue;
        }
        if (arg == "--brotli-order") { config.enable_brotli_reorder = true; continue; }
        if (arg == "--stats")        { show_stats = true; continue; }
        if (arg == "--debug-dead-js") { config.debug_dead_js = true; continue; }

        if (arg == "--max-passes" && i + 1 < argc) {
            config.max_iterations = std::atoi(argv[++i]);
            if (config.max_iterations < 1) config.max_iterations = 1;
            continue;
        }

        // Positional: input file (accept "-" for stdin)
        if (arg == "-" || (arg.size() > 0 && arg[0] != '-')) {
            input_path = arg;
            continue;
        }

        // Unknown flags
        std::cerr << "tinyizer: error: unknown option '" << arg << "'\n"
                  << "Try '" << argv[0] << " --help' for usage.\n";
        return EX_USAGE;
    }

    // --- validate input ---
    if (input_path.empty()) {
        std::cerr << "tinyizer: error: no input file specified\n"
                  << "Try '" << argv[0] << " --help' for usage.\n";
        return EX_USAGE;
    }

    // Read input
    auto start_time = std::chrono::high_resolution_clock::now();

    std::string input;
    if (input_path == "-" || input_path == "/dev/stdin") {
        // read from stdin
        std::ostringstream oss;
        oss << std::cin.rdbuf();
        input = oss.str();
    } else {
        input = read_file(input_path);
    }
    if (input.empty()) {
        std::cerr << "tinyizer: error: cannot read input file '" << input_path << "'\n"
                  << "  reason: file is missing, empty, or permission denied\n";
        return EX_INPUT;
    }

    size_t input_size = input.size();

    // Determine type
    FileType type = (force_type != FileType::AUTO) ? force_type : detect_type(input_path);
    if (type == FileType::AUTO) {
        std::cerr << "tinyizer: error: cannot detect file type from extension '" << input_path << "'\n"
                  << "  use --html-only, --css-only, or --js-only to specify\n";
        return EX_USAGE;
    }

    // Disable DOM-dependent passes for standalone CSS/JS files
    if (type == FileType::CSS || type == FileType::JS) {
        config.enable_dead_css = false;
        config.enable_html_minify = false;
    }

    // --- parse ---
    UnifiedDocument doc;
    doc.set_total_raw_bytes(input_size);

    HTMLParser html_parser(doc.string_pool());
    CSSParser css_parser(doc.string_pool());
    JSParser js_parser;

    if (type == FileType::HTML) {
        auto dom = html_parser.parse(input);
        if (!dom) {
            std::cerr << "tinyizer: error: HTML parse failed in '" << input_path << "'\n"
                      << "  reason: document is empty or contains no parseable elements\n";
            return EX_PARSE;
        }
        doc.set_root(std::move(dom));

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
        auto root = std::make_unique<DOMNode>(DOMNode::Type::ELEMENT, doc.string_pool().intern("__root__"));
        doc.set_root(std::move(root));
    } else {
        // JS
        doc.add_inline_script(input);
        auto ast = js_parser.parse(input, doc.js_root_scope());
        doc.add_js_script_ast(std::move(ast));
        auto root = std::make_unique<DOMNode>(DOMNode::Type::ELEMENT, doc.string_pool().intern("__root__"));
        doc.set_root(std::move(root));
    }

    // --- optimize ---
    Optimizer optimizer(config);
    auto opt_start = std::chrono::high_resolution_clock::now();
    optimizer.optimize(doc);
    auto opt_end = std::chrono::high_resolution_clock::now();

    // --- serialize ---
    std::string output;

    if (type == FileType::CSS) {
        output = serialize_css(doc.stylesheets());
    } else if (type == FileType::JS) {
        for (auto& opt_js : doc.optimized_js) {
            if (!output.empty() && !opt_js.empty()) output += ';';
            output += opt_js;
        }
    } else {
        output = optimizer.serialize(doc);
    }

    if (output.empty() && input_size > 0) {
        std::cerr << "tinyizer: error: serialization produced empty output from "
                  << input_size << " bytes of " << file_type_name(type) << "\n";
        return EX_INTERNAL;
    }

    auto end_time = std::chrono::high_resolution_clock::now();

    // --- output ---
    if (output_path.empty()) {
        std::cout << output;
    } else if (!write_file(output_path, output)) {
        std::cerr << "tinyizer: error: cannot write output file '" << output_path << "'\n";
        return EX_INPUT;
    }

    // --- stats ---
    if (show_stats) {
        auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
        auto opt_ms  = std::chrono::duration_cast<std::chrono::milliseconds>(opt_end - opt_start).count();

        std::cerr << "\n--- tinyizer " << TINYIZER_VERSION << " ---\n"
                  << "  input:   " << input_path << " (" << input_size << " bytes, "
                  << file_type_name(type) << ")\n"
                  << "  output:  " << output.size() << " bytes\n"
                  << "  reduced: " << (input_size > 0
                      ? (100.0 * (input_size - output.size()) / input_size) : 0)
                  << "% (" << (input_size - output.size()) << " bytes)\n"
                  << "  passes:  " << optimizer.passes_run() << "\n"
                  << "  time:    " << total_ms << " ms (optimization: " << opt_ms << " ms)\n";
    }

    return EX_OK;
}
