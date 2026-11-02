/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// format_corpus_checks: the formatter's corpus gate (M17 wave 2). For every
// tests/corpus/*.bas file, under each shipped preset:
//
//   1. idempotence: format(format(x)) == format(x);
//   2. significant-token equivalence: formatting may move whitespace, but it
//      must not add, drop, reorder, or re-spell a token. Strings and comments
//      compare byte for byte; an identifier compares byte for byte *unless* it
//      is a language-catalog word (keyword, builtin type, intrinsic), which a
//      case transform is allowed to respell — then it compares
//      case-insensitively.
//
// The gate exists so "the formatter round-trips the corpus" stops being an ad
// hoc claim made in a commit message and becomes a suite that fails loudly.
// A failure names the file, the preset, and the first differing token.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "format/config.h"
#include "format/engine.h"
#include "format/tokens.h"
#include "language.h"

namespace fs = std::filesystem;

#ifndef CORPUS_DIR
#define CORPUS_DIR "tests/corpus"
#endif

static int failures = 0;

// The `[ok]` lines are the proof the suite visited every file; a failure is
// printed with the file and preset so a red run needs no guessing.
static void report(std::string const &file, std::string const &style,
                   std::string const &reason) {
  std::printf("[FAIL] %-28s %-8s %s\n", file.c_str(), style.c_str(),
              reason.c_str());
  ++failures;
}

static std::string readFile(fs::path const &p) {
  std::ifstream in(p, std::ios::binary);
  return std::string{std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>()};
}

static std::string lowerOf(std::string_view s) {
  std::string out(s);
  for (char &c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

// Is `word` in one of the language catalogs the formatter's case transforms
// read (src/language.cpp)? A catalog word may be respelled; anything else is a
// user identifier and must survive byte for byte.
static bool isCatalogWord(std::string_view word) {
  std::string const lower = lowerOf(word);
  return fblang::isReservedWord(lower) || fblang::isBuiltinType(lower) ||
         fblang::intrinsicFor(lower) != nullptr;
}

// One short, printable form of a token for the failure line.
static std::string describe(fbformat::Token const &t) {
  std::string text;
  for (char const c : t.text) {
    if (c == '\n') {
      text += "\\n";
    } else if (c == '\r') {
      text += "\\r";
    } else if (c == '\t') {
      text += "\\t";
    } else {
      text += c;
    }
    if (text.size() > 60) {
      text += "...";
      break;
    }
  }
  return text;
}

// The significant tokens (everything but whitespace, newlines, and EOF).
static std::vector<fbformat::Token> significant(std::string const &text) {
  fbformat::TokenStream stream(text);
  std::vector<fbformat::Token> out;
  for (fbformat::Token const &t : stream.tokens()) {
    if (t.kind == fbformat::TokenKind::Whitespace ||
        t.kind == fbformat::TokenKind::Newline ||
        t.kind == fbformat::TokenKind::Eof) {
      continue;
    }
    out.push_back(t);
  }
  return out;
}

// Do two tokens carry the same meaning? Whitespace is already gone. A catalog
// word compares case-insensitively (the case transforms may respell it);
// everything else, including strings and comments, compares exactly.
static bool sameToken(fbformat::Token const &a, fbformat::Token const &b) {
  if (a.kind != b.kind) {
    return false;
  }
  if (a.kind == fbformat::TokenKind::String ||
      a.kind == fbformat::TokenKind::LineComment ||
      a.kind == fbformat::TokenKind::BlockComment ||
      a.kind == fbformat::TokenKind::Directive ||
      a.kind == fbformat::TokenKind::Metacommand) {
    return a.text == b.text;
  }
  if (a.text == b.text) {
    return true;
  }
  return isCatalogWord(a.text) && isCatalogWord(b.text) &&
         lowerOf(a.text) == lowerOf(b.text);
}

static void checkFile(fs::path const &file, std::string const &style,
                      fbformat::FormatConfig const &config) {
  std::string const name = file.filename().string();
  std::string const original = readFile(file);

  std::string const once =
      fbformat::formatDocument(original, file.string(), config);

  std::string const twice =
      fbformat::formatDocument(once, file.string(), config);
  if (twice != once) {
    report(name, style, "format(format(x)) != format(x) (not idempotent)");
    return;
  }

  std::vector<fbformat::Token> const before = significant(original);
  std::vector<fbformat::Token> const after = significant(once);
  size_t const common = std::min(before.size(), after.size());
  for (size_t i = 0; i < common; ++i) {
    if (!sameToken(before[i], after[i])) {
      report(name, style,
             "token " + std::to_string(i) + " changed: [" +
                 describe(before[i]) + "] -> [" + describe(after[i]) + "]");
      return;
    }
  }
  if (before.size() != after.size()) {
    report(name, style,
           "significant token count changed: " + std::to_string(before.size()) +
               " -> " + std::to_string(after.size()));
    return;
  }
  std::printf("[ok]   %-28s %-8s\n", name.c_str(), style.c_str());
}

int main(int argc, char **argv) {
  fs::path const dir = argc > 1 ? fs::path(argv[1]) : fs::path(CORPUS_DIR);

  std::vector<std::string> const styles = {"FBdocs", "fbc"};
  std::vector<fs::path> files;
  if (fs::exists(dir)) {
    for (fs::directory_iterator it(dir), end; it != end; ++it) {
      if (it->path().extension() == ".bas") {
        files.push_back(it->path());
      }
    }
  }
  std::sort(files.begin(), files.end());
  if (files.empty()) {
    std::printf("format_corpus_checks: no *.bas under %s\n", dir.c_str());
    return 1;
  }

  for (std::string const &style : styles) {
    fbformat::ResolvedConfig const resolved =
        fbformat::configForStyle(style, dir.string());
    if (!resolved.ok) {
      report("(preset)", style, resolved.error);
      continue;
    }
    for (fs::path const &file : files) {
      checkFile(file, style, resolved.config);
    }
  }

  std::printf("%zu files x %zu styles, %d failure(s)\n", files.size(),
              styles.size(), failures);
  return failures == 0 ? 0 : 1;
}
