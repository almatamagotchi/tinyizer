cmake_minimum_required(VERSION 3.14)
project(tinyizer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- version from git ---
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE TINYIZER_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT TINYIZER_GIT_SHA)
    set(TINYIZER_GIT_SHA "unknown")
endif()

# Aggressive optimization for x86-64
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT CMAKE_BUILD_TYPE)
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=x86-64-v2 -mtune=generic -flto -fno-rtti -fno-exceptions -DNDEBUG")
endif()

# Source files
set(TINYIZER_SOURCES
  src/main.cpp
  src/parser/html_parser.cpp
  src/parser/css_parser.cpp
  src/parser/js_parser.cpp
  src/parser/tokenizer.cpp
  src/ir/unified_doc.cpp
  src/ir/dom_node.cpp
  src/ir/css_rule.cpp
  src/ir/js_scope.cpp
  src/ir/identifier.cpp
  src/transform/optimizer.cpp
  src/transform/css_serializer.cpp
  src/transform/html_minifier.cpp
  src/transform/css_minifier.cpp
  src/transform/js_minifier.cpp
  src/transform/cross_identifier.cpp
  src/transform/dead_css.cpp
  src/transform/dead_js.cpp
  src/transform/css_shorthand.cpp
  src/transform/js_constant_fold.cpp
  src/transform/brotli_reorder.cpp
  src/transform/obfuscator.cpp
  src/util/string_pool.cpp
  src/util/frequency_map.cpp
)

add_executable(tinyizer ${TINYIZER_SOURCES})
target_include_directories(tinyizer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(tinyizer PRIVATE TINYIZER_VERSION="${TINYIZER_GIT_SHA}")

# Tests
option(BUILD_TESTS "Build test suite" ON)
if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Install
install(TARGETS tinyizer DESTINATION bin)
