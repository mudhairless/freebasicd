/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "code_actions.h"

#include "i18n.h"
#include "language.h"
#include "lexer.h"
#include "resolve.h"
#include "symbols.h"

#include <fstream>
#include <iterator>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
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
  return std::all_of(line.begin(), line.end(),
                     [](char c) { return isSpaceOrTab(c) || c == '\r'; });
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

// Where a closer goes, and what must precede it.
//  - `knownAt` is set -> the parse recorded where the closer belongs (the
//    statement the block's grammar could not accept, or the closer that did not
//    match), so the insertion lands there and needs no prefix: it is a
//    statement's own first token, and everything after it stays put.
//  - otherwise there was no evidence, and the buffer's last line decides:
//      * last line carries text -> append after it, opening a fresh line (the
//        prefix is that newline, and `at` is the buffer end, which is not a
//        line start);
//      * last line is blank -> land on the first of the trailing blank lines,
//      so
//        the closer sits right below the last statement and keeps the blank
//        lines below it. `at` is then a line start and needs no prefix.
//
// The no-evidence case is not a fallback for a procedure body: that body
// accepts every statement, so end-of-buffer is its real answer (FreeBASIC.md
// §7).
struct Insertion {
  std::uint32_t at = 0;
  std::string prefix;
};

Insertion closerInsertion(std::string_view content,
                          std::optional<std::uint32_t> knownAt) {
  Insertion in;
  if (knownAt && *knownAt <= content.size()) {
    in.at = *knownAt;
    return in;
  }
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

// `unterminated-block` -> insert the closer this opener expects.
//
// Where it goes is the parse's answer, never a guess made here: when the parser
// recorded the block's logical end (a statement a record/enum body's grammar
// could not accept, or a closer that did not match) the fix writes the closer
// *there* — before the offending statement, which is where fbc says it belongs
// (FreeBASIC.md §7). With no evidence, the buffer's end is the best available
// guess, and that is the procedure body's real answer: its grammar accepts
// every statement, so nothing in the source marks where the body ends.
//
// One fix still closes exactly one block — the innermost, which the parser
// reports first — so applying fixes in turn nests them correctly: the re-parse
// after the first makes the next enclosing block the innermost.
std::vector<QuickFix> insertBlockCloser(Diagnostic const &d,
                                        QuickFixContext const &ctx) {
  if (ctx.doc == nullptr) {
    return {};
  }
  std::string const closer = expectedCloserAt(ctx.doc->tokens, d.range.beg);
  if (closer.empty()) {
    return {}; // an opener with no closer in the language tables: offer nothing
  }
  Insertion const in = closerInsertion(ctx.content, d.closerAt);
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
  return std::all_of(text.begin(), text.end(),
                     [](char c) { return c != '"' && c != '\n' && c != '\r'; });
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

// `invalid-member-name` -> move the member's name off the reserved word.
//
// The one repair that needs no guess. A reserved word cannot be a member name
// (FreeBASIC.md §7, probed), and a trailing `_` is not part of the word — the
// suffix is a separate identifier character, the same rule that makes `foo` and
// `foo$` two different variables. So `and_` is already a legal name and needs
// no second hop: the edit is the diagnostic's own range plus one character, and
// nothing about the enclosing body is consulted to produce it.
std::vector<QuickFix> renameReservedMemberName(Diagnostic const &d,
                                               QuickFixContext const &ctx) {
  if (d.range.beg >= d.range.end || d.range.end > ctx.content.size()) {
    return {}; // a range from another revision of the buffer
  }
  std::string_view const name =
      ctx.content.substr(d.range.beg, d.range.end - d.range.beg);
  // The range has to *be* the name: the diagnostic names one word, and a
  // keyword is letters only, so anything else means the bytes have moved under
  // a stale range. Appending `_` to that is not a rename, so offer nothing. The
  // test also bounds the edit: letters hold no newline, quote or backslash, so
  // the replacement cannot escape the range it claims.
  if (name.empty()) {
    return {};
  }
  for (char const c : name) {
    if ((c < 'a' || c > 'z') && (c < 'A' || c > 'Z')) {
      return {};
    }
  }

  QuickFix fix;
  // TRANSLATORS: %s is the member name the fix would write with a trailing
  // underscore (e.g. `and_`); it is a FreeBASIC identifier, never translated.
  fix.title = trf("Rename member to \"%s_\"", name);
  fix.code = d.code;
  fix.diagRange = d.range;
  // Only the name changes, so the range is replaced by name-plus-underscore and
  // nothing else on the line is touched.
  fix.edits.push_back({d.range, std::string(name) + "_"});
  return {std::move(fix)};
}

// `keyword-suffix` -> delete the ignored suffix char.
//
// The parser's diagnostic sits on exactly the suffix character — one byte out
// of `% & ! $` — so the fix is the diagnostic's own range replaced by nothing,
// and the buffer fully determines it: the token already lexed as the bare
// keyword, and deleting the char makes the source hold what fbc reads
// (warning 44, "Suffix ignored"). No workspace knowledge is consulted, and any
// range that is not one of those four bytes means the buffer moved under a
// stale diagnostic: offer nothing, like the member rename does.
std::vector<QuickFix> removeKeywordSuffix(Diagnostic const &d,
                                          QuickFixContext const &ctx) {
  if (d.range.end != d.range.beg + 1 || d.range.end > ctx.content.size()) {
    return {};
  }
  char const c = ctx.content[d.range.beg];
  if (c != '%' && c != '&' && c != '!' && c != '$') {
    return {};
  }
  QuickFix fix;
  // TRANSLATORS: %s is the suffix character (%, &, ! or $); it is never
  // translated and the placeholder is %s because only %s is substituted.
  fix.title = trf("Remove the '%s' suffix", std::string(1, c));
  fix.code = d.code;
  fix.diagRange = d.range;
  fix.edits.push_back({d.range, ""});
  return {std::move(fix)};
}

// `module-var-not-shared` -> add `Shared` to the declaration the diagnostic
// names.
//
// The diagnostic sits on the *use* (a bare name inside a procedure, a leading
// `.name`, or a `typeof(.name)`/`typeof(name)` directive argument); the
// declaration it is about lives elsewhere in the buffer, so the fix and the
// diagnostic must agree on which one — that is `moduleSharedCandidate`, the
// same predicate `resolve.cpp` gated the diagnostic on, so a fix is offered
// exactly when the diagnostic reported one. The edit inserts ` Shared` right
// after the declaration's leading keyword, which is `Dim`-first source order
// and valid for every keyword the candidate accepted.
std::vector<QuickFix> addSharedModuleVar(Diagnostic const &d,
                                         QuickFixContext const &ctx) {
  if (ctx.doc == nullptr || d.range.beg >= d.range.end ||
      d.range.end > ctx.content.size()) {
    return {};
  }
  std::string const key =
      canonicalKey(ctx.content.substr(d.range.beg, d.range.end - d.range.beg),
                   ctx.doc->parse.lang);
  Symbol const *decl = moduleSharedCandidate(ctx.doc->parse, ctx.content, key);
  if (decl == nullptr) {
    return {}; // no in-file declaration this fix could edit
  }
  std::optional<std::uint32_t> const at =
      sharedInsertionOffset(ctx.content, *decl);
  if (!at) {
    return {};
  }
  QuickFix fix;
  // TRANSLATORS: the first %s is the FreeBASIC keyword Shared (passed as an
  // argument, never translated); the second is the variable's name.
  fix.title = trf("Add '%s' to '%s'", "Shared", decl->name);
  fix.code = d.code;
  fix.diagRange = d.range;
  fix.edits.push_back({SourceRange{*at, *at}, " Shared"});
  return {std::move(fix)};
}

// `redundant-leading-dot` -> delete the dot.
//
// The diagnostic sits on the dot itself, one byte before the identifier. The
// hint's proof is that the bare name at that site resolves to the same
// module-level declaration, so deleting exactly this byte cannot change what
// the name means. Anything other than a one-byte `.` range means the buffer
// moved under a stale diagnostic: offer nothing.
std::vector<QuickFix> removeRedundantLeadingDot(Diagnostic const &d,
                                                QuickFixContext const &ctx) {
  if (d.range.end != d.range.beg + 1 || d.range.end > ctx.content.size()) {
    return {};
  }
  if (ctx.content[d.range.beg] != '.') {
    return {};
  }
  QuickFix fix;
  // TRANSLATORS: the fix deletes a redundant leading dot from an expression.
  fix.title = trf("Remove the redundant leading dot");
  fix.code = d.code;
  fix.diagRange = d.range;
  fix.edits.push_back({d.range, ""});
  return {std::move(fix)};
}

// The constructor name the wrap fix gives a module: its file stem,
// kept to identifier characters, capitalized, with `Init` appended
// (`second.bas` -> `SecondInit`). A stem that keeps nothing becomes
// `ModuleInit`, and one that opens with a digit gains a letter prefix so the
// result is a valid identifier.
std::string constructorNameForPath(std::filesystem::path const &path) {
  std::string kept;
  for (char const c : path.stem().string()) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_') {
      kept.push_back(c);
    }
  }
  if (kept.empty()) {
    kept = "Module";
  }
  if (kept.front() >= '0' && kept.front() <= '9') {
    kept.insert(kept.begin(), 'A');
  }
  if (kept.front() >= 'a' && kept.front() <= 'z') {
    kept.front() = static_cast<char>(kept.front() - 'a' + 'A');
  }
  return kept + "Init";
}

