# FreeBASIC Language Server
# Copyright (C) 2026 Ebben Feagan
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)
# Semantic versioning (see AGENTS.md): the number is bumped only when a release
# ships, never in an ordinary feature or fix commit.
project(freebasicd LANGUAGES CXX VERSION 0.7.0
        DESCRIPTION "A FreeBASIC Language Server")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)

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
# cost is the include dir. It parses the server's freebasicd.toml config
# (M11).
add_subdirectory(third_party/tomlplusplus)

# Localization (M20): system GNU gettext. FindGettext locates the *tools*
# (xgettext/msgfmt/msgmerge) that extract and compile catalogs; cmake/
# FindIntl.cmake locates the *runtime* (libintl) and defines Intl::Intl —
# glibc provides gettext in libc, macOS/Windows link a separate library.
set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake ${CMAKE_MODULE_PATH})
find_package(Gettext)
find_package(Intl REQUIRED)
# CMake's bundled FindGettext locates msgmerge/msgfmt only; the pot
# extraction tool needs its own discovery here.
find_program(GETTEXT_XGETTEXT_EXECUTABLE xgettext)

# M2 language layer: lexer + parser + language data. LSP-agnostic, byte
# offsets; the session converts to/from LSP positions at the boundary. The
# workspace index (M4) is in-memory only.
add_library(freebasicd_lang STATIC
    src/analysis_cache.cpp
    src/code_actions.cpp
    src/i18n.cpp
    src/index.cpp
    src/inlay_hints.cpp
    src/language.cpp
    src/lexer.cpp
    src/parser.cpp
    src/resolve.cpp
    src/semantic_tokens.cpp
    src/settings.cpp
)
target_include_directories(freebasicd_lang PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(freebasicd_lang PUBLIC tomlplusplus::tomlplusplus Intl::Intl)
# Catalog locations the runtime probes (see src/i18n.cpp): the build tree for
# dev builds, the install prefix for installed binaries. Both are compile-time
# so initI18n() needs no arguments.
target_compile_definitions(freebasicd_lang PUBLIC
    FBLANG_LOCALEDIR_BUILD="${CMAKE_BINARY_DIR}/share/locale"
    FBLANG_LOCALEDIR_INSTALL="${CMAKE_INSTALL_PREFIX}/share/locale"
)

add_library(freebasicd_core STATIC
    src/session.cpp
    src/utf16.cpp
)
target_link_libraries(freebasicd_core PRIVATE freebasicd_lang lspcpp)

add_executable(freebasicd
    src/main.cpp
)
target_link_libraries(freebasicd PRIVATE freebasicd_core lspcpp)
# The version is a compile definition rather than a runtime lookup: the server
# has no CLI flags and reads nothing from disk at startup, so this is the only
# place the number surfaces. LspCpp's InitializeResult has no `serverInfo`
# field, so the protocol cannot carry it without a fork change.
target_compile_definitions(freebasicd PRIVATE
    FBLANG_VERSION="${PROJECT_VERSION}"
)

# M18: `cmake --install <build> --prefix <dir>` lays down a runnable prefix.
# The catalogs come from the gettext block below (share/locale), and the GPL
# requires the license to travel with the binary.
install(TARGETS freebasicd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES LICENSE.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

# --- gettext catalogs (M20) -------------------------------------------------
# Committed sources: po/freebasicd.pot is the extracted message template;
# po/<lang>.po are the hand-maintained translations (English msgids, msgstr
# left to translators). The default build compiles every po to a .mo under
# ${FBLANG_LOCALE_OUT}/<lang>/LC_MESSAGES (the tree the dev build's runtime
# probes); `cmake --build . --target po-template` re-extracts the pot from the
# sources and `--target update-po` refreshes the po files against it. None of
# the regeneration targets run by default, so a fresh checkout's committed
# files stay untouched.
#
# The initial language set is exactly the human languages whose source/include
# folder names the workspace-detection catalog (src/language.cpp) recognizes —
# English is the msgid language and needs no po.
set(FBLANG_PO_DIR ${CMAKE_SOURCE_DIR}/po)
set(FBLANG_DOMAIN freebasicd)
set(FBLANG_LOCALE_OUT ${CMAKE_BINARY_DIR}/share/locale)
set(FBLANG_LANGUAGES
    af cs da de eo es et fi fr hr hu id is it lt lv ms nl no pl pt ro sk sl
    sv sw tl tr vi)

if(GETTEXT_FOUND)
    # Extraction: `cmake --build <dir> --target po-template`. Needs xgettext in
    # addition to msgfmt/msgmerge (FindGettext supplies the latter two).
    if(GETTEXT_XGETTEXT_EXECUTABLE)
        file(GLOB FBLANG_XGETTEXT_SOURCES RELATIVE ${CMAKE_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/src/*.cpp
            ${CMAKE_SOURCE_DIR}/src/*.h
            ${CMAKE_SOURCE_DIR}/tools/*.cpp)
        # The template header carries the project identity from LICENSE.md
        # (title, copyright holder, license, maintainer contact). xgettext
        # fills the copyright/bugs fields via the flags below; the remaining
        # placeholder lines (title, FIRST AUTHOR, Last-Translator,
        # Language-Team, charset, the header's `#, fuzzy` flag) are fixed by
        # tools/fix_pot_header.cmake so regeneration reproduces the committed
        # pot verbatim.
        add_custom_target(po-template
            COMMAND ${GETTEXT_XGETTEXT_EXECUTABLE}
                --from-code=UTF-8
                --keyword=tr
                --keyword=trf:1
                --flag=trf:1:c-format
                --add-comments=TRANSLATORS:
                --package-name=${PROJECT_NAME}
                "--copyright-holder=Ebben Feagan"
                --msgid-bugs-address=ebben.feagan@gmail.com
                -o po/${FBLANG_DOMAIN}.pot
                ${FBLANG_XGETTEXT_SOURCES}
            COMMAND ${CMAKE_COMMAND} -P
                    ${CMAKE_SOURCE_DIR}/tools/fix_pot_header.cmake
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Regenerating po/freebasicd.pot from src/")
    else()
        message(STATUS "xgettext not found; the po-template target is "
                       "unavailable (committed po/freebasicd.pot stays as-is)")
    endif()

    # Compilation: every committed po -> .mo, part of the default build so a
    # dev binary finds its catalogs next to itself. msgfmt --check turns a
    # malformed translation (or a format-string/placeholder drift against the
    # pot) into a build error instead of a silent fallback.
    set(FBLANG_MO_FILES "")
    foreach(lang IN LISTS FBLANG_LANGUAGES)
        set(_po ${FBLANG_PO_DIR}/${lang}.po)
        if(NOT EXISTS ${_po})
            message(WARNING "Missing translation file ${_po}; skipping")
            continue()
        endif()
        set(_mo ${FBLANG_LOCALE_OUT}/${lang}/LC_MESSAGES/${FBLANG_DOMAIN}.mo)
        add_custom_command(
            OUTPUT ${_mo}
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    ${FBLANG_LOCALE_OUT}/${lang}/LC_MESSAGES
            COMMAND ${GETTEXT_MSGFMT_EXECUTABLE} --check -o ${_mo} ${_po}
            DEPENDS ${_po}
            COMMENT "Compiling po/${lang}.po -> ${lang} message catalog")
        list(APPEND FBLANG_MO_FILES ${_mo})
    endforeach()
    add_custom_target(translations ALL DEPENDS ${FBLANG_MO_FILES})

    # Refresh: `cmake --build <dir> --target update-po` merges the freshly
    # extracted pot into every po (new msgids appear untranslated, obsolete
    # ones are dropped).
    if(GETTEXT_XGETTEXT_EXECUTABLE)
        set(_msgmerge_cmds "")
        foreach(lang IN LISTS FBLANG_LANGUAGES)
            if(EXISTS ${FBLANG_PO_DIR}/${lang}.po)
                list(APPEND _msgmerge_cmds
                    COMMAND ${GETTEXT_MSGMERGE_EXECUTABLE} --update --backup=none
                            ${FBLANG_PO_DIR}/${lang}.po
                            ${FBLANG_PO_DIR}/${FBLANG_DOMAIN}.pot)
            endif()
        endforeach()
        add_custom_target(update-po
            ${_msgmerge_cmds}
            DEPENDS po-template
            COMMENT "Merging po/freebasicd.pot into po/*.po")
    endif()

    # GNUInstallDirs is included unconditionally near the top; the catalogs
    # land beside the binary's share/locale.
    install(DIRECTORY ${FBLANG_LOCALE_OUT}/ DESTINATION
            ${CMAKE_INSTALL_DATADIR}/locale)
else()
    message(STATUS "gettext tools not found; skipping catalog generation "
                   "(po/*.po will not compile). Install gettext to translate "
                   "log and diagnostic messages.")
endif()

enable_testing()

add_executable(lexer_checks
    tests/lexer_checks.cpp
)
target_include_directories(lexer_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lexer_checks PRIVATE freebasicd_lang)
add_test(NAME lexer_checks COMMAND lexer_checks)

add_executable(analysis_cache_checks
    tests/analysis_cache_checks.cpp
)
target_include_directories(analysis_cache_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(analysis_cache_checks PRIVATE freebasicd_lang)
add_test(NAME analysis_cache_checks COMMAND analysis_cache_checks)

add_executable(language_checks
    tests/language_checks.cpp
)
target_include_directories(language_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(language_checks PRIVATE freebasicd_lang)
add_test(NAME language_checks COMMAND language_checks)

add_executable(parser_checks
    tests/parser_checks.cpp
)
target_include_directories(parser_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(parser_checks PRIVATE freebasicd_lang)
add_test(NAME parser_checks COMMAND parser_checks)

add_executable(resolve_checks
    tests/resolve_checks.cpp
)
target_include_directories(resolve_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(resolve_checks PRIVATE freebasicd_lang)
add_test(NAME resolve_checks COMMAND resolve_checks)

add_executable(corpus_checks
    tests/corpus_checks.cpp
)
target_include_directories(corpus_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(corpus_checks PRIVATE freebasicd_lang)
target_compile_definitions(corpus_checks PRIVATE CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
add_test(NAME corpus_checks COMMAND corpus_checks)
set_tests_properties(corpus_checks PROPERTIES SKIP_RETURN_CODE 77)

add_executable(utf16_checks
    tests/utf16_checks.cpp
)
target_include_directories(utf16_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(utf16_checks PRIVATE freebasicd_core lspcpp)
add_test(NAME utf16_checks COMMAND utf16_checks)

add_executable(semantic_tokens_checks
    tests/semantic_tokens_checks.cpp
)
target_include_directories(semantic_tokens_checks PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(semantic_tokens_checks PRIVATE freebasicd_lang)
add_test(NAME semantic_tokens_checks COMMAND semantic_tokens_checks)

add_executable(inlay_hints_checks
    tests/inlay_hints_checks.cpp
)
target_include_directories(inlay_hints_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(inlay_hints_checks PRIVATE freebasicd_lang)
add_test(NAME inlay_hints_checks COMMAND inlay_hints_checks)

# M12 quick fixes: the providers, exercised as pure functions over a document
# (no index, no session) plus the registry's answer for a code with no fix.
add_executable(code_actions_checks
    tests/code_actions_checks.cpp
)
target_include_directories(code_actions_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(code_actions_checks PRIVATE freebasicd_lang)
add_test(NAME code_actions_checks COMMAND code_actions_checks)

# M9 editor grammars: a deterministic emitter shared by the writer tool and the
# freshness test. RapidJSON is compile-time only (the emitter parses its own
# output back before returning).
add_library(freebasicd_grammar STATIC
    tools/grammar_emitter.cpp
)
target_include_directories(freebasicd_grammar PUBLIC ${CMAKE_SOURCE_DIR}/tools)
target_include_directories(freebasicd_grammar PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/LspCpp/third_party/rapidjson/include)
target_link_libraries(freebasicd_grammar PRIVATE freebasicd_lang)

add_executable(gen_grammar
    tools/gen_grammar.cpp
)
target_link_libraries(gen_grammar PRIVATE freebasicd_grammar)

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
target_link_libraries(grammar_checks PRIVATE freebasicd_grammar freebasicd_lang)
add_test(NAME grammar_checks COMMAND grammar_checks)

add_executable(index_checks
    tests/index_checks.cpp
)
target_include_directories(index_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(index_checks PRIVATE freebasicd_lang)
add_test(NAME index_checks COMMAND index_checks)

# M11: freebasicd.toml parsing + config-file root marker (tomlplusplus).
add_executable(settings_checks
    tests/settings_checks.cpp
)
target_include_directories(settings_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(settings_checks PRIVATE freebasicd_lang)
add_test(NAME settings_checks COMMAND settings_checks)

add_executable(session_integration
    tests/session_integration.cpp
)
target_include_directories(session_integration PRIVATE
    ${CMAKE_SOURCE_DIR}
    third_party/LspCpp/tests
)
target_link_libraries(session_integration PRIVATE freebasicd_core lspcpp)
add_test(NAME session_integration COMMAND session_integration)

# M20 i18n checks: gettext wiring (a CMake-built de catalog round-trips
# through tr()/trf()), the "FreeBASIC is never translated" invariants, and a
# freshness gate tying every translatable literal to the committed pot. The
# runtime round-trip needs msgfmt at configure time; without it the functional
# part skips (return code 77) while the structural part still runs.
add_executable(i18n_checks
    tests/i18n_checks.cpp
)
target_include_directories(i18n_checks PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(i18n_checks PRIVATE
    SRC_DIR="${CMAKE_SOURCE_DIR}/src"
    FBLANG_POT="${CMAKE_SOURCE_DIR}/po/freebasicd.pot")
target_link_libraries(i18n_checks PRIVATE freebasicd_lang)
if(GETTEXT_FOUND)
    # A tiny dedicated catalog exercises the whole runtime path with no
    # installed locale: bind domain fblang-test, LANGUAGE=de, expect msgstr.
    set(_i18n_test_locale ${CMAKE_BINARY_DIR}/i18n-test)
    set(_i18n_test_po ${_i18n_test_locale}/fblang-test.po)
    set(_i18n_test_mo ${_i18n_test_locale}/de/LC_MESSAGES/fblang-test.mo)
    file(WRITE ${_i18n_test_po}
        "msgid \"\"\n"
        "msgstr \"\"\n"
        "\"Content-Type: text/plain; charset=UTF-8\\n\"\n"
        "\"Language: de\\n\"\n"
        "\n"
        "msgid \"hello %s\"\n"
        "msgstr \"Grüße aus dem Test: %s\"\n")
    add_custom_command(
        OUTPUT ${_i18n_test_mo}
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${_i18n_test_locale}/de/LC_MESSAGES
        COMMAND ${GETTEXT_MSGFMT_EXECUTABLE} -o ${_i18n_test_mo}
                ${_i18n_test_po}
        DEPENDS ${_i18n_test_po})
    add_custom_target(i18n-test-catalog ALL DEPENDS ${_i18n_test_mo})
    target_compile_definitions(i18n_checks PRIVATE
        FBLANG_TEST_LOCALE_DIR="${_i18n_test_locale}")
endif()
add_test(NAME i18n_checks COMMAND i18n_checks)
set_tests_properties(i18n_checks PROPERTIES SKIP_RETURN_CODE 77)