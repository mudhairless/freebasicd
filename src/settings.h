/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fblang {

// The server-side configuration file. A `freebasicd.toml` at a directory
// marks that directory as a workspace root (it joins the version-control and
// source/include-layout markers), and carries the root's settings. Parsed with
// the vendored tomlplusplus (third_party/tomlplusplus) — M11.
inline constexpr char const *kConfigFileName = "freebasicd.toml";

// What a project builds (M21 step 2). An LSP never sees the link command, so
// the artifact kind is a fact only the user can supply. It defaults to an
// executable, which keeps a single-file project — and every project without a
// config file — behaving as before.
enum class BuildKind { Exe, Dll, StaticLib };

struct BuildSettings {
  BuildKind kind = BuildKind::Exe;

  // The module that is `main`, for an executable only. Absolute when the
  // config was loaded through settingsForDir, which resolves a relative value
  // against the config file's own directory; parseSettings alone keeps the
  // text as written.
  std::optional<std::string> main;

  bool operator==(BuildSettings const &other) const {
    return kind == other.kind && main == other.main;
  }
};

// Which compiler a root diagnoses against (M21 step 2). The version is *not*
// configured: it is probed from the resolved executable at adoption. `path`
// is the executable itself, absolute when loaded through settingsForDir; unset
// means the first `fbc` found on PATH.
struct FbcSettings {
  std::optional<std::string> path;

  bool operator==(FbcSettings const &other) const { return path == other.path; }
};

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

  // M21 step 2: the two facts an LSP cannot derive, plus the compiler pin.
  BuildSettings build;
  FbcSettings fbc;

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
  //
  // An entry copied from a diagnostic's *wire* code (`key, fbc error: N`) is
  // reduced to the routing key at load time, so the copy/paste a client's
  // diagnostics invite keeps working. The config file is never rewritten; the
  // load path reports the entry so the session can warn the user instead (see
  // SuppressNote).
  std::vector<std::string> suppress;

  bool operator==(Settings const &other) const {
    return includePaths == other.includePaths && build == other.build &&
           fbc == other.fbc && diagnosticsOn == other.diagnosticsOn &&
           semanticTokensOn == other.semanticTokensOn &&
           inlayHintsOn == other.inlayHintsOn &&
           codeLensOn == other.codeLensOn &&
           warningLevel == other.warningLevel && suppress == other.suppress;
  }
};

// Why a settings note was produced. `SuppressWireCode` is the existing
// `suppress` cleanup; the others are the M21 value-shape and build.main
// contradictions.
enum class SettingsNoteKind {
  SuppressWireCode,
  UnknownBuildKind,
  WrongType,
  BuildMainOnNonExe,
  BuildMainNotIndexed,
};

// A configuration problem the session reports through the log channel.
//
// `SuppressWireCode`: `entry` is the entry as written and `key` the routing key
// it reduces to (see SuppressNote below). The other kinds: `key` names the
// config key (`build.kind`, `build.main`, ...) and `entry` is the offending
// value as written, when there is one.
struct SuppressNote {
  std::string entry;
  std::string key;
  SettingsNoteKind kind = SettingsNoteKind::SuppressWireCode;
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
// When `notes` is non-null it receives one note per value problem: a `suppress`
// entry that carried more than a routing key, an unknown `[build].kind`, a
// wrong-typed `[build]` / `[fbc]` value, and so on. When `configDir` is
// non-empty, relative `build.main` and `fbc.path` values are resolved against
// it (the config file's own directory); otherwise they stay as written.
Settings parseSettings(std::string_view tomlText, bool *ok = nullptr,
                       std::vector<SuppressNote> *notes = nullptr,
                       std::filesystem::path const &configDir = {});

// Settings for `dir`: parsed from `dir/freebasicd.toml` when it exists,
// else the defaults. `ok` and `notes` are forwarded to parseSettings (false
// only for malformed TOML; a missing file or an empty/comment-only one is
// fine). Relative `build.main` and `fbc.path` values are resolved against
// `dir`.
Settings settingsForDir(std::filesystem::path const &dir, bool *ok = nullptr,
                        std::vector<SuppressNote> *notes = nullptr);

} // namespace fblang