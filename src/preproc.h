/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// The FreeBASIC preprocessor, as far as a language server needs it: the
// `#define`/`#macro` symbol table, the intrinsic `__FB_*` defines, and a
// constant-expression evaluator for the `#if` family. Byte-offset based and
// LSP-agnostic; the parser drives it one directive line at a time and asks
// `active()` whether the content that follows is reachable.
//
// Core rule for conditional compilation (our documented divergence from fbc,
// FreeBASIC.md §12): a `#if` chain whose conditions cannot all be decided is
// *not* trusted — the whole `#if ... #endif` region is treated as unreachable.
// fbc instead folds an unknown identifier to 0; a language server that guesses
// would parse and report code the user's real compile never sees.

namespace fblang {

// One reachable `#define NAME` / `#macro NAME(...)`, ready to publish as a
// `SymbolKind::Define` symbol so it resolves, hovers, and autocompletes like
// any other declaration. `nameBeg`/`nameEnd` are byte offsets relative to the
// start of the directive text passed to `feed`.
struct PreprocDefine {
  std::string name;          // as written (original case)
  std::string key;           // lowercase lookup key (suffix char included)
  bool isMacro = false;      // `#macro ... #endmacro`
  bool functionLike = false; // carries a `(...)` parameter list
  std::uint32_t nameBeg = 0; // offset of the name token in the directive line
  std::uint32_t nameEnd = 0;
  // Hover/details header: the directive's first line only (`#define MAX_ITEMS
  // 10`, or `#macro say(w)`), so a macro's parameters show without dumping its
  // body.
  std::string signature;
};

// A define's replacement text, as the evaluator sees it. `functionLike`
// entries (a `#macro`, or any `#define NAME(...)`) are never textually
// expanded — fbc rejects a call-form name inside `#if`.
struct PreprocEntry {
  bool functionLike = false;
  std::string value; // empty for a bare `#define NAME`
};

// Outcome of feeding one `#` directive line.
struct PreprocFeedResult {
  // `#else`/`#elseif`(`def`/`ndef`)/`#endif` with no open `#if`. The caller
  // owns the diagnostic; `#endif` is reported as it always was.
  bool strayCloser = false;
  // Set when the line is a reachable `#define`/`#macro` the caller should
  // publish.
  std::optional<PreprocDefine> define;
};

class Preprocessor {
public:
  Preprocessor();

  // Feed one directive line (its text, starting at `#`, possibly spanning
  // continuation lines). Updates the conditional stack and the define table.
  // `active()` then reports whether the content after this line is reachable.
  PreprocFeedResult feed(std::string_view line);

  // Whether content at the current point is reachable. False inside an
  // inactive branch, inside a decidable-false `#if`, and inside a `#macro`
  // body.
  bool active() const;
  bool inMacroBody() const { return inMacroBody_; }

  // Whether `name` (any case) is currently defined — the `#ifdef` question and
  // the `defined(...)` operator. Built-ins answer true until `#undef`'d.
  bool isDefined(std::string_view name) const;

  // The replacement text of `name`'s entry for the evaluator's splicer, or
  // nullptr when the name is unknown or not expandable.
  PreprocEntry const *entryFor(std::string_view name) const;

private:
  struct Frame {
    bool parentActive = true; // enclosing reachability when this #if arrived
    bool taken = false;       // some arm of this conditional decided true
    bool undecided = false;   // some arm's condition was undecidable
    bool curActive = false;   // the arm now in force is reachable
  };

  // All frames' arms must be live for content to be reachable.
  bool outerActive() const;

  // Evaluate a `#if`/`#elseif` condition. `decided` is false when an
  // identifier cannot be expanded, a define has no value, the expression has a
  // syntax/type error, or expansion recurses too deep.
  bool evaluateCondition(std::string_view text, bool *decided) const;

  std::unordered_map<std::string, PreprocEntry> defines_;
  std::vector<Frame> frames_;
  bool inMacroBody_ = false;
};

} // namespace fblang
