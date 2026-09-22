#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fblang {

// The server-side configuration file. A `freebasiclsp.toml` at a directory
// marks that directory as a workspace root (it joins the version-control and
// source/include-layout markers), and carries the root's settings. Parsed with
// the vendored tomlplusplus (third_party/tomlplusplus) — M11.
inline constexpr char const *kConfigFileName = "freebasiclsp.toml";

// Server configuration read from `freebasiclsp.toml` at a workspace root.
// Few keys, fixed defaults. Unknown keys are ignored and a malformed or
// type-mismatched table keeps the defaults, so a bad config file never
// degrades a session below the defaults.
struct Settings {
  // `-i`-style include directories, resolved relative to the config file's
  // directory, that join include resolution for files under the owning root.
  // (Parsed and carried here; the include-search seam consumes them once
  // didChangeConfiguration lands — PLAN M11.)
  std::vector<std::string> includePaths;

  bool diagnosticsOn = true;
  bool semanticTokensOn = true;
  bool inlayHintsOn = true;
};

// True when `dir/freebasiclsp.toml` exists as a regular file — the marker that
// `dir` is a workspace root.
bool hasConfigFile(std::filesystem::path const &dir);

// Parse a freebasiclsp.toml document into Settings. Unknown keys are ignored;
// a parse failure or a key of the wrong type keeps that key's default, so the
// parser can never produce a Settings the server considers invalid.
Settings parseSettings(std::string_view tomlText);

// Settings for `dir`: parsed from `dir/freebasiclsp.toml` when it exists,
// else the defaults.
Settings settingsForDir(std::filesystem::path const &dir);

} // namespace fblang