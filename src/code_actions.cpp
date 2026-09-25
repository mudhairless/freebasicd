/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "code_actions.h"

#include "i18n.h"
#include "language.h"
#include "lexer.h"
#include "symbols.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fblang {

namespace {

// How many include-retarget candidates one diagnostic may offer. The list is
// capped so a common header name spread over a big workspace cannot bury the
// other fixes; the shallowest candidates sort first, so the cap keeps the most
// plausible ones.
constexpr std::size_t kMaxIncludeCandidates = 5;

bool isSpaceOrTab(char c) { return c == ' ' || c == '\t'; }

// A line that carries nothing but whitespace and its line ending.
bool isBlankLine(std::string_view line) {
  for (char c : line) {
    if (!isSpaceOrTab(c) && c != '\r') {
      return false;
    }
  }
  return true;
}

// The buffer's line ending, so an inserted closer never introduces a lone LF
// into a CRLF buffer (or the reverse).
std::string_view lineEnding(std::string_view content) {
  return content.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
}

// Start of the physical line containing `off`.
std::size_t lineStartAt(std::string_view content, std::size_t off) {
  if (off == 0) {
    return 0;
  }
  std::size_t const nl = content.rfind('\n', off - 1);
  return nl == std::string_view::npos ? 0 : nl + 1;
}

// Leading spaces/tabs of the physical line containing `off` — the indentation
// an inserted closer copies from its own opener.
std::string indentAt(std::string_view content, std::uint32_t off) {
  std::size_t const limit = std::min<std::size_t>(off, content.size());
  std::size_t const lineStart = lineStartAt(content, limit);
  std::size_t indent = 0;
  while (lineStart + indent < limit &&
         isSpaceOrTab(content[lineStart + indent])) {
    ++indent;
  }
  return std::string(content.substr(lineStart, indent));
}

// Where a closer goes, and what must precede it. Two shapes, decided by the
// buffer's last line:
//  - last line carries text -> append after it, opening a fresh line (the
//    prefix is that newline, and `at` is the buffer end, which is not a line
//    start);
//  - last line is blank -> land on the first of the trailing blank lines, so
//    the closer sits right below the last statement and keeps the blank lines
//    below it. `at` is then a line start and needs no prefix.
struct Insertion {
  std::uint32_t at = 0;
  std::string prefix;
};

Insertion closerInsertion(std::string_view content) {
  Insertion in;
  std::size_t lineStart = content.rfind('\n');
  lineStart = (lineStart == std::string_view::npos) ? 0 : lineStart + 1;
  if (!isBlankLine(content.substr(lineStart))) {
    in.at = static_cast<std::uint32_t>(content.size());
    in.prefix = std::string(lineEnding(content));
    return in;
  }
  while (lineStart > 0) {
    std::size_t const prevNewline = lineStart - 1;
    std::size_t const prevStart =
        content.rfind('\n', prevNewline == 0 ? 0 : prevNewline - 1);
    std::size_t const start =
        (prevStart == std::string_view::npos) ? 0 : prevStart + 1;
    if (!isBlankLine(content.substr(start, prevNewline - start))) {
      break;
    }
    lineStart = start;
  }
  in.at = static_cast<std::uint32_t>(lineStart);
  return in;
}

// `unterminated-block` -> append the closer this opener expects.
//
// The parser closes an unterminated block at EOF, so the block's own range
// says nothing about where its closer belongs: every still-open block ends at
// the buffer end, and inserting an *outer* block's closer there would invert
// the nesting. One fix therefore closes exactly one block — the innermost,
// which is the one the parser reports first — at the end of the buffer. The
// re-parse that follows makes the next enclosing block the innermost, and its
// fix appends its closer after this one, so applying the fixes in turn nests
// them correctly.
std::vector<QuickFix> insertBlockCloser(Diagnostic const &d,
                                        QuickFixContext const &ctx) {
  if (ctx.doc == nullptr) {
    return {};
  }
  std::string const closer = expectedCloserAt(ctx.doc->tokens, d.range.beg);
  if (closer.empty()) {
    return {}; // an opener with no closer in the language tables: offer nothing
  }
  Insertion const in = closerInsertion(ctx.content);
  QuickFix fix;
  // TRANSLATORS: %s is a FreeBASIC block closer written verbatim
  // (END SUB, NEXT, WEND, #ENDIF, ...); it is never translated.
  fix.title = trf("Insert '%s'", closer);
  fix.code = d.code;
  fix.diagRange = d.range;
  fix.edits.push_back({SourceRange{in.at, in.at},
                       in.prefix + indentAt(ctx.content, d.range.beg) + closer +
                           std::string(lineEnding(ctx.content))});
  return {std::move(fix)};
}

// True when `text` can be a filename literal: non-empty, no quote, newline, or
// leading/trailing separator. Bounds what a stale client-supplied range can
// turn into an edit.
bool looksLikeFilenameLiteral(std::string_view text) {
  if (text.empty() || text.front() == '/' || text.back() == '/') {
    return false;
  }
  for (char c : text) {
    if (c == '"' || c == '\n' || c == '\r') {
      return false;
    }
  }
  return true;
}

// `include-not-found` -> point the existing directive at a file that exists.
//
// The diagnostic sits on a literal that resolved nowhere, so the fix has to
// retarget *that* directive — a second `#include` of the same name elsewhere
// in the file would leave the reported edge unresolved and the diagnostic
// standing. Candidates are the workspace files whose name (or stem, so a
// mistyped extension still matches) fits the literal, expressed relative to
// the including file and verified through the document's own include-resolution
// seam: a candidate is offered only when the next publish would resolve it.
std::vector<QuickFix> retargetInclude(Diagnostic const &d,
                                      QuickFixContext const &ctx) {
  if (ctx.doc == nullptr || ctx.documentPath == nullptr ||
      ctx.workspaceFiles == nullptr || !ctx.resolveInclude) {
    return {};
  }
  if (d.range.beg >= d.range.end || d.range.end > ctx.content.size()) {
    return {};
  }
  std::string_view const current =
      ctx.content.substr(d.range.beg, d.range.end - d.range.beg);
  if (!looksLikeFilenameLiteral(current)) {
    return {};
  }

  std::filesystem::path const wanted{std::string(current)};
  std::string const wantName = toLowerChars(wanted.filename().string());
  std::string const wantStem = toLowerChars(wanted.stem().string());
  std::filesystem::path const dir = ctx.documentPath->parent_path();

  struct Candidate {
    std::size_t depth; // path segments, so the shallowest file is offered first
    std::string literal;
  };
  std::vector<Candidate> candidates;
  for (auto const &file : *ctx.workspaceFiles) {
    if (!file) {
      continue;
    }
    std::filesystem::path const path(file->path);
    if (toLowerChars(path.filename().string()) != wantName &&
        toLowerChars(path.stem().string()) != wantStem) {
      continue;
    }
    std::string literal = path.lexically_relative(dir).generic_string();
    if (literal.empty() || literal == ".") {
      continue; // the including file itself
    }
    if (!ctx.resolveInclude(literal)) {
      continue; // would not resolve on the next publish: not a fix
    }
    if (std::any_of(candidates.begin(), candidates.end(),
                    [&](Candidate const &c) { return c.literal == literal; })) {
      continue;
    }
    candidates.push_back({static_cast<std::size_t>(
                              std::count(literal.begin(), literal.end(), '/')),
                          std::move(literal)});
  }
  if (candidates.empty()) {
    return {}; // nothing in the workspace fits: never guess
  }
  std::sort(candidates.begin(), candidates.end(),
            [](Candidate const &a, Candidate const &b) {
              return a.depth != b.depth ? a.depth < b.depth
                                        : a.literal < b.literal;
            });

  std::vector<QuickFix> fixes;
  for (std::size_t i = 0; i < candidates.size() && i < kMaxIncludeCandidates;
       ++i) {
    QuickFix fix;
    // TRANSLATORS: %s is an include path the fix would write (e.g.
    // inc/config.bi), quoted; it is never translated.
    fix.title = trf("Change include to \"%s\"", candidates[i].literal);
    fix.code = d.code;
    fix.diagRange = d.range;
    // Only the literal's bytes change — the quotes are outside the range.
    fix.edits.push_back({d.range, candidates[i].literal});
    fixes.push_back(std::move(fix));
  }
  return fixes;
}

} // namespace