// `module-code-outside-main` -> wrap the run in `Sub <name>() Constructor`.
//
// The diagnostic is anchored on the run's first statement, and the fix
// wraps the whole run: the run's text is preserved verbatim between two new
// lines — the constructor opener before the first statement and `End Sub`
// after the last. The run is recovered from the parse (the same grouping
// the diagnostic was built from), so a fix is offered exactly when the
// diagnostic's range is a run's head; anything else is a buffer that moved
// under a stale diagnostic, and gets nothing.
std::vector<QuickFix> wrapInModuleConstructor(Diagnostic const &d,
                                              QuickFixContext const &ctx) {
  if (ctx.doc == nullptr || ctx.documentPath == nullptr) {
    return {};
  }
  if (d.range.beg >= d.range.end || d.range.end > ctx.content.size()) {
    return {};
  }
  for (std::vector<SourceRange> const &run : moduleCodeRuns(ctx.doc->parse)) {
    if (run.empty() || run.front().beg != d.range.beg ||
        run.front().end != d.range.end) {
      continue;
    }
    std::string const name = constructorNameForPath(*ctx.documentPath);
    QuickFix fix;
    // TRANSLATORS: %s is the constructor name the fix would write (e.g.
    // SecondInit); it is a FreeBASIC identifier, never translated.
    fix.title = trf("Wrap in constructor '%s'", name);
    fix.code = d.code;
    fix.diagRange = d.range;
    std::string const le(lineEnding(ctx.content));
    std::string header;
    header.append("Sub ");
    header.append(name);
    header.append("() Constructor");
    header.append(le);
    fix.edits.push_back(
        {SourceRange{run.front().beg, run.front().beg}, std::move(header)});
    fix.edits.push_back(
        {SourceRange{run.back().end, run.back().end}, le + "End Sub"});
    return {std::move(fix)};
  }
  return {};
}

