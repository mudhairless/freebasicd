/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// format_config_checks: the formatter's config loader (M17). LSP-agnostic:
// parseFormatTable/loadConfigForPath/applyClientOptions are plain data behind
// tomlplusplus, exercised directly here. The tests pin the resolution the
// design calls for: style first, then the config file's own keys override it,
// with FBdocs as the default style and single-parent `inherit` chains.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "format/config.h"

using namespace fbformat;
namespace fs = std::filesystem;

static int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b)                                                         \
  do {                                                                         \
    if (!((a) == (b))) {                                                       \
      std::printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);       \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

// A malformed document is reported (false) and must not touch `out`.
static void TestMalformedIsReported() {
  FormatConfig c;
  c.keywordCase = CaseStyle::lower;
  std::string style;
  std::string inherit;
  CHECK(!parseFormatTable("this is not = = toml", &c, &style, &inherit));
  CHECK(c.keywordCase == CaseStyle::lower); // untouched
}

// The dotted option keys (`SpacesInParens.*`, `NumericLiteralCase.*`,
// `KeepEmptyLines.*`) resolve against the `[format]` table, and an empty
// document is valid.
static void TestParseFormatTableKeys() {
  FormatConfig c;
  std::string style;
  std::string inherit;
  CHECK(parseFormatTable("[format-style]\n"
                         "style = \"fbc\"\n"
                         "\n"
                         "[format]\n"
                         "KeywordCase = \"Title\"\n"
                         "ColumnLimit = 100\n"
                         "SpacesInParens.InParameterLists = true\n"
                         "NumericLiteralCase.Prefix = \"UPPER\"\n"
                         "KeepEmptyLines.AtStartOfFile = false\n"
                         "CallParenthesesExclude = [\"Line\", \"Circle\"]\n",
                         &c, &style, &inherit));
  CHECK_EQ(style, "fbc");
  CHECK(c.keywordCase == CaseStyle::Title);
  CHECK(c.columnLimit == 100);
  CHECK(c.spacesInParensInParameterLists);
  CHECK(c.numericLiteralCasePrefix == NumericPrefixCase::UPPER);
  CHECK(!c.keepEmptyLinesAtStartOfFile);
  CHECK(c.callParenthesesExclude.size() == 2);
  CHECK_EQ(c.callParenthesesExclude[0], "Line");

  // An empty document parses and leaves the factory defaults.
  FormatConfig e;
  CHECK(parseFormatTable("", &e, nullptr, nullptr));
  CHECK(e.keywordCase == CaseStyle::Leave);

  // Unknown keys are ignored; a wrong-typed value keeps the default.
  FormatConfig u;
  u.columnLimit = 7;
  CHECK(parseFormatTable("[format]\nNope = 1\nColumnLimit = \"x\"\n", &u,
                         nullptr, nullptr));
  CHECK(u.columnLimit == 7);
}

// The client's FormattingOptions map mechanically; they are the lowest layer
// and only apply when no config file exists (the LSP caller decides that).
static void TestApplyClientOptions() {
  FormatConfig base;
  base.keywordCase = CaseStyle::Title; // a preset value must survive

  FormatConfig spaces = applyClientOptions(base, 2, true, true, true, true);
  CHECK(spaces.keywordCase == CaseStyle::Title);
  CHECK(spaces.useTab == UseTab::Never);
  CHECK(spaces.tabWidth == 2);
  CHECK(spaces.indentWidth == 2);

  FormatConfig tabs = applyClientOptions(base, 8, false, false, false, false);
  CHECK(tabs.useTab == UseTab::ForIndentation);
  CHECK(tabs.tabWidth == 8);
  CHECK(!tabs.trimTrailingWhitespace);
  CHECK(!tabs.insertNewlineAtEOF);

  // A non-positive tabSize leaves the width alone (LSP requires >= 1, but a
  // zero must not zero the indent).
  FormatConfig zero = applyClientOptions(base, 0, true, true, true, true);
  CHECK(zero.tabWidth == base.tabWidth);
}

// No config file anywhere: the default style is FBdocs (wiki-canonical
// keyword spelling, a column limit), not the every-option-Leave factory
// values.
static void TestDefaultIsFbdocs() {
  std::string const empty = (fs::temp_directory_path() / "fbfmt-none").string();
  std::error_code ec;
  fs::remove_all(empty, ec);
  fs::create_directories(empty, ec);

  FormatConfig const c = loadConfigForPath(empty + "/a.bas");
  CHECK(c.keywordCase == CaseStyle::Canonical);
  CHECK(c.typeNameCase == CaseStyle::Canonical);
  CHECK(c.intrinsicCase == CaseStyle::Canonical);
  CHECK(c.columnLimit == 80);

  fs::remove_all(empty, ec);
}

// `.fbformat` walking up wins, and its own keys override the named style.
static void TestConfigOverridesStyle() {
  fs::path const root = fs::temp_directory_path() / "fbfmt-override";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "src", ec);
  {
    std::ofstream out(root / ".fbformat");
    out << "[format-style]\nstyle = \"FBdocs\"\n\n[format]\nKeywordCase = "
           "\"lower\"\n";
  }

  FormatConfig const c = loadConfigForPath((root / "src" / "a.bas").string());
  // The file's KeywordCase wins over FBdocs's Title...
  CHECK(c.keywordCase == CaseStyle::lower);
  // ...while FBdocs fills the keys the file did not set.
  CHECK(c.columnLimit == 80);
  CHECK_EQ(c.basedOnStyle, "FBdocs");

  fs::remove_all(root, ec);
}

