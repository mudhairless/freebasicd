#include "grammar_emitter.h"

#include "language.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

namespace fbgrammar {

namespace {

// Words per TextMate keyword alternation / vim `syn keyword` group. Keeps each
// Oniguruma pattern well under any practical length limit.
constexpr std::size_t kWordGroupSize = 150;

// Control characters (below ASCII space) are emitted as `\u00xx` escapes. The
// escape is exactly 6 chars plus the NUL terminator, which an 8-byte buffer
// always fits.
constexpr unsigned char kControlCharLimit = 0x20;
constexpr std::size_t kUnicodeEscapeBufSize = 8;

std::string jsonEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char const raw : s) {
    unsigned char const c = static_cast<unsigned char>(raw);
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < kControlCharLimit) {
        // snprintf's result is checked and used so a failure can never
        // silently emit a truncated escape; truncation also cannot occur
        // for this fixed format at this range.
        char buf[kUnicodeEscapeBufSize];
        int const written = std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buf)) {
          std::abort();
        }
        out.append(buf, static_cast<std::size_t>(written));
      } else {
        out.push_back(static_cast<char>(c));
      }
      break;
    }
  }
  return out;
}

std::string jsonString(std::string_view s) {
  return "\"" + jsonEscape(s) + "\"";
}

// Escape Oniguruma metacharacters so a literal token is matched literally.
std::string regexEscape(std::string_view s) {
  constexpr char const kMetachars[] = ".^$*+?()[]{}|\\/";
  std::string out;
  out.reserve(s.size());
  for (char const c : s) {
    if (std::strchr(kMetachars, c) != nullptr) {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

// Escape a literal for a vim very-nomagic (`\V`) pattern, where only the
// backslash and the `/` delimiter are special.
std::string vimLiteral(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char const c : s) {
    if (c == '\\' || c == '/') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

std::vector<std::string> catalogReservedWords() {
  std::vector<std::string> out;
  for (std::string_view const w : fblang::reservedWords()) {
    out.emplace_back(w);
  }
  return out;
}

std::vector<std::string> catalogOperators() {
  std::vector<std::string> out;
  for (std::string_view const op : fblang::symbolOperators()) {
    out.emplace_back(op);
  }
  // Longest first so `.` never pre-empts `...` in a leftmost-first alternation.
  std::sort(out.begin(), out.end(),
            [](std::string const &a, std::string const &b) {
              if (a.size() != b.size()) {
                return a.size() > b.size();
              }
              return a < b;
            });
  return out;
}

std::string validateJson(std::string const &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) {
    // The emitted grammar must always parse; a parse failure is a hard emitter
    // bug and is fatal. The diagnostic goes through std::cerr rather than
    // fprintf so there is no return value to disregard on a path that can
    // only end in abort() anyway.
    std::cerr << "grammar_emitter: emitted TextMate JSON does not parse: "
              << rapidjson::GetParseError_En(doc.GetParseError()) << " (offset "
              << doc.GetErrorOffset() << ")\n";
    std::abort();
  }
  return json;
}

std::string emitTmLanguage() {
  std::vector<std::string> const reserved = catalogReservedWords();
  std::vector<std::string> const ops = catalogOperators();

  std::string patterns;
  auto addPattern = [&patterns](std::string const &object) {
    if (!patterns.empty()) {
      patterns += ",\n";
    }
    patterns += "    " + object;
  };

  // 1. Comments. The block comment is a repository rule so its `begin/end`
  //    pair can nest by including itself. Line `'` and `REM` follow the
  //    lexer's "to end of line" treatment.
  addPattern(R"({ "include": "#block_comment" })");
  addPattern(R"({ "name": "comment.line.freebasic", "match": "'.*$" })");
  addPattern(
      R"({ "name": "comment.line.rem.freebasic", "match": "(?i:\\bREM\\b).*$" })");

  // 2. Strings, including the `!"..."` / `$"..."` forms; `""` is an escaped
  //    quote, handled by an inner pattern so the region does not end early.
  addPattern(R"({ "name": "string.freebasic", "begin": "(?:[!$]?)\"", )"
             R"("end": "\"", "patterns": [{ "match": "\"\"" }] })");

  // 3. Numbers. Radix and leading-dot alternatives precede the operator table
  //    so `&HFF` / `.5` are numbers, matching the lexer.
  addPattern(
      R"({ "name": "number.freebasic", "match": "(?:&[HhOoBb][0-9A-Fa-f]+|\\d+(?:\\.\\d+)?(?:[eE][+-]?\\d+)?|\\.\\d+)[%&!#]?" })");

  // 4. `#`-preprocessor / metacommand: the whole line is one token.
  addPattern(R"({ "name": "macro.freebasic", "match": "^\\s*#.*$" })");

  // 5. Keywords: every reserved word, including the built-in type names (the
  //    lexer emits both as Keyword tokens). Split deterministically.
  for (std::size_t i = 0; i < reserved.size(); i += kWordGroupSize) {
    std::size_t const end = std::min(i + kWordGroupSize, reserved.size());
    std::string pattern = "(?i:\\b(?:";
    for (std::size_t j = i; j < end; ++j) {
      if (j != i) {
        pattern.push_back('|');
      }
      pattern += regexEscape(reserved[j]);
    }
    pattern += ")\\b)";
    addPattern("{ \"name\": \"keyword.freebasic\", \"match\": " +
               jsonString(pattern) + " }");
  }

  // 6. Operators, regex-escaped, longest alternative first.
  {
    std::string pattern = "(?:";
    for (std::size_t i = 0; i < ops.size(); ++i) {
      if (i != 0) {
        pattern.push_back('|');
      }
      pattern += regexEscape(ops[i]);
    }
    pattern += ")";
    addPattern("{ \"name\": \"operator.freebasic\", \"match\": " +
               jsonString(pattern) + " }");
  }

  // 7. Identifiers with an optional type-suffix char.
  addPattern(
      R"({ "name": "variable.freebasic", "match": "[A-Za-z_][A-Za-z0-9_]*[%&!#$]?" })");

  std::string json;
  json += "{\n";
  json += "  \"name\": \"FreeBASIC\",\n";
  json += "  \"scopeName\": \"source.freebasic\",\n";
  json += "  \"fileTypes\": [\"bas\", \"bi\"],\n";
  json += "  \"patterns\": [\n";
  json += patterns;
  json += "\n  ],\n";
  json += "  \"repository\": {\n";
  json += "    \"block_comment\": {\n";
  json += "      \"name\": \"comment.block.freebasic\",\n";
  json += "      \"begin\": \"/'\",\n";
  json += "      \"end\": \"'/\",\n";
  json += "      \"patterns\": [{ \"include\": \"#block_comment\" }]\n";
  json += "    }\n";
  json += "  }\n";
  json += "}\n";
  return validateJson(json);
}

std::string emitVim() {
  std::vector<std::string> const reserved = catalogReservedWords();
  std::vector<std::string> const ops = catalogOperators();

  std::string vim;
  vim += "\" Vim syntax file for FreeBASIC.\n";
  vim += "\" Generated by tools/gen_grammar from src/language.cpp; do not edit "
         "by hand.\n";
  vim +=
      "\" Install: ~/.vim/syntax/freebasic.vim, or a Neovim runtime path. See "
      "editors/README.md.\n";
  vim += "\n";
  vim += "if exists(\"b:current_syntax\")\n";
  vim += "  finish\n";
  vim += "endif\n";
  vim += "let b:current_syntax = \"freebasic\"\n";
  vim += "\n";
  vim += "syn case ignore\n";
  vim += "\n";
  vim += "\" Comments. Vim regions nest only when the contained region is\n";
  vim += "\" listed in `contains`, so /'...'/ nesting is approximated.\n";
  vim +=
      R"(syn region  fbBlockComment start=+/\'+ end=+'/+ contains=fbBlockComment)";
  vim += "\n";
  vim += R"(syn match   fbComment /'.*$/ contains=fbBlockComment)";
  vim += "\n";
  vim += R"(syn match   fbRemComment /\<REM\>.*$/ contains=fbTodo)";
  vim += "\n";
  vim += R"(syn keyword fbTodo TODO FIXME XXX contained)";
  vim += "\n";
  vim += "\n";
  vim += "\" Strings; a doubled quote is an escaped quote.\n";
  vim += R"(syn region  fbString start=/[!$]\?"/ end=/"/ skip=/""/)";
  vim += "\n";
  vim += "\n";
  vim += "\" Numbers (radix, decimal, leading-dot) with an optional suffix.\n";
  vim += R"(syn match   fbNumber /&[HhOoBb][0-9A-Fa-f]\+[%&!#]\?/)";
  vim += "\n";
  vim +=
      R"(syn match   fbNumber /\<\d\+\(\.\d\+\)\?\([eE][+-]\?\d\+\)\?[%&!#]\?/)";
  vim += "\n";
  vim += R"(syn match   fbNumber /\.\d\+\([eE][+-]\?\d\+\)\?[%&!#]\?/)";
  vim += "\n";
  vim += "\n";
  vim += "\" Preprocessor / metacommand lines.\n";
  vim += R"(syn match   fbPreProc /^\s*#.*$/)";
  vim += "\n";
  vim += "\n";
  vim += "\" Keywords (generated from src/language.cpp).\n";
  for (std::size_t i = 0; i < reserved.size(); i += kWordGroupSize) {
    std::size_t const end = std::min(i + kWordGroupSize, reserved.size());
    vim += "syn keyword fbKeyword";
    for (std::size_t j = i; j < end; ++j) {
      vim += " " + reserved[j];
    }
    vim += "\n";
  }
  vim += "\n";
  vim +=
      "\" Operators, longest first so multi-char operators win. `\\V` makes\n";
  vim += "\" the pattern very nomagic, so only `\\` and `/` need escaping.\n";
  for (std::size_t i = 0; i < ops.size(); ++i) {
    // ops is longest-first; emit shortest-first so vim's last-defined pattern
    // wins at a shared start position.
    vim += "syn match   fbOperator /\\V" + vimLiteral(ops[ops.size() - 1 - i]) +
           "/\n";
  }
  vim += "\n";
  vim += "\" Identifiers with an optional type-suffix char.\n";
  vim += R"(syn match   fbIdentifier /[A-Za-z_][A-Za-z0-9_]*[%&!#$]\?/)";
  vim += "\n";
  vim += "\n";
  vim += "hi def link fbBlockComment Comment\n";
  vim += "hi def link fbComment      Comment\n";
  vim += "hi def link fbRemComment   Comment\n";
  vim += "hi def link fbTodo         Todo\n";
  vim += "hi def link fbString       String\n";
  vim += "hi def link fbNumber       Number\n";
  vim += "hi def link fbPreProc      PreProc\n";
  vim += "hi def link fbKeyword      Keyword\n";
  vim += "hi def link fbOperator     Operator\n";
  vim += "hi def link fbIdentifier   Identifier\n";
  return vim;
}

std::string emitReadme() {
  return R"(# FreeBASIC editor grammars

Generated from the `src/language.cpp` catalog by `tools/gen_grammar`. **Do not
edit these files by hand** — edit the catalog and regenerate:

```
cmake --build build --target grammar     # or: ./build/gen_grammar editors
```

`grammar_checks` (ctest) regenerates in memory and byte-diffs the result
against the committed files, so a stale grammar fails CI.

## Files

- `freebasic.tmLanguage.json` — TextMate grammar (`source.freebasic`).
- `freebasic.vim` — vim/Neovim syntax (`b:current_syntax = "freebasic"`).

## Install

### VS Code / TextMate-compatible editors

Copy or symlink `freebasic.tmLanguage.json` into an extension's `syntaxes/`
directory and register it for the `bas`/`bi` file types.

### vim

```
mkdir -p ~/.vim/syntax ~/.vim/ftdetect
cp editors/freebasic.vim ~/.vim/syntax/freebasic.vim
printf 'au BufRead,BufNewFile *.bas,*.bi set filetype=freebasic\n' \
  > ~/.vim/ftdetect/freebasic.vim
```

### Neovim

Use the same files under `stdpath("config")/syntax` and
`stdpath("config")/ftdetect`, or drop them into a runtime path.
)";
}

} // namespace

std::vector<GeneratedFile> generate() {
  std::vector<GeneratedFile> files;
  files.push_back({"freebasic.tmLanguage.json", emitTmLanguage()});
  files.push_back({"freebasic.vim", emitVim()});
  files.push_back({"README.md", emitReadme()});
  return files;
}

} // namespace fbgrammar
