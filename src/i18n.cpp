/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "i18n.h"

#include <clocale>
#include <cstdlib>
#include <filesystem>
#include <libintl.h>
#include <locale.h>
#include <string>
#include <string_view>

namespace fblang {

namespace {

constexpr const char *kDomain = "freebasiclsp";

// The directory holding <lang>/LC_MESSAGES/freebasiclsp.mo trees. Order:
// an explicit override wins, then the build-tree catalog (dev builds run
// straight from the build directory), then the configured install prefix as a
// last resort — the tree only exists there once `cmake --install` ran.
const char *localeDir() {
  if (const char *env = std::getenv("FBLANG_LOCALEDIR");
      env != nullptr && *env != '\0') {
    return env;
  }
#ifdef FBLANG_LOCALEDIR_BUILD
  if (std::filesystem::is_directory(FBLANG_LOCALEDIR_BUILD)) {
    return FBLANG_LOCALEDIR_BUILD;
  }
#endif
#ifdef FBLANG_LOCALEDIR_INSTALL
  return FBLANG_LOCALEDIR_INSTALL;
#else
  return "";
#endif
}

} // namespace

void initI18n() {
  // Only the message facets follow the environment; the numeric/parsing
  // facets stay at "C" so LSP output never depends on the UI locale.
  (void)setlocale(LC_MESSAGES, "");
  (void)bindtextdomain(kDomain, localeDir());
  (void)bind_textdomain_codeset(kDomain, "UTF-8");
  (void)textdomain(kDomain);
}

void setClientLocale(std::string_view locale) {
  if (locale.empty()) {
    return;
  }
  // IETF language tags use '-' (de-DE); the C library wants '_' (de_DE).
  std::string const tag(locale);
  std::string cTag(tag);
  for (char &c : cTag) {
    if (c == '-') {
      c = '_';
    }
  }
  // Best effort: returns nullptr (and leaves the locale untouched) when the
  // tag is not installed on this system.
  (void)setlocale(LC_MESSAGES, cTag.c_str());
}

} // namespace fblang