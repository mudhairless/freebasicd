/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fblang {

// The server-side configuration file. A `freebasicd.toml` at a directory
// marks that directory as a workspace root (it joins the version-control and
// source/include-layout markers), and carries the root's settings. Parsed with
// the vendored tomlplusplus (third_party/tomlplusplus) — M11.
inline constexpr char const *kConfigFileName = "freebasicd.toml";

// Server configuration read from `freebasicd.toml` at a workspace root.
// Few keys, fixed defaults. Unknown keys are ignored and a malformed or
// type-mismatched table keeps the defaults, so a bad config file never
// degrades a session below the defaults.
struct Settings {
  // `-i`-style include directories, relative to the config file's directory,
  // that join include resolution as step ② for files under the owning root
  // (resolved to absolute dirs by the owning WorkspaceIndex; each configured
  // dir is consulted in config order before the workspace-root search).
  std::vector<std::string> includePaths;

  bool diagnosticsOn = true;
  bool semanticTokensOn = true;
  bool inlayHintsOn = true;

  bool operator==(Settings const &other) const {
    return includePaths == other.includePaths &&
           diagnosticsOn == other.diagnosticsOn &&
           semanticTokensOn == other.semanticTokensOn &&
           inlayHintsOn == other.inlayHintsOn;
  }
};

// True when `dir/freebasicd.toml` exists as a regular file — the marker that
// `dir` is a workspace root.
bool hasConfigFile(std::filesystem::path const &dir);

// Parse a freebasicd.toml document into Settings. Unknown keys are ignored;
// a parse failure or a key of the wrong type keeps that key's default, so the
// parser can never produce a Settings the server considers invalid. When `ok`
// is non-null it is set to false only when the document is malformed TOML
// (wrong-typed keys keep defaults and leave `*ok` true) — the caller uses it
// to log an error instead of silently treating a broken config as defaults.
Settings parseSettings(std::string_view tomlText, bool *ok = nullptr);

// Settings for `dir`: parsed from `dir/freebasicd.toml` when it exists,
// else the defaults. `ok` is forwarded to parseSettings (false only for
// malformed TOML; a missing file or an empty/comment-only one is fine).
Settings settingsForDir(std::filesystem::path const &dir, bool *ok = nullptr);

} // namespace fblang