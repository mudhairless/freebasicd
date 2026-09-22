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

# tomlplusplus (github.com/marzer/tomlplusplus) is vendored as a git submodule
# pinned to v3.4.0 (30172438...). Header-only; an INTERFACE target whose only
# cost is the include dir. It parses the server's freebasiclsp.toml config
# (M11).
add_subdirectory(third_party/tomlplusplus)

# M2 language layer: lexer + parser + language data. LSP-agnostic, byte
# offsets; the session converts to/from LSP positions at the boundary. The
# workspace index (M4) is in-memory only.
add_library(freebasiclsp_lang STATIC
    src/analysis_cache.cpp
    src/index.cpp
    src/inlay_hints.cpp
    src/language.cpp
    src/lexer.cpp
    src/parser.cpp
    src/resolve.cpp
    src/semantic_tokens.cpp
    src/settings.cpp
)
target_include_directories(freebasiclsp_lang PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(freebasiclsp_lang PUBLIC tomlplusplus::tomlplusplus)

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

add_executable(analysis_cache_checks
    tests/analysis_cache_checks.cpp
)
target_include_directories(analysis_cache_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(analysis_cache_checks PRIVATE freebasiclsp_lang)
add_test(NAME analysis_cache_checks COMMAND analysis_cache_checks)

add_executable(language_checks
    tests/language_checks.cpp
)
target_include_directories(language_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(language_checks PRIVATE freebasiclsp_lang)
add_test(NAME language_checks COMMAND language_checks)

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

add_executable(semantic_tokens_checks
    tests/semantic_tokens_checks.cpp
)
target_include_directories(semantic_tokens_checks PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(semantic_tokens_checks PRIVATE freebasiclsp_lang)
add_test(NAME semantic_tokens_checks COMMAND semantic_tokens_checks)

add_executable(inlay_hints_checks
    tests/inlay_hints_checks.cpp
)
target_include_directories(inlay_hints_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(inlay_hints_checks PRIVATE freebasiclsp_lang)
add_test(NAME inlay_hints_checks COMMAND inlay_hints_checks)

# M9 editor grammars: a deterministic emitter shared by the writer tool and the
# freshness test. RapidJSON is compile-time only (the emitter parses its own
# output back before returning).
add_library(freebasiclsp_grammar STATIC
    tools/grammar_emitter.cpp
)
target_include_directories(freebasiclsp_grammar PUBLIC ${CMAKE_SOURCE_DIR}/tools)
target_include_directories(freebasiclsp_grammar PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/LspCpp/third_party/rapidjson/include)
target_link_libraries(freebasiclsp_grammar PRIVATE freebasiclsp_lang)

add_executable(gen_grammar
    tools/gen_grammar.cpp
)
target_link_libraries(gen_grammar PRIVATE freebasiclsp_grammar)

add_custom_target(grammar
    COMMAND gen_grammar ${CMAKE_SOURCE_DIR}/editors
    DEPENDS gen_grammar
    COMMENT "Regenerating editors/ from src/language.cpp")

add_executable(grammar_checks
    tests/grammar_checks.cpp
)
target_include_directories(grammar_checks PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/LspCpp/third_party/rapidjson/include)
target_compile_definitions(grammar_checks PRIVATE
    EDITORS_DIR="${CMAKE_SOURCE_DIR}/editors")
target_link_libraries(grammar_checks PRIVATE freebasiclsp_grammar freebasiclsp_lang)
add_test(NAME grammar_checks COMMAND grammar_checks)

add_executable(index_checks
    tests/index_checks.cpp
)
target_include_directories(index_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(index_checks PRIVATE freebasiclsp_lang)
add_test(NAME index_checks COMMAND index_checks)

# M11: freebasiclsp.toml parsing + config-file root marker (tomlplusplus).
add_executable(settings_checks
    tests/settings_checks.cpp
)
target_include_directories(settings_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(settings_checks PRIVATE freebasiclsp_lang)
add_test(NAME settings_checks COMMAND settings_checks)

add_executable(session_integration
    tests/session_integration.cpp
)
target_include_directories(session_integration PRIVATE
    ${CMAKE_SOURCE_DIR}
    third_party/LspCpp/tests
)
target_link_libraries(session_integration PRIVATE freebasiclsp_core lspcpp)
add_test(NAME session_integration COMMAND session_integration)