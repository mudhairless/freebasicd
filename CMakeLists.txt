cmake_minimum_required(VERSION 3.16)
project(freebasiclsp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# LspCpp is vendored as a git submodule pinned to commit 19150d12. Keep its
# build surface minimal: no websockets, examples, tests, or Boost-requiring
# targets. Standalone Asio and C++17 are the defaults.
set(LSPCPP_BUILD_WEBSOCKETS OFF CACHE BOOL "" FORCE)
set(LSPCPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LSPCPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LSPCPP_BUILD_MINIMAL_EXAMPLE OFF CACHE BOOL "" FORCE)
set(LSPCPP_BUILD_PERF_SMOKE OFF CACHE BOOL "" FORCE)
set(LSPCPP_BUILD_FUZZER OFF CACHE BOOL "" FORCE)
set(LSPCPP_USE_CPP17 ON CACHE BOOL "" FORCE)
set(LSPCPP_STANDALONE_ASIO ON CACHE BOOL "" FORCE)

add_subdirectory(third_party/LspCpp)

# M2 language layer: lexer + parser + language data. LSP-agnostic, byte
# offsets; the session converts to/from LSP positions at the boundary.
add_library(freebasiclsp_lang STATIC
    src/language.cpp
    src/lexer.cpp
    src/parser.cpp
    src/resolve.cpp
)
target_include_directories(freebasiclsp_lang PUBLIC ${CMAKE_SOURCE_DIR}/src)

add_library(freebasiclsp_core STATIC
    src/session.cpp
    src/utf16.cpp
)
target_link_libraries(freebasiclsp_core PRIVATE freebasiclsp_lang lspcpp)

add_executable(freebasiclsp
    src/main.cpp
)
target_link_libraries(freebasiclsp PRIVATE freebasiclsp_core lspcpp)

enable_testing()

add_executable(lexer_checks
    tests/lexer_checks.cpp
)
target_include_directories(lexer_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lexer_checks PRIVATE freebasiclsp_lang)
add_test(NAME lexer_checks COMMAND lexer_checks)

add_executable(parser_checks
    tests/parser_checks.cpp
)
target_include_directories(parser_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(parser_checks PRIVATE freebasiclsp_lang)
add_test(NAME parser_checks COMMAND parser_checks)

add_executable(resolve_checks
    tests/resolve_checks.cpp
)
target_include_directories(resolve_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(resolve_checks PRIVATE freebasiclsp_lang)
add_test(NAME resolve_checks COMMAND resolve_checks)

add_executable(corpus_checks
    tests/corpus_checks.cpp
)
target_include_directories(corpus_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(corpus_checks PRIVATE freebasiclsp_lang)
target_compile_definitions(corpus_checks PRIVATE CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
add_test(NAME corpus_checks COMMAND corpus_checks)
set_tests_properties(corpus_checks PROPERTIES SKIP_RETURN_CODE 77)

add_executable(utf16_checks
    tests/utf16_checks.cpp
)
target_include_directories(utf16_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(utf16_checks PRIVATE freebasiclsp_core lspcpp)
add_test(NAME utf16_checks COMMAND utf16_checks)

add_executable(session_integration
    tests/session_integration.cpp
)
target_include_directories(session_integration PRIVATE
    ${CMAKE_SOURCE_DIR}
    third_party/LspCpp/tests
)
target_link_libraries(session_integration PRIVATE freebasiclsp_core lspcpp)
add_test(NAME session_integration COMMAND session_integration)