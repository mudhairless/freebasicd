/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "settings.h"

#include <toml++/toml.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>

#include "symbols.h" // toLowerChars

namespace fblang {

namespace {

// A routing key never contains a comma, so an entry that has one names more
// than the key: a wire `Diagnostic.code` was copied in whole
// (`include-not-found, fbc error: 23`). Reduce it to the text before the first
// comma, trailing blanks trimmed, and report whether a tail was dropped.
bool routingKeyOf(std::string_view entry, std::string *key) {
  std::size_t const comma = entry.find(',');
  if (comma == std::string_view::npos) {
    return false;
  }
  std::size_t end = comma;
  while (end > 0 && (entry[end - 1] == ' ' || entry[end - 1] == '\t')) {
    --end;
  }
  key->assign(entry.substr(0, end));
  return true;
}

} // namespace

bool hasConfigFile(std::filesystem::path const &dir) {
  std::error_code ec;
  return std::filesystem::is_regular_file(dir / kConfigFileName, ec);
}

Settings parseSettings(std::string_view tomlText, bool *ok,
                       std::vector<SuppressNote> *notes) {
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
  // suppress: an array of our routing keys (see Settings). Each string is
  // ASCII-lowercased and the result is sorted and deduped, so the value is a
  // set — ordering and case are not part of it. Non-string elements are
  // skipped, and a wrong-typed (non-array) key keeps the default. An entry
  // copied from a diagnostic's wire code (`key, fbc error: N`) is reduced to
  // the key and reported through `notes` so the caller can ask the user to
  // clean the config up (see SuppressNote).
  if (toml::array const *const arr = root["suppress"].as_array()) {
    for (toml::node const &el : *arr) {
      if (auto const str = el.value<std::string>()) {
        std::string const lowered = toLowerChars(*str);
        std::string key;
        if (routingKeyOf(lowered, &key)) {
          if (notes != nullptr) {
            notes->push_back(SuppressNote{*str, key});
          }
        } else {
          key = lowered;
        }
        s.suppress.push_back(std::move(key));
      }
    }
    std::sort(s.suppress.begin(), s.suppress.end());
    s.suppress.erase(std::unique(s.suppress.begin(), s.suppress.end()),
                     s.suppress.end());
  }
  return s;
}

Settings settingsForDir(std::filesystem::path const &dir, bool *ok,
                        std::vector<SuppressNote> *notes) {
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
  return parseSettings(text, ok, notes);
}

} // namespace fblang