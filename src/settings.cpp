/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "settings.h"

#include <toml++/toml.h>

#include <fstream>
#include <iterator>
#include <string>

namespace fblang {

bool hasConfigFile(std::filesystem::path const &dir) {
  std::error_code ec;
  return std::filesystem::is_regular_file(dir / kConfigFileName, ec);
}

Settings parseSettings(std::string_view tomlText, bool *ok) {
  if (ok != nullptr) {
    *ok = true;
  }
  Settings s;
  toml::table root;
  try {
    // With TOML_EXCEPTIONS (the vendored default) parse_result is an alias
    // for toml::table, and a parse error throws toml::parse_error.
    root = toml::parse(tomlText);
  } catch (...) {
    if (ok != nullptr) {
      *ok = false; // malformed file: everything keeps its default
    }
    return s;
  }

  // includePaths: an array of strings, relative to the config file's dir.
  if (toml::array const *const arr = root["includePaths"].as_array()) {
    for (toml::node const &el : *arr) {
      if (auto const str = el.value<std::string>()) {
        s.includePaths.push_back(*str);
      }
    }
  }
  if (auto const v = root["diagnosticsOn"].value<bool>()) {
    s.diagnosticsOn = *v;
  }
  if (auto const v = root["semanticTokensOn"].value<bool>()) {
    s.semanticTokensOn = *v;
  }
  if (auto const v = root["inlayHintsOn"].value<bool>()) {
    s.inlayHintsOn = *v;
  }
  if (auto const v = root["codeLensOn"].value<bool>()) {
    s.codeLensOn = *v;
  }
  // warningLevel: one of fbc's `-w` option names, English only (never
  // translated), matched case-sensitively like the compiler's own option
  // parser. An unknown string keeps the default — `-w none` — as does any
  // non-string value (value<std::string> fails and leaves the default).
  // fbc's `-w error` is deliberately not in the accepted set (see Settings).
  if (auto const v = root["warningLevel"].value<std::string>()) {
    if (*v == "all" || *v == "constness" || *v == "funcptr") {
      s.warningLevel = 0;
    } else if (*v == "none") {
      s.warningLevel = 4;
    } else if (*v == "pedantic" || *v == "param" || *v == "escape" ||
               *v == "next" || *v == "suffix" || *v == "upcast") {
      s.warningLevel = 1;
    }
    // else: invalid — stays at the default (`-w none`).
  }
  return s;
}

Settings settingsForDir(std::filesystem::path const &dir, bool *ok) {
  if (ok != nullptr) {
    *ok = true;
  }
  if (!hasConfigFile(dir)) {
    return Settings{};
  }
  // Unreadable file → an empty read → parseSettings on "" keeps the defaults
  // (and reports ok, since nothing is malformed — there is just no config);
  // no error plumbing needed here.
  std::ifstream in(dir / kConfigFileName);
  std::string const text{std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>()};
  return parseSettings(text, ok);
}

} // namespace fblang