bool isRoutineKind(SymbolKind k) {
  return k == SymbolKind::Sub || k == SymbolKind::Function ||
         k == SymbolKind::Property;
}
bool isTypeKind(SymbolKind k) {
  return k == SymbolKind::Type || k == SymbolKind::Union ||
         k == SymbolKind::Enum;
}
bool isValueKind(SymbolKind k) {
  return k == SymbolKind::Dim || k == SymbolKind::Const;
}

Symbol const *findRootByKey(ParseResult const &parse, std::string const &key,
                            bool (*pred)(SymbolKind)) {
  for (Symbol const &r : parse.roots) {
    if (r.key == key && pred(r.kind)) {
      return &r;
    }
  }
  return nullptr;
}

Symbol const *findAnyRootByKey(ParseResult const &parse,
                               std::string const &key) {
  for (Symbol const &r : parse.roots) {
    if (r.key == key && !r.key.empty()) {
      return &r;
    }
    for (Symbol const &c : r.children) {
      if (c.key == key && !c.key.empty()) {
        return &c;
      }
    }
  }
  return nullptr;
}

bool isHeaderPathStr(std::string const &p) {
  std::string const ext =
      toLowerChars(std::filesystem::path(p).extension().string());
  return ext == ".bi";
}

bool fileDeclaresKey(std::shared_ptr<IndexedFile const> const &file,
                     std::string const &key) {
  if (!file) {
    return false;
  }
  for (Symbol const &r : file->roots) {
    if (r.key == key && !r.key.empty() &&
        (isRoutineKind(r.kind) || isTypeKind(r.kind) || isValueKind(r.kind))) {
      return true;
    }
    if (r.kind == SymbolKind::Enum && !r.explicitEnum) {
      for (Symbol const &c : r.children) {
        if (c.key == key) {
          return true;
        }
      }
    }
  }
  return false;
}

