/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Quick-fix checks for the M12 code-action module.
//
// The providers are pure functions of (diagnostic, QuickFixContext), so every
// case here runs without an index, a session, or a workspace on disk: the
// include fix's candidate source and its resolution seam are injected, which
// is exactly the seam the session supplies.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "code_actions.h"
#include "resolve.h"
#include "symbols.h"

using namespace fblang;

static int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

namespace {

// A context over `doc` with no workspace: the shape a document served without
// an index (single-file mode) hands the providers.
QuickFixContext bareContext(std::string_view content, AnalyzedDoc const &doc) {
  QuickFixContext ctx;
  ctx.content = content;
  ctx.doc = &doc;
  return ctx;
}

Diagnostic diagAt(std::string const &code, std::uint32_t beg,
                  std::uint32_t end) {
  Diagnostic d;
  d.code = code;
  d.range = SourceRange{beg, end};
  d.severity = Severity::Error;
  return d;
}

// The single fix the registered provider for `code` offers for `d`.
std::vector<QuickFix> fixesFor(std::string const &code, Diagnostic const &d,
                               QuickFixContext const &ctx) {
  QuickFixProvider const *const provider = quickFixProviderFor(code);
  if (provider == nullptr) {
    return {};
  }
  return (*provider)(d, ctx);
}

// A pure insertion at `off`, rendered the way editText spells an edit, so a
// test can assert the insertion geometry as well as the text.
std::string insertAt(std::size_t off) {
  return std::to_string(off) + ":" + std::to_string(off) + ">";
}

// The fix's single edit rendered as "at:end>text" so a test can assert the
// insertion geometry as well as the text.
std::string editText(QuickFix const &fix) {
  if (fix.edits.size() != 1) {
    return "<not one edit>";
  }
  TextEditBytes const &e = fix.edits.front();
  return std::to_string(e.range.beg) + ":" + std::to_string(e.range.end) + ">" +
         e.newText;
}

// An IndexedFile stub at `path` — a candidate only needs its path.
std::shared_ptr<IndexedFile const> fileAt(std::string const &path) {
  auto f = std::make_shared<IndexedFile>();
  f->path = path;
  f->fromDisk = true;
  return f;
}

void CloserFixAppendsOneBlockEnd() {
  // Two nested unterminated blocks: the parser reports the innermost first.
  std::string const src = "sub outer()\n"
                          "  for i = 1 to 3\n"
                          "    print i\n";
  AnalyzedDoc const doc = analyze(src);
  QuickFixContext const ctx = bareContext(src, doc);

  // Innermost: the `for` opener's own range carries the diagnostic.
  std::size_t const forBeg = src.find("for");
  std::vector<QuickFix> const forFixes =
      fixesFor("unterminated-block",
               diagAt("unterminated-block", static_cast<std::uint32_t>(forBeg),
                      static_cast<std::uint32_t>(forBeg + 3)),
               ctx);
  CHECK(forFixes.size() == 1);
  if (forFixes.size() == 1) {
    CHECK(forFixes[0].title == "Insert 'NEXT'");
    CHECK(forFixes[0].code == "unterminated-block");
    // The buffer already ends in a newline, so the closer lands at the buffer
    // end carrying the opener's own indentation.
    CHECK(editText(forFixes[0]) == insertAt(src.size()) + "  NEXT\n");
  }

  // Enclosing: the `sub` gets its own fix, likewise at the end of the buffer.
  // Applying the two in turn is what nests them correctly — the re-parse after
  // the first makes the `sub` the innermost still-open block.
  CHECK(forFixes.size() == 1 && forFixes[0].diagRange.beg == forBeg);
  std::vector<QuickFix> const subFixes =
      fixesFor("unterminated-block", diagAt("unterminated-block", 0, 3), ctx);
  CHECK(subFixes.size() == 1);
  if (subFixes.size() == 1) {
    CHECK(subFixes[0].title == "Insert 'END SUB'");
    CHECK(editText(subFixes[0]) == insertAt(src.size()) + "END SUB\n");
  }
}

// Where the parse recorded a boundary, the closer goes *there* — not at the end
// of the buffer. `type point` followed by statement its body cannot accept is
// the reported case (BUGS.md B-1): that statement is fbc's `error 19` anchor,
// so the closer belongs on the line above it and everything after stays put.
void CloserFixInsertsAtTheRecordedBoundary() {
  std::string const src = "type point\n"
                          "  x as single\n"
                          "print 1\n";
  AnalyzedDoc const doc = analyze(src);
  QuickFixContext const ctx = bareContext(src, doc);
  CHECK(doc.parse.diagnostics.size() == 1);
  if (doc.parse.diagnostics.size() != 1) {
    return;
  }
  Diagnostic const &d = doc.parse.diagnostics.front();
  CHECK(d.code == "unterminated-block");
  CHECK(d.closerAt.value_or(0) ==
        static_cast<std::uint32_t>(src.find("print")));

  std::vector<QuickFix> const fixes = fixesFor("unterminated-block", d, ctx);
  CHECK(fixes.size() == 1);
  if (fixes.size() != 1) {
    return;
  }
  CHECK(fixes[0].title == "Insert 'END TYPE'");
  // A statement's own first token is a line start, so the insertion needs no
  // leading newline — the closer lands above the statement and the
  // end-of-buffer / blank-line logic never applies.
  CHECK(editText(fixes[0]) == insertAt(src.find("print")) + "END TYPE\n");

  // And the acceptance criterion again, at the boundary: the applied text
  // re-parses without the diagnostic, with the field still a field.
  TextEditBytes const &e = fixes[0].edits.front();
  std::string const applied =
      src.substr(0, e.range.beg) + e.newText + src.substr(e.range.end);
  CHECK(applied == "type point\n  x as single\nEND TYPE\nprint 1\n");
  AnalyzedDoc const after = analyze(applied);
  CHECK(after.parse.diagnostics.empty());
  CHECK(after.parse.roots.size() == 1);
  CHECK(after.parse.roots.front().children.size() == 1);
}

// The reported buffer, verbatim, end to end: the closer is the by-value
// self-reference rule's boundary (`dim p as point` inside `type point` is fbc's
// `error 88`), so the applied fix must both clear the diagnostic and leave the
// module-level `dim p as point` a module-level Dim instead of a field named
// `p` that the record also declares.
void CloserFixClosesTheRecordAboveASelfReferentialField() {
  std::string const src = "type point\n"
                          "    x as single\n"
                          "    y as single\n"
                          "\n"
                          "dim p as point\n"
                          "p.x = 1.5\n"
                          "print p.y\n";
  AnalyzedDoc const doc = analyze(src);
  QuickFixContext const ctx = bareContext(src, doc);
  CHECK(doc.parse.diagnostics.size() == 1);
  if (doc.parse.diagnostics.size() != 1) {
    return;
  }
  std::vector<QuickFix> const fixes =
      fixesFor("unterminated-block", doc.parse.diagnostics.front(), ctx);
  CHECK(fixes.size() == 1);
  if (fixes.size() != 1) {
    return;
  }
  CHECK(fixes[0].title == "Insert 'END TYPE'");
  CHECK(editText(fixes[0]) == insertAt(src.find("dim p")) + "END TYPE\n");

  TextEditBytes const &e = fixes[0].edits.front();
  std::string const applied =
      src.substr(0, e.range.beg) + e.newText + src.substr(e.range.end);
  AnalyzedDoc const after = analyze(applied);
  CHECK(after.parse.diagnostics.empty());
  CHECK(after.parse.roots.size() == 2);
}

// A procedure body accepts every statement, so nothing in the source marks
// where it ends: the parse records no boundary and end-of-buffer is the answer,
// not a fallback. Same for a stale `closerAt` — an offset past the buffer it
// was computed against is evidence about different bytes, so it is ignored
// rather than trusted.
void CloserFixFallsBackToTheBufferEndWithoutEvidence() {
  std::string const src = "sub main()\n  print 1\n";
  AnalyzedDoc const doc = analyze(src);
  QuickFixContext const ctx = bareContext(src, doc);
  CHECK(doc.parse.diagnostics.size() == 1);
  if (doc.parse.diagnostics.size() != 1) {
    return;
  }
  Diagnostic const d = doc.parse.diagnostics.front();
  CHECK(d.code == "unterminated-block");
  CHECK(!d.closerAt.has_value());

  std::vector<QuickFix> const fixes = fixesFor("unterminated-block", d, ctx);
  CHECK(fixes.size() == 1);
  if (fixes.size() == 1) {
    CHECK(editText(fixes[0]) == insertAt(src.size()) + "END SUB\n");
  }

  Diagnostic stale = d;
  stale.closerAt = static_cast<std::uint32_t>(src.size() + 1);
  std::vector<QuickFix> const staleFixes =
      fixesFor("unterminated-block", stale, ctx);
  CHECK(staleFixes.size() == 1);
  if (staleFixes.size() == 1) {
    CHECK(editText(staleFixes[0]) == insertAt(src.size()) + "END SUB\n");
  }
}

// The acceptance criterion, at the seam the fix is a pure function of: the
// text a fix writes, spliced into the buffer the diagnostic came from, must
// re-parse without that diagnostic.
void ApplyingAFixClearsItsDiagnostic() {
  std::string const src = "sub main()\n  print 1\n";
  AnalyzedDoc const doc = analyze(src);
  QuickFixContext const ctx = bareContext(src, doc);
  CHECK(doc.parse.diagnostics.size() == 1);
  if (doc.parse.diagnostics.size() != 1) {
    return;
  }
  std::vector<QuickFix> const fixes =
      fixesFor("unterminated-block", doc.parse.diagnostics.front(), ctx);
  CHECK(fixes.size() == 1);
  if (fixes.size() != 1) {
    return;
  }
  TextEditBytes const &e = fixes[0].edits.front();
  std::string const applied =
      src.substr(0, e.range.beg) + e.newText + src.substr(e.range.end);
  AnalyzedDoc const after = analyze(applied);
  // The inserted closer is uppercase, so this also pins that a keyword closes
  // its block whatever its case — fbc matches keywords case-insensitively.
  CHECK(after.parse.diagnostics.empty());
  CHECK(applied == "sub main()\n  print 1\nEND SUB\n");
}

void CloserFixUsesTheBufferLineEnding() {
  // A CRLF buffer must not collect a lone LF.
  std::string const src = "sub c()\r\n  print 1\r\n";
  AnalyzedDoc const doc = analyze(src);
  QuickFixContext const ctx = bareContext(src, doc);
  std::vector<QuickFix> const fixes =
      fixesFor("unterminated-block", diagAt("unterminated-block", 0, 3), ctx);
  CHECK(fixes.size() == 1);
  if (fixes.size() == 1) {
    CHECK(editText(fixes[0]) == insertAt(src.size()) + "END SUB\r\n");
  }
}

void CloserFixLandsAboveTrailingBlankLines() {
  std::string const src = "while a\n"
                          "  b = 1\n"
                          "\n"
                          "\n";
  AnalyzedDoc const doc = analyze(src);
  QuickFixContext const ctx = bareContext(src, doc);
  std::vector<QuickFix> const fixes =
      fixesFor("unterminated-block", diagAt("unterminated-block", 0, 5), ctx);
  CHECK(fixes.size() == 1);
  if (fixes.size() == 1) {
    // Start of the first blank line — a line start needs no leading newline —
    // so the trailing blank lines stay below the closer.
    std::size_t const firstBlank = src.find("\n\n") + 1;
    CHECK(editText(fixes[0]) == insertAt(firstBlank) + "WEND\n");
  }
}

void CloserFixForPreprocessorBlock() {
  std::string const src = "#ifdef DEBUG\nprint 1\n";
  AnalyzedDoc const doc = analyze(src);
  QuickFixContext const ctx = bareContext(src, doc);
  std::vector<QuickFix> const fixes =
      fixesFor("unterminated-block", diagAt("unterminated-block", 0, 11), ctx);
  CHECK(fixes.size() == 1);
  if (fixes.size() == 1) {
    CHECK(fixes[0].title == "Insert '#ENDIF'");
  }
}

void CloserFixOffersNothingForAnUnknownOpener() {
  std::string const src = "print 1\n";
  AnalyzedDoc const doc = analyze(src);
  QuickFixContext const ctx = bareContext(src, doc);
  // A range that starts no block opener: no closer, so no fix.
  CHECK(fixesFor("unterminated-block", diagAt("unterminated-block", 2, 3), ctx)
            .empty());
  // And an unknown diagnostic code has no provider at all.
  CHECK(quickFixProviderFor("closer-mismatch") == nullptr);
  CHECK(quickFixProviderFor("stray-closer") == nullptr);
  CHECK(quickFixProviderFor("") == nullptr);
}

void IncludeFixRetargetsToAWorkspaceFile() {
  std::string const src = "#include \"config.bai\"\nprint 1\n";
  AnalyzedDoc const doc = analyze(src);
  std::size_t const lit = src.find("config.bai"); // the literal, quotes outside

  std::vector<std::shared_ptr<IndexedFile const>> const files = {
      fileAt("/ws/inc/config.bi"), fileAt("/ws/deep/nested/config.bas")};
  std::filesystem::path const docPath("/ws/main.bas");

  std::set<std::string> offered;
  QuickFixContext ctx = bareContext(src, doc);
  ctx.documentPath = &docPath;
  ctx.workspaceFiles = &files;
  // The seam answers only the literal that would really resolve.
  ctx.resolveInclude =
      [](std::string const &literal) -> std::optional<std::string> {
    if (literal == "inc/config.bi") {
      return std::string("/ws/inc/config.bi");
    }
    return std::nullopt;
  };

  std::vector<QuickFix> const fixes =
      fixesFor("include-not-found",
               diagAt("include-not-found", static_cast<std::uint32_t>(lit),
                      static_cast<std::uint32_t>(lit + 10)),
               ctx);
  // Only the candidate the seam accepts is offered — the nested one would not
  // resolve, so it is never offered.
  CHECK(fixes.size() == 1);
  if (fixes.size() == 1) {
    CHECK(fixes[0].title == "Change include to \"inc/config.bi\"");
    CHECK(fixes[0].code == "include-not-found");
    // The edit replaces the literal only: the quotes are outside the range.
    CHECK(editText(fixes[0]) == std::to_string(lit) + ":" +
                                    std::to_string(lit + 10) +
                                    ">inc/config.bi");
  }
  for (QuickFix const &f : fixes) {
    offered.insert(f.title);
  }
  CHECK(offered.size() == fixes.size());
}

void IncludeFixOffersNothingWithoutACandidate() {
  std::string const src = "#include \"nope.bi\"\n";
  AnalyzedDoc const doc = analyze(src);
  std::size_t const lit = src.find("nope.bi");

  std::vector<std::shared_ptr<IndexedFile const>> const files = {
      fileAt("/ws/other.bi")};
  std::filesystem::path const docPath("/ws/main.bas");
  QuickFixContext ctx = bareContext(src, doc);
  ctx.documentPath = &docPath;
  ctx.workspaceFiles = &files;
  ctx.resolveInclude = [](std::string const &) -> std::optional<std::string> {
    return std::nullopt;
  };

  Diagnostic const d =
      diagAt("include-not-found", static_cast<std::uint32_t>(lit),
             static_cast<std::uint32_t>(lit + 7));
  // Nothing in the workspace fits the literal: no fix, never a guess.
  CHECK(fixesFor("include-not-found", d, ctx).empty());
  // A document served without an index has no candidate source at all.
  CHECK(fixesFor("include-not-found", d, bareContext(src, doc)).empty());
  // Nor is a range that is not a filename literal a fix (a stale client range
  // pointing at a quote or a newline is refused, not edited).
  Diagnostic const quoted =
      diagAt("include-not-found", static_cast<std::uint32_t>(lit - 1),
             static_cast<std::uint32_t>(lit + 7));
  CHECK(fixesFor("include-not-found", quoted, ctx).empty());
}

void IncludeFixOrdersShallowestFirst() {
  std::string const src = "#include \"util.bi\"\n";
  AnalyzedDoc const doc = analyze(src);
  std::size_t const lit = src.find("util.bi");

  std::vector<std::shared_ptr<IndexedFile const>> const files = {
      fileAt("/ws/a/b/c/util.bi"), fileAt("/ws/inc/util.bi"),
      fileAt("/ws/util.bi")};
  std::filesystem::path const docPath("/ws/src/main.bas");
  QuickFixContext ctx = bareContext(src, doc);
  ctx.documentPath = &docPath;
  ctx.workspaceFiles = &files;
  ctx.resolveInclude =
      [](std::string const &literal) -> std::optional<std::string> {
    return literal;
  };

  std::vector<QuickFix> const fixes =
      fixesFor("include-not-found",
               diagAt("include-not-found", static_cast<std::uint32_t>(lit),
                      static_cast<std::uint32_t>(lit + 7)),
               ctx);
  CHECK(fixes.size() == 3);
  if (fixes.size() == 3) {
    // Relative to the including file, shallowest first.
    CHECK(fixes[0].edits.front().newText == "../util.bi");
    CHECK(fixes[1].edits.front().newText == "../inc/util.bi");
    CHECK(fixes[2].edits.front().newText == "../a/b/c/util.bi");
  }
}

void IncludeFixCapsTheCandidateList() {
  std::string const src = "#include \"cap.bi\"\n";
  AnalyzedDoc const doc = analyze(src);
  std::size_t const lit = src.find("cap.bi");

  std::vector<std::shared_ptr<IndexedFile const>> files;
  for (int i = 0; i < 20; ++i) {
    files.push_back(fileAt("/ws/d" + std::to_string(i) + "/cap.bi"));
  }
  std::filesystem::path const docPath("/ws/main.bas");
  QuickFixContext ctx = bareContext(src, doc);
  ctx.documentPath = &docPath;
  ctx.workspaceFiles = &files;
  ctx.resolveInclude =
      [](std::string const &literal) -> std::optional<std::string> {
    return literal;
  };

  std::vector<QuickFix> const fixes =
      fixesFor("include-not-found",
               diagAt("include-not-found", static_cast<std::uint32_t>(lit),
                      static_cast<std::uint32_t>(lit + 6)),
               ctx);
  CHECK(fixes.size() == 5);
}

void UnresolvedIncludeDiagnosticsMatchTheEdges() {
  std::vector<IncludeEdge> edges(3);
  edges[0].target = "/ws/ok.bi"; // resolved: no diagnostic
  edges[1].literal = "missing.bi";
  edges[1].targetRange = SourceRange{11, 20};
  edges[2].literal = "noliteral.bi";
  edges[2].targetRange = SourceRange{30, 30}; // empty range: skipped

  std::vector<Diagnostic> const diags = unresolvedIncludeDiagnostics(edges, 64);
  CHECK(diags.size() == 1);
  if (diags.size() == 1) {
    CHECK(diags[0].code == "include-not-found");
    CHECK(diags[0].severity == Severity::Error);
    CHECK(diags[0].range.beg == 11 && diags[0].range.end == 20);
    CHECK(diags[0].message == "include file not found: \"missing.bi\"");
  }
  // A literal reaching past the buffer is skipped rather than published with a
  // range the client cannot map.
  edges[2].targetRange = SourceRange{60, 90};
  CHECK(unresolvedIncludeDiagnostics(edges, 64).size() == 1);
}

void RegistryAnswersTheFixableCodes() {
  CHECK(quickFixProviders().size() == 2);
  CHECK(quickFixProviderFor("unterminated-block") != nullptr);
  CHECK(quickFixProviderFor("include-not-found") != nullptr);
  // Every registered code is a real diagnostic code, and the registry is
  // stable across calls (the session serves it from the handler pool).
  CHECK(quickFixProviders().data() == quickFixProviders().data());
}

} // namespace

int main() {
  CloserFixAppendsOneBlockEnd();
  CloserFixInsertsAtTheRecordedBoundary();
  CloserFixClosesTheRecordAboveASelfReferentialField();
  CloserFixFallsBackToTheBufferEndWithoutEvidence();
  ApplyingAFixClearsItsDiagnostic();
  CloserFixUsesTheBufferLineEnding();
  CloserFixLandsAboveTrailingBlankLines();
  CloserFixForPreprocessorBlock();
  CloserFixOffersNothingForAnUnknownOpener();
  IncludeFixRetargetsToAWorkspaceFile();
  IncludeFixOffersNothingWithoutACandidate();
  IncludeFixOrdersShallowestFirst();
  IncludeFixCapsTheCandidateList();
  UnresolvedIncludeDiagnosticsMatchTheEdges();
  RegistryAnswersTheFixableCodes();

  if (failures == 0) {
    std::printf("code_actions_checks: all passed\n");
    return 0;
  }
  std::printf("code_actions_checks: %d failures\n", failures);
  return 1;
}