std::vector<QuickFixRegistration> const &quickFixProviders() {
  // One row per fix, in the order the client sees them. A new fix adds a row
  // and a function; nothing else in the server changes.
  static std::vector<QuickFixRegistration> const table = {
      // parser.cpp: a block opener that reached EOF without its closer.
      {"unterminated-block", insertBlockCloser},
      // M6: an `#include` edge that resolved nowhere (session.cpp publishes
      // these through unresolvedIncludeDiagnostics).
      {"include-not-found", retargetInclude},
  };
  return table;
}

QuickFixProvider const *quickFixProviderFor(std::string const &code) {
  for (QuickFixRegistration const &reg : quickFixProviders()) {
    if (reg.code == code) {
      return &reg.provider;
    }
  }
  return nullptr;
}

std::vector<Diagnostic>
unresolvedIncludeDiagnostics(std::vector<IncludeEdge> const &edges,
                             std::size_t contentSize) {
  std::vector<Diagnostic> out;
  for (IncludeEdge const &e : edges) {
    if (!e.target.empty()) {
      continue;
    }
    if (e.targetRange.beg >= e.targetRange.end ||
        e.targetRange.end > contentSize) {
      continue; // a directive with no filename literal
    }
    Diagnostic d;
    d.range = e.targetRange;
    d.severity = Severity::Error;
    d.code = "include-not-found";
    d.message = trf("include file not found: \"%s\"", e.literal);
    out.push_back(std::move(d));
  }
  return out;
}

} // namespace fblang