std::uint32_t insertionOffsetFor(std::string_view content,
                                 ParseResult const &parse,
                                 std::uint32_t firstUse) {
  Symbol const *enclosing = nullptr;
  std::function<void(std::vector<Symbol> const &)> walk =
      [&](std::vector<Symbol> const &nodes) {
        for (Symbol const &n : nodes) {
          if (n.range.beg <= firstUse && firstUse <= n.range.end) {
            if ((isRoutineKind(n.kind) || n.kind == SymbolKind::Constructor ||
                 n.kind == SymbolKind::Destructor ||
                 n.kind == SymbolKind::Operator) &&
                (enclosing == nullptr ||
                 (n.range.end - n.range.beg) <
                     (enclosing->range.end - enclosing->range.beg))) {
              enclosing = &n;
            }
            walk(n.children);
          }
        }
      };
  walk(parse.roots);
  std::uint32_t const anchor =
      enclosing != nullptr ? enclosing->range.beg : firstUse;
  return static_cast<std::uint32_t>(lineStartAt(content, anchor));
}

// `use-before-declaration` -> Declare, reorder, or `#include` above the
// first use.
std::vector<QuickFix> fixUseBeforeDeclaration(Diagnostic const &d,
                                              QuickFixContext const &ctx) {
  if (ctx.doc == nullptr || d.range.beg >= d.range.end ||
      d.range.end > ctx.content.size()) {
    return {};
  }
  std::string const name(
      ctx.content.substr(d.range.beg, d.range.end - d.range.beg));
  std::string const key = canonicalKey(name, ctx.doc->parse.lang);
  if (key.empty()) {
    return {};
  }
  std::vector<QuickFix> fixes;
  std::string const le(lineEnding(ctx.content));

  // First use of the name that is before its in-file introduction.
  std::uint32_t firstUse = d.range.beg;
  Symbol const *inFileDecl = findAnyRootByKey(ctx.doc->parse, key);
  if (inFileDecl != nullptr) {
    std::uint32_t earliest = d.range.beg;
    for (Occurrence const &occ : inFileDecl->occurrences) {
      earliest = std::min(occ.range.beg, earliest);
    }
    firstUse = earliest;
  }
  std::uint32_t const insertAt =
      insertionOffsetFor(ctx.content, ctx.doc->parse, firstUse);

  // In-file fixes.
  Symbol const *routineDecl = findRootByKey(ctx.doc->parse, key, isRoutineKind);
  if (routineDecl != nullptr && !routineDecl->declOnly &&
      routineDecl->selection.beg > d.range.beg) {
    std::string const sig = routineDecl->signature;
    if (!sig.empty()) {
      QuickFix fix;
      // TRANSLATORS: the first %s is the FreeBASIC keyword Declare (passed
      // as an argument, never translated); the second is the routine name.
      fix.title = trf("Insert '%s' for '%s'", "Declare", name);
      fix.code = d.code;
      fix.diagRange = d.range;
      std::string text = "Declare " + sig + le;
      fix.edits.push_back({SourceRange{insertAt, insertAt}, std::move(text)});
      fixes.push_back(std::move(fix));
    }
  } else {
    Symbol const *typeDecl = findRootByKey(ctx.doc->parse, key, isTypeKind);
    if (typeDecl != nullptr && typeDecl->selection.beg > d.range.beg) {
      if (typeDecl->range.beg < ctx.content.size() &&
          typeDecl->range.end <= ctx.content.size() &&
          typeDecl->range.end > typeDecl->range.beg) {
        std::string const block(ctx.content.substr(
            typeDecl->range.beg, typeDecl->range.end - typeDecl->range.beg));
        QuickFix fix;
        // TRANSLATORS: %s is the type name being moved above its first use.
        fix.title = trf("Move '%s' above its first use", name);
        fix.code = d.code;
        fix.diagRange = d.range;
        std::string insertText = block;
        if (!insertText.empty() && insertText.back() != '\n') {
          insertText += le;
        }
        fix.edits.push_back({SourceRange{insertAt, insertAt}, insertText});
        std::uint32_t delEnd = typeDecl->range.end;
        if (delEnd < ctx.content.size() && ctx.content[delEnd] == '\n') {
          ++delEnd;
          if (delEnd > 1 && ctx.content[delEnd - 2] == '\r') {
            // CRLF already counted via the \n; keep simple.
          }
        }
        fix.edits.push_back({SourceRange{typeDecl->range.beg, delEnd}, ""});
        fixes.push_back(std::move(fix));
      }
    } else {
      Symbol const *valueDecl = findRootByKey(ctx.doc->parse, key, isValueKind);
      if (valueDecl != nullptr && valueDecl->selection.beg > d.range.beg) {
        std::uint32_t lineBeg = static_cast<std::uint32_t>(
            lineStartAt(ctx.content, valueDecl->selection.beg));
        std::uint32_t lineEnd = valueDecl->selection.end;
        while (lineEnd < ctx.content.size() && ctx.content[lineEnd] != '\n') {
          ++lineEnd;
        }
        std::uint32_t delEnd = lineEnd;
        if (delEnd < ctx.content.size() && ctx.content[delEnd] == '\n') {
          ++delEnd;
        }
        if (lineBeg < ctx.content.size() &&
            valueDecl->selection.beg >= lineBeg) {
          std::string const block(
              ctx.content.substr(lineBeg, delEnd - lineBeg));
          QuickFix fix;
          // TRANSLATORS: %s is the variable or constant name being moved.
          fix.title = trf("Move '%s' above its first use", name);
          fix.code = d.code;
          fix.diagRange = d.range;
          std::string insertText = block;
          if (!insertText.empty() && insertText.back() != '\n') {
            insertText += le;
          }
          fix.edits.push_back({SourceRange{insertAt, insertAt}, insertText});
          fix.edits.push_back({SourceRange{lineBeg, delEnd}, ""});
          fixes.push_back(std::move(fix));
        }
      }
    }
  }

  // Include fixes: workspace headers first.
  struct Cand {
    std::size_t depth;
    std::string literal;
    std::string headerPath;
  };
  std::vector<Cand> cands;
  if (ctx.workspaceFiles != nullptr && ctx.documentPath != nullptr &&
      ctx.resolveInclude) {
    std::filesystem::path const dir = ctx.documentPath->parent_path();
    for (auto const &file : *ctx.workspaceFiles) {
      if (!file || file->path.empty() || !isHeaderPathStr(file->path)) {
        continue;
      }
      if (!fileDeclaresKey(file, key)) {
        continue;
      }
      std::string literal = std::filesystem::path(file->path)
                                .lexically_relative(dir)
                                .generic_string();
      if (literal.empty() || literal == ".") {
        continue;
      }
      if (!ctx.resolveInclude(literal)) {
        continue;
      }
      bool dup = false;
      for (Cand const &c : cands) {
        if (c.literal == literal) {
          dup = true;
        }
      }
      if (dup) {
        continue;
      }
      cands.push_back({static_cast<std::size_t>(
                           std::count(literal.begin(), literal.end(), '/')),
                       literal, file->path});
    }
  }
  std::sort(cands.begin(), cands.end(), [](Cand const &a, Cand const &b) {
    return a.depth != b.depth ? a.depth < b.depth : a.literal < b.literal;
  });
  for (std::size_t i = 0; i < cands.size() && i < kMaxIncludeCandidates; ++i) {
    QuickFix fix;
    // TRANSLATORS: the first %s is the FreeBASIC keyword Include (passed
    // as an argument, never translated); the second is the header path.
    fix.title = trf("%s \"%s\"", "Include", cands[i].literal);
    fix.code = d.code;
    fix.diagRange = d.range;
    std::string text = "#include \"" + cands[i].literal + "\"" + le;
    fix.edits.push_back({SourceRange{insertAt, insertAt}, std::move(text)});
    fixes.push_back(std::move(fix));
  }
  // System folder fallback, only when the workspace search found nothing
  // and no in-file fix already answers (scanning the installed headers
  // on every request that already has a Declare/reorder fix would bury
  // the fast fixes under a full scan).
  if (cands.empty() && fixes.empty() && ctx.resolveInclude &&
      ctx.systemIncludeDir != nullptr && !ctx.systemIncludeDir->empty()) {
    std::filesystem::path const sysDir = *ctx.systemIncludeDir;
    std::vector<Cand> sysCands;
    {
      static std::mutex sysMu;
      static std::map<std::pair<std::string, std::string>, std::vector<Cand>>
          sysCache;
      std::pair<std::string, std::string> const sysKey{sysDir.string(), key};
      std::lock_guard<std::mutex> const lk(sysMu);
      auto const it = sysCache.find(sysKey);
      if (it != sysCache.end()) {
        sysCands = it->second;
      } else {
        std::error_code ec2;
        if (std::filesystem::is_directory(sysDir, ec2)) {
          std::filesystem::recursive_directory_iterator it2(sysDir, ec2);
          std::filesystem::recursive_directory_iterator const end2;
          for (; !ec2 && it2 != end2; it2.increment(ec2)) {
            std::error_code sec2;
            if (it2->is_directory(sec2)) {
              continue;
            }
            std::filesystem::path const p = it2->path();
            if (!isHeaderPathStr(p.string())) {
              continue;
            }
            std::ifstream in(p, std::ios::binary);
            if (!in) {
              continue;
            }
            std::string const content((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
            std::string const lower = toLowerChars(content);
            if (lower.find(key) == std::string::npos) {
              continue;
            }
            AnalyzedDoc const headerDoc = analyze(content);
            bool declares = false;
            for (Symbol const &r : headerDoc.parse.roots) {
              if (r.key == key && !r.key.empty()) {
                declares = true;
                break;
              }
            }
            if (!declares) {
              continue;
            }
            std::string literal = p.lexically_relative(sysDir).generic_string();
            if (literal.empty() || literal == ".") {
              continue;
            }
            if (!ctx.resolveInclude(literal)) {
              continue;
            }
            sysCands.push_back({static_cast<std::size_t>(std::count(
                                    literal.begin(), literal.end(), '/')),
                                literal, p.string()});
            if (sysCands.size() >= kMaxIncludeCandidates) {
              break;
            }
          }
        }
        sysCache[sysKey] = sysCands;
      }
    }
    {
      std::sort(sysCands.begin(), sysCands.end(),
                [](Cand const &a, Cand const &b) {
                  return a.depth != b.depth ? a.depth < b.depth
                                            : a.literal < b.literal;
                });
      for (std::size_t i = 0; i < sysCands.size() && i < kMaxIncludeCandidates;
           ++i) {
        QuickFix fix;
        // TRANSLATORS: the first %s is the FreeBASIC keyword Include
        // (passed as an argument, never translated); the second is the
        // system header path the fix would write.
        fix.title = trf("%s \"%s\"", "Include", sysCands[i].literal);
        fix.code = d.code;
        fix.diagRange = d.range;
        std::string text = "#include \"" + sysCands[i].literal + "\"" + le;
        fix.edits.push_back({SourceRange{insertAt, insertAt}, std::move(text)});
        fixes.push_back(std::move(fix));
      }
    }
    return fixes;
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
      // parser.cpp: a reserved word in the name position of a record or enum
      // member list (see the probed tables in language.cpp).
      {"invalid-member-name", renameReservedMemberName},
      // An `#include` edge that resolved nowhere (session.cpp publishes
      // these through unresolvedIncludeDiagnostics).
      {"include-not-found", retargetInclude},
      // lexer.cpp/parser.cpp: a suffix char on a reserved word (fbc warning
      // 44, "Suffix ignored") — the fix deletes the char the diagnostic sits
      // on, leaving the bare keyword the parser already read.
      {"keyword-suffix", removeKeywordSuffix},
      // resolve.cpp: a module-level `Dim` without `Shared` read inside a
      // procedure (or through a leading `.` / `typeof(.x)` / `typeof(x)`) —
      // fbc errors 42/8/9; the fix adds the keyword to the declaration.
      {"module-var-not-shared", addSharedModuleVar},
      // resolve.cpp: a module-level leading dot whose bare name resolves to
      // the same declaration — our own Hint, so the fix deletes just the dot.
      {"redundant-leading-dot", removeRedundantLeadingDot},
      // resolve.cpp: a run of module-level executable statements outside the
      // main module — the fix wraps the run in a module
      // constructor, which states the load-time intent on purpose.
      {"module-code-outside-main", wrapInModuleConstructor},
      // resolve.cpp: a use before its declaration — Declare, reorder, or
      // `#include` of a header carrying the name above the first use.
      {"use-before-declaration", fixUseBeforeDeclaration},
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
    // fbc's own number for an unresolved `#include` (probed 1.10.2:
    // "error 23: File not found"). The fix still keys on the routing `code`
    // above, never the wire string.
    d.fbcId = FbcDiagId::FileNotFound;
    d.message = trf("include file not found: \"%s\"", e.literal);
    out.push_back(std::move(d));
  }
  return out;
}

} // namespace fblang
