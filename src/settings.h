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
  bool codeLensOn = true;

  // fbc's `-w` warning level, as the threshold on the catalog's level column:
  // a mapped warning surfaces iff its level >= warningLevel (fbc's own rule,
  // `errReportWarnEx` in error.bas). The config accepts fbc's `-w` option
  // names, one per value, English only — never translated:
  // `all`/`constness`/`funcptr` → 0, `pedantic`/`param`/`escape`/`next`/
  // `suffix`/`upcast` → 1, `none` → 4. 4 is `-w none` (the highest catalog
  // level is 3) and is both the default and the invalid-value fallback: this
  // server surfaces no warnings until a config enables them, unlike fbc whose
  // driver default is level 1.
  //
  // fbc's `-w error` (warnings counted as failures) is deliberately not
  // accepted: a mapped warning is the language's own warning and stays one on
  // the wire, and an error this server can diagnose already renders as an
  // error. `warningLevel = "error"` is an unknown string → `-w none`.
  int warningLevel = 4;

  // Diagnostics dropped from the wire entirely, named by their own routing key
  // (`Diagnostic::code` — `bad-continuation`, `unterminated-block`, ... — the
  // stable identifier quick fixes register on, never fbc's number the wire may
  // show instead). Entries are ASCII-lowercased and the list is sorted and
  // deduped at parse time, so neither ordering nor case is part of the value.
  // A suppressed diagnostic is dropped at delivery for the same reason the
  // `-w` gate is: the parser still records it (a fact about the bytes), the
  // wire obeys the config.
  //
  // This is the root-wide list. A per-line source comment (clang-tidy's
  // `' ignore:` / `' ignore-next-line:`) and a config allow-list counterpart
  // (a whitelist that keeps only the named keys) are future work and will feed
  // the same delivery gate; see PLAN.md M23.
  std::vector<std::string> suppress;

  bool operator==(Settings const &other) const {
    return includePaths == other.includePaths &&
           diagnosticsOn == other.diagnosticsOn &&
           semanticTokensOn == other.semanticTokensOn &&
           inlayHintsOn == other.inlayHintsOn &&
           codeLensOn == other.codeLensOn &&
           warningLevel == other.warningLevel && suppress == other.suppress;
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