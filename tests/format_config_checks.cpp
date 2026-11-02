/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// format_config_checks: the formatter's config loader (M17). LSP-agnostic:
// parseFormatTable/resolveConfig/configForStyle/applyClientOptions are plain
// data behind tomlplusplus, exercised directly here. The tests pin the
// resolution the design calls for: an explicit file or style first, then the
// named preset, then the config file's own keys, with FBdocs as the implicit
// default and single-parent `inherit` chains. A config file that cannot be
// trusted is refused (`ok == false`) rather than silently replaced by defaults.

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
// values. `foundConfigFile` is what lets the session apply client options.
static void TestDefaultIsFbdocs() {
  std::string const empty = (fs::temp_directory_path() / "fbfmt-none").string();
  std::error_code ec;
  fs::remove_all(empty, ec);
  fs::create_directories(empty, ec);

  ResolvedConfig const r = resolveConfig(empty + "/a.bas");
  CHECK(r.ok);
  CHECK(!r.foundConfigFile);
  CHECK(r.configFile.empty());
  CHECK(r.warnings.empty());
  CHECK(r.config.keywordCase == CaseStyle::Canonical);
  CHECK(r.config.typeNameCase == CaseStyle::Canonical);
  CHECK(r.config.intrinsicCase == CaseStyle::Canonical);
  CHECK(r.config.columnLimit == 80);

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

  ResolvedConfig const r = resolveConfig((root / "src" / "a.bas").string());
  CHECK(r.ok);
  CHECK(r.foundConfigFile);
  CHECK(r.configFile.find(".fbformat") != std::string::npos);
  // The file's KeywordCase wins over FBdocs's Title...
  CHECK(r.config.keywordCase == CaseStyle::lower);
  // ...while FBdocs fills the keys the file did not set.
  CHECK(r.config.columnLimit == 80);
  CHECK_EQ(r.config.basedOnStyle, "FBdocs");

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

  ResolvedConfig const r = resolveConfig((root / "a.bas").string());
  CHECK(r.ok);
  CHECK(r.config.keywordCase == CaseStyle::UPPER);  // inherited from Base
  CHECK(r.config.typeNameCase == CaseStyle::lower); // from the config file
  CHECK(r.config.columnLimit == 55);                // Child overrides Base

  // A cycle is a broken config, not a quiet stop halfway through the chain.
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
  ResolvedConfig const loop = resolveConfig((root / "a.bas").string());
  CHECK(!loop.ok);
  CHECK(loop.error.find("cycle") != std::string::npos);

  fs::remove_all(root, ec);
}

// An explicit config file replaces discovery; an explicit style replaces the
// file's named preset (the file's own [format] keys still win over it); an
// unknown style is refused.
static void TestExplicitConfigAndStyle() {
  fs::path const root = fs::temp_directory_path() / "fbfmt-explicit";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  fs::path const discovered = root / ".fbformat";
  fs::path const explicitFile = root / "alt.toml";
  {
    std::ofstream out(discovered);
    out << "[format-style]\nstyle = \"fbc\"\n";
  }
  {
    std::ofstream out(explicitFile);
    out << "[format]\nKeywordCase = \"UPPER\"\nColumnLimit = 33\n";
  }

  // Discovery alone picks fbc (lowercase keywords).
  CHECK(resolveConfig((root / "a.bas").string(), {}).config.keywordCase ==
        CaseStyle::lower);

  // --config skips discovery entirely.
  ConfigQuery q;
  q.configFile = explicitFile.string();
  ResolvedConfig const explicitCfg =
      resolveConfig((root / "a.bas").string(), q);
  CHECK(explicitCfg.ok);
  CHECK(explicitCfg.foundConfigFile);
  CHECK(explicitCfg.configFile.find("alt.toml") != std::string::npos);
  CHECK(explicitCfg.config.keywordCase == CaseStyle::UPPER);
  CHECK(explicitCfg.config.columnLimit == 33);

  // --style overrides the style the discovered file names.
  q = {};
  q.style = "FBdocs";
  ResolvedConfig const styled = resolveConfig((root / "a.bas").string(), q);
  CHECK(styled.ok);
  CHECK(styled.foundConfigFile);
  CHECK(styled.config.keywordCase == CaseStyle::Canonical);

  // A typo must not degrade silently, whichever layer names the style.
  q.style = "NoSuchStyle";
  ResolvedConfig const unknown = resolveConfig((root / "a.bas").string(), q);
  CHECK(!unknown.ok);
  CHECK(unknown.error.find("unknown style") != std::string::npos);

  {
    std::ofstream out(discovered);
    out << "[format-style]\nstyle = \"NoSuchStyle\"\n";
  }
  ResolvedConfig const unknownFile =
      resolveConfig((root / "a.bas").string(), {});
  CHECK(!unknownFile.ok);
  CHECK(unknownFile.error.find("unknown style") != std::string::npos);

  // An explicit file that does not exist is an error, not a fallback.
  ConfigQuery missing;
  missing.configFile = (root / "nope.toml").string();
  ResolvedConfig const missingCfg =
      resolveConfig((root / "a.bas").string(), missing);
  CHECK(!missingCfg.ok);
  CHECK(missingCfg.error.find("cannot read") != std::string::npos);

  fs::remove_all(root, ec);
}

