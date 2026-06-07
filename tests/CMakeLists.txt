cmake_minimum_required(VERSION 3.14)

# Simple test runner (no external test framework needed)
add_executable(test_html_parser
  test_html_parser.cpp
  ../src/parser/html_parser.cpp
  ../src/parser/tokenizer.cpp
  ../src/ir/dom_node.cpp
  ../src/util/string_pool.cpp
)
target_include_directories(test_html_parser PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

add_executable(test_css_parser
  test_css_parser.cpp
  ../src/parser/css_parser.cpp
  ../src/parser/tokenizer.cpp
  ../src/ir/css_rule.cpp
  ../src/ir/dom_node.cpp
  ../src/util/string_pool.cpp
  ../src/util/frequency_map.cpp
)
target_include_directories(test_css_parser PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

add_executable(test_full_pipeline
  test_full_pipeline.cpp
  ../src/parser/html_parser.cpp
  ../src/parser/css_parser.cpp
  ../src/parser/js_parser.cpp
  ../src/parser/tokenizer.cpp
  ../src/ir/unified_doc.cpp
  ../src/ir/dom_node.cpp
  ../src/ir/css_rule.cpp
  ../src/ir/js_scope.cpp
  ../src/ir/identifier.cpp
  ../src/transform/optimizer.cpp
  ../src/transform/css_serializer.cpp
  ../src/transform/html_minifier.cpp
  ../src/transform/css_minifier.cpp
  ../src/transform/js_minifier.cpp
  ../src/transform/cross_identifier.cpp
  ../src/transform/dead_css.cpp
  ../src/transform/dead_js.cpp
  ../src/transform/css_shorthand.cpp
  ../src/transform/js_constant_fold.cpp
  ../src/transform/brotli_reorder.cpp
  ../src/transform/obfuscator.cpp
  ../src/util/string_pool.cpp
  ../src/util/frequency_map.cpp
)
target_include_directories(test_full_pipeline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# Simple test macros
add_definitions(-DTESTING)
