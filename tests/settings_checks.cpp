// settings_checks: freebasiclsp.toml parsing and the config-file root marker.
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

  Settings const s2 = parseSettings("# comment only\n");
  CHECK(s2.includePaths.empty());
  CHECK(s2.diagnosticsOn);
}

static void TestParseFullSettings() {
  Settings const s = parseSettings("includePaths = [\"inc\", \"lib/x\"]\n"
                                   "diagnosticsOn = false\n"
                                   "semanticTokensOn = false\n"
                                   "inlayHintsOn = false\n");
  CHECK(s.includePaths.size() == 2);
  CHECK_EQ(s.includePaths[0], "inc");
  CHECK_EQ(s.includePaths[1], "lib/x");
  CHECK(!s.diagnosticsOn);
  CHECK(!s.semanticTokensOn);
  CHECK(!s.inlayHintsOn);
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
  TestConfigFileMarkerAndLoad();

  if (failures != 0) {
    std::printf("settings_checks: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("settings_checks: all passed\n");
  return 0;
}