// A project-local `.styles/<name>.toml` shadows the shipped preset of the same
// name (local search is first), and `inherit` is single-parent.
static void TestLocalPresetAndInherit() {
  fs::path const root = fs::temp_directory_path() / "fbfmt-preset";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / ".styles", ec);
  {
    std::ofstream out(root / ".styles" / "Base.toml");
    out << "[format]\nKeywordCase = \"UPPER\"\nColumnLimit = 40\n";
  }
  {
    std::ofstream out(root / ".styles" / "Child.toml");
    out << "[format-style]\ninherit = \"Base\"\n\n[format]\nColumnLimit = 55\n";
  }
  {
    std::ofstream out(root / ".fbformat");
    out << "[format-style]\nstyle = \"Child\"\n\n[format]\nTypeNameCase = "
           "\"lower\"\n";
  }

  FormatConfig const c = loadConfigForPath((root / "a.bas").string());
  CHECK(c.keywordCase == CaseStyle::UPPER);  // inherited from Base
  CHECK(c.typeNameCase == CaseStyle::lower); // from the config file
  CHECK(c.columnLimit == 55);                // Child overrides Base

  // A cycle terminates: nothing is applied twice and the process does not hang.
  {
    std::ofstream out(root / ".styles" / "LoopA.toml");
    out << "[format-style]\ninherit = \"LoopB\"\n\n[format]\nColumnLimit = "
           "10\n";
  }
  {
    std::ofstream out(root / ".styles" / "LoopB.toml");
    out << "[format-style]\ninherit = \"LoopA\"\n\n[format]\nColumnLimit = "
           "20\n";
  }
  {
    std::ofstream out(root / ".fbformat");
    out << "[format-style]\nstyle = \"LoopA\"\n";
  }
  FormatConfig const loop = loadConfigForPath((root / "a.bas").string());
  CHECK(loop.columnLimit == 10 ||
        loop.columnLimit == 20); // terminates, no crash

  fs::remove_all(root, ec);
}

// The shipped `fbc` preset resolves through the build/install styles dir and
// inherits FBdocs: it differs on keyword case and the paren-spacing keys, and
// keeps FBdocs's inherited column limit.
static void TestShippedFbcPreset() {
  fs::path const root = fs::temp_directory_path() / "fbfmt-shipped";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  {
    std::ofstream out(root / ".fbformat");
    out << "[format-style]\nstyle = \"fbc\"\n";
  }

  FormatConfig const c = loadConfigForPath((root / "a.bas").string());
  CHECK(c.keywordCase == CaseStyle::lower);
  CHECK(c.typeNameCase == CaseStyle::lower);
  CHECK(c.useTab == UseTab::ForContinuationAndIndentation);
  CHECK(c.spacesInParensInParameterLists);
  CHECK(c.columnLimit == 80); // inherited from FBdocs

  fs::remove_all(root, ec);
}

int main() {
  TestMalformedIsReported();
  TestParseFormatTableKeys();
  TestApplyClientOptions();
  TestDefaultIsFbdocs();
  TestConfigOverridesStyle();
  TestLocalPresetAndInherit();
  TestShippedFbcPreset();

  if (failures != 0) {
    std::printf("format_config_checks: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("format_config_checks: all passed\n");
  return 0;
}