// Invalid TOML in the chosen config file is refused instead of quietly
// formatting with FBdocs: the file's presence is authoritative.
static void TestMalformedConfigIsRefused() {
  fs::path const root = fs::temp_directory_path() / "fbfmt-malformed";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  {
    std::ofstream out(root / ".fbformat");
    out << "this is not = = toml\n";
  }

  ResolvedConfig const r = resolveConfig((root / "a.bas").string());
  CHECK(!r.ok);
  CHECK(r.foundConfigFile);
  CHECK(r.error.find("not valid TOML") != std::string::npos);

  fs::remove_all(root, ec);
}

// `.fbformat` shadows `freebasicd.toml` in the same directory, and the notice
// is a warning the caller logs rather than a library stderr write.
static void TestShadowWarning() {
  fs::path const root = fs::temp_directory_path() / "fbfmt-shadow";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  {
    std::ofstream out(root / ".fbformat");
    out << "[format]\nKeywordCase = \"lower\"\n";
  }
  {
    std::ofstream out(root / "freebasicd.toml");
    out << "[format]\nKeywordCase = \"UPPER\"\n";
  }

  ResolvedConfig const r = resolveConfig((root / "a.bas").string());
  CHECK(r.ok);
  CHECK(r.config.keywordCase == CaseStyle::lower);
  CHECK_EQ(r.warnings.size(), std::size_t{1});
  CHECK(r.warnings[0].find("freebasicd.toml") != std::string::npos);

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

  ResolvedConfig const r = resolveConfig((root / "a.bas").string());
  CHECK(r.ok);
  CHECK(r.config.keywordCase == CaseStyle::lower);
  CHECK(r.config.typeNameCase == CaseStyle::lower);
  CHECK(r.config.useTab == UseTab::ForContinuationAndIndentation);
  CHECK(r.config.spacesInParensInParameterLists);
  CHECK(r.config.columnLimit == 80); // inherited from FBdocs

  fs::remove_all(root, ec);
}

// configForStyle is the corpus gate's entry point: a named preset with no
// discovery, and an unknown name refused.
static void TestConfigForStyle() {
  ResolvedConfig const fbdocs = configForStyle("FBdocs");
  CHECK(fbdocs.ok);
  CHECK(!fbdocs.foundConfigFile);
  CHECK(fbdocs.config.keywordCase == CaseStyle::Canonical);
  CHECK_EQ(fbdocs.config.basedOnStyle, "FBdocs");

  ResolvedConfig const fbc = configForStyle("fbc");
  CHECK(fbc.ok);
  CHECK(fbc.config.keywordCase == CaseStyle::lower);
  CHECK(fbc.config.columnLimit == 80);

  ResolvedConfig const missing = configForStyle("NoSuchStyle");
  CHECK(!missing.ok);
  CHECK(missing.error.find("unknown style") != std::string::npos);

  ResolvedConfig const empty = configForStyle("");
  CHECK(!empty.ok);
}

int main() {
  TestMalformedIsReported();
  TestParseFormatTableKeys();
  TestApplyClientOptions();
  TestDefaultIsFbdocs();
  TestConfigOverridesStyle();
  TestLocalPresetAndInherit();
  TestExplicitConfigAndStyle();
  TestMalformedConfigIsRefused();
  TestShadowWarning();
  TestShippedFbcPreset();
  TestConfigForStyle();

  if (failures != 0) {
    std::printf("format_config_checks: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("format_config_checks: all passed\n");
  return 0;
}
