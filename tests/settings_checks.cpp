/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// settings_checks: freebasicd.toml parsing and the config-file root marker.
// LSP-agnostic: parseSettings/settingsForDir/hasConfigFile are plain data
// behind tomlplusplus, exercised directly here (M11).

#include <atomic>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "settings.h"

using namespace fblang;

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
    if ((a) != (b)) {                                                          \
      std::printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,        \
                  std::string(a).c_str(), std::string(b).c_str());             \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static void TestParseDefaults() {
  Settings const s = parseSettings(""); // empty document
  CHECK(s.includePaths.empty());
  CHECK(s.diagnosticsOn);
  CHECK(s.semanticTokensOn);
  CHECK(s.inlayHintsOn);
  CHECK(s.codeLensOn);
  // M23: no warnings until a config enables them (`-w none`), unlike fbc's
  // driver default of level 1.
  CHECK(s.warningLevel == 4);
  CHECK(s.suppress.empty());

  Settings const s2 = parseSettings("# comment only\n");
  CHECK(s2.includePaths.empty());
  CHECK(s2.diagnosticsOn);
  CHECK(s2.warningLevel == 4);
  CHECK(s2.suppress.empty());
}

static void TestParseFullSettings() {
  Settings const s = parseSettings(
      "includePaths = [\"inc\", \"lib/x\"]\n"
      "diagnosticsOn = false\n"
      "semanticTokensOn = false\n"
      "inlayHintsOn = false\n"
      "codeLensOn = false\n"
      "warningLevel = \"all\"\n"
      "suppress = [\"Unterminated-String\", \"bad-continuation\",\n"
      "            \"bad-continuation\", 7]\n");
  CHECK(s.includePaths.size() == 2);
  CHECK_EQ(s.includePaths[0], "inc");
  CHECK_EQ(s.includePaths[1], "lib/x");
  CHECK(!s.diagnosticsOn);
  CHECK(!s.semanticTokensOn);
  CHECK(!s.inlayHintsOn);
  CHECK(!s.codeLensOn);
  CHECK(s.warningLevel == 0);
  // Lowercased, deduped, sorted; the non-string element is skipped.
  CHECK(s.suppress.size() == 2);
  CHECK_EQ(s.suppress[0], "bad-continuation");
  CHECK_EQ(s.suppress[1], "unterminated-string");
}

// M23: the vocabulary is fbc's `-w` option names, English only, matched
// case-sensitively like the compiler's own option parser. Each maps to the
// level threshold fbc's driver would set (`all`/`constness`/`funcptr` → 0,
// `pedantic` and the family options → 1, `none` → 4); anything else — an
// unknown string, a wrong case, a wrong type — is `-w none`. fbc's `-w error`
// is deliberately not accepted (a mapped warning stays a warning).
static void TestWarningLevelParsing() {
  struct LevelCase {
    char const *text;
    int level;
  };
  LevelCase const cases[] = {
      {"all", 0},    {"constness", 0}, {"funcptr", 0}, {"pedantic", 1},
      {"param", 1},  {"escape", 1},    {"next", 1},    {"suffix", 1},
      {"upcast", 1}, {"none", 4},      {"error", 4},   {"ALL", 4},
      {"Suffix", 4}, {"bogus", 4},     {"", 4},        {"3", 4},
  };
  for (LevelCase const &c : cases) {
    Settings const s =
        parseSettings(std::string("warningLevel = \"") + c.text + "\"\n");
    CHECK(s.warningLevel == c.level);
  }

  // A non-string value (even a valid-looking number) keeps the default.
  Settings const s = parseSettings("warningLevel = 1\n");
  CHECK(s.warningLevel == 4);
}

// M23: `suppress` is a set of our routing keys — lowercased, sorted, deduped at
// parse time, so ordering and case are not part of the value (two configs that
// name the same set compare equal and answer the same resultId). A non-array
// value keeps the default, and non-string elements are skipped.
static void TestSuppressParsing() {
  Settings const a = parseSettings("suppress = [\"b\", \"A\", \"b\"]\n");
  Settings const b = parseSettings("suppress = [\"a\", \"b\"]\n");
  CHECK(a.suppress.size() == 2);
  CHECK_EQ(a.suppress[0], "a");
  CHECK_EQ(a.suppress[1], "b");
  CHECK(a == b);

  Settings const nonArray = parseSettings("suppress = \"bad-continuation\"\n");
  CHECK(nonArray.suppress.empty());
  Settings const mixed = parseSettings("suppress = [true, \"\", \"x\"]\n");
  CHECK(mixed.suppress.size() == 2); // "" and "x" are both strings
  CHECK_EQ(mixed.suppress[0], "");
  CHECK_EQ(mixed.suppress[1], "x");
}

