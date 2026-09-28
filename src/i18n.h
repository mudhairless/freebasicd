/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// Localization support: GNU gettext (system libintl, found via
// cmake/FindIntl.cmake — never vendored) for log, diagnostic, and editor-UI
// messages (a code lens title is the last of these).
// The message domain is "freebasicd"; catalogs are built by the CMake
// `translations` target from po/*.po and live under
// <locale>/<lang>/LC_MESSAGES/freebasicd.mo.
//
// Never-translate invariants, enforced by tests/i18n_checks:
//   * the word `FreeBASIC` never appears inside a translatable literal — it
//     is a proper noun that only enters a message as a %s argument;
//   * FreeBASIC keywords never appear inside a translatable literal with an
//     uppercase spelling (END, SUB, NEXT, ...) — a keyword reaches the user
//     only as a dynamic insertion (e.g. closeBlock display text, dialect
//     names) passed to trf(), which translators never see. Lowercase
//     homographs in English prose ("for doc comments") are ordinary words.
//
// Translators see only the msgids extracted by xgettext (`po-template`
// target). Add a `// TRANSLATORS:` comment directly above a tr()/trf() call
// when the message needs context.

#include <libintl.h>

#include <initializer_list>
#include <string>
#include <string_view>

namespace fblang {

// Initialize gettext: bind the "freebasicd" domain to the locale catalog
// directory (env override FBLANG_LOCALEDIR, else the build-tree catalog for a
// dev build, else the configured install prefix), force UTF-8 output, and put
// the process into its environment locale. Idempotent; call once at startup.
void initI18n();

// Best-effort switch to the locale the LSP client reports in
// `ClientCapabilities.general.locale` (an IETF language tag). Only tags the
// OS can actually install switch the catalog; otherwise gettext keeps using
// the environment locale. Harmless when the tag is uninstalled or malformed.
void setClientLocale(std::string_view locale);

// Localized message lookup in the active domain; returns `msgid` unchanged
// when no catalog or no translation applies.
inline const char *tr(const char *msgid) { return gettext(msgid); }

namespace detail {

// Substitute the first three `%s` in an already-translated template with
// `args`, in order; any further `%s` and every other byte pass through. The
// substitution is textual rather than a printf call because a `%s` in a msgid
// stands for arbitrary text — an identifier, a keyword, a count, a file name —
// never for a typed value, and gettext translates the template only.
inline std::string
substitutePercentS(std::string_view t,
                   std::initializer_list<std::string_view> args) {
  std::string out;
  out.reserve(t.size());
  size_t ai = 0;
  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i] == '%' && i + 1 < t.size() && t[i + 1] == 's') {
      if (ai < args.size()) {
        out += *(args.begin() + static_cast<std::ptrdiff_t>(ai));
      }
      ++ai;
      ++i;
    } else {
      out += t[i];
    }
  }
  return out;
}

} // namespace detail

// Localized message template with up to three %s placeholders. The template is
// translated; the arguments (identifiers, keywords, file names, the word
// "FreeBASIC") are inserted verbatim and are never translated. Marked
// c-format so msgfmt --check rejects a translation whose placeholders drift.
inline std::string trf(const char *msgid, std::string_view a0 = {},
                       std::string_view a1 = {}, std::string_view a2 = {}) {
  return detail::substitutePercentS(tr(msgid), {a0, a1, a2});
}

// Localized count message: the singular for n == 1 and the plural otherwise, as
// decided by the *catalog's* plural rule through ngettext — a language whose
// rule has three or four forms cannot be served by the two msgids English
// needs, so the choice belongs to the catalog and never to this code. The count
// is inserted verbatim at `%s`, like every other argument. Marked c-format (see
// the `trn` keyword in the `po-template` target) so msgfmt --check rejects a
// translation that drops or reorders the placeholder.
inline std::string trn(const char *msgid, const char *plural, unsigned long n) {
  std::string const count = std::to_string(n);
  return detail::substitutePercentS(ngettext(msgid, plural, n), {count});
}

} // namespace fblang