// M23: a `suppress` entry copied from a diagnostic's wire code carries the fbc
// half after the routing key (`include-not-found, fbc error: 23`). A key never
// contains a comma, so the loader keeps the text before the first one and
// reports the entry through `notes` for the session to warn about; the config
// file itself is never rewritten. The suffix is not part of the value, so the
// composed form compares equal to the bare key — a suffix-only edit is not a
// settings change and moves no resultId.
static void TestSuppressWireSuffix() {
  std::vector<SuppressNote> notes;
  Settings const s = parseSettings(
      "suppress = [\"include-not-found, fbc error: 23\",\n"
      "            \"keyword-suffix, fbc warning: 44\", \"stray-closer\"]\n",
      nullptr, &notes);
  CHECK(s.suppress.size() == 3);
  CHECK_EQ(s.suppress[0], "include-not-found");
  CHECK_EQ(s.suppress[1], "keyword-suffix");
  CHECK_EQ(s.suppress[2], "stray-closer");
  CHECK(notes.size() == 2);
  CHECK_EQ(notes[0].entry, "include-not-found, fbc error: 23");
  CHECK_EQ(notes[0].key, "include-not-found");
  CHECK_EQ(notes[1].entry, "keyword-suffix, fbc warning: 44");
  CHECK_EQ(notes[1].key, "keyword-suffix");

  Settings const bare =
      parseSettings("suppress = [\"include-not-found\", \"keyword-suffix\", "
                    "\"stray-closer\"]\n");
  CHECK(s == bare);

  // Whitespace before the comma belongs to the dropped tail, not the key.
  std::vector<SuppressNote> spaced;
  Settings const w = parseSettings("suppress = [\"bad-continuation , x\"]\n",
                                   nullptr, &spaced);
  CHECK(w.suppress.size() == 1);
  CHECK_EQ(w.suppress[0], "bad-continuation");
  CHECK(spaced.size() == 1);
  CHECK_EQ(spaced[0].key, "bad-continuation");
}

static void TestUnknownKeysIgnored() {
  // Forward-compatible: keys the server does not know must be ignored, and the
  // known keys around them still parse. (Keys after a [table] header belong to
  // that table in TOML, so the known key must precede the unknown table.)
  Settings const s = parseSettings("futureKey = \"whatever\"\n"
                                   "includePaths = [\"extra\"]\n"
                                   "[nested.table]\n"
                                   "x = 1\n");
  CHECK(s.includePaths.size() == 1);
  CHECK_EQ(s.includePaths[0], "extra");
}

static void TestMalformedKeepsDefaults() {
  // A TOML parse error must fall back to the defaults, never throw out of the
  // function or produce a half-parsed state.
  Settings const s = parseSettings("this is = not toml [[[\n");
  CHECK(s.includePaths.empty());
  CHECK(s.diagnosticsOn);

  // Wrong types keep that key's default, not the string.
  Settings const s2 =
      parseSettings("diagnosticsOn = 42\nincludePaths = \"inc\"\n");
  CHECK(s2.diagnosticsOn);
  CHECK(s2.includePaths.empty());

  // Wrong element types in the array are skipped, valid ones kept.
  Settings const s3 = parseSettings("includePaths = [1, \"ok\", false]\n");
  CHECK(s3.includePaths.size() == 1);
  CHECK_EQ(s3.includePaths[0], "ok");
}

static void TestConfigFileMarkerAndLoad() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-settings-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(sandbox);
  std::filesystem::path const withConfig = sandbox / "with";
  std::filesystem::path const bare = sandbox / "bare";
  std::filesystem::create_directories(withConfig);
  std::filesystem::create_directories(bare);

  CHECK(!hasConfigFile(withConfig));
  CHECK(!hasConfigFile(bare));
  Settings const defaults = settingsForDir(bare);
  CHECK(defaults.includePaths.empty());

  {
    std::ofstream out(withConfig / kConfigFileName);
    out << "includePaths = [\"inc\"]\ndiagnosticsOn = false\n";
  }
  CHECK(hasConfigFile(withConfig));
  CHECK(!hasConfigFile(bare));
  Settings const s = settingsForDir(withConfig);
  CHECK(s.includePaths.size() == 1);
  CHECK_EQ(s.includePaths[0], "inc");
  CHECK(!s.diagnosticsOn);

  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

int main() {
  TestParseDefaults();
  TestParseFullSettings();
  TestUnknownKeysIgnored();
  TestMalformedKeepsDefaults();
  TestWarningLevelParsing();
  TestSuppressParsing();
  TestSuppressWireSuffix();
  TestConfigFileMarkerAndLoad();

  if (failures != 0) {
    std::printf("settings_checks: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("settings_checks: all passed\n");
  return 0;
}