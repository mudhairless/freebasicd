/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "session.h"

#include "i18n.h"
#include "index.h"
#include "inlay_hints.h"
#include "language.h"
#include "lexer.h"
#include "parser.h"
#include "resolve.h"
#include "semantic_tokens.h"
#include "symbols.h"
#include "utf16.h"

#include "LibLsp/JsonRpc/json.h"
#include "LibLsp/lsp/LanguageSession.h"
#include "LibLsp/lsp/client/registerCapability.h"
#include "LibLsp/lsp/general/exit.h"
#include "LibLsp/lsp/general/initialize.h"
#include "LibLsp/lsp/general/initialized.h"
#include "LibLsp/lsp/general/lsTextDocumentClientCapabilities.h"
#include "LibLsp/lsp/general/shutdown.h"
#include "LibLsp/lsp/location_type.h"
#include "LibLsp/lsp/lsAny.h"
#include "LibLsp/lsp/lsMarkedString.h"
#include "LibLsp/lsp/lsResponseError.h"
#include "LibLsp/lsp/lsTextDocumentEdit.h"
#include "LibLsp/lsp/lsTextEdit.h"
#include "LibLsp/lsp/lsp_completion.h"
#include "LibLsp/lsp/lsp_diagnostic.h"
#include "LibLsp/lsp/symbol.h"
#include "LibLsp/lsp/textDocument/code_action.h"
#include "LibLsp/lsp/textDocument/completion.h"
#include "LibLsp/lsp/textDocument/declaration_definition.h"
#include "LibLsp/lsp/textDocument/did_change.h"
#include "LibLsp/lsp/textDocument/did_close.h"
#include "LibLsp/lsp/textDocument/did_open.h"
#include "LibLsp/lsp/textDocument/did_save.h"
#include "LibLsp/lsp/textDocument/document_symbol.h"
#include "LibLsp/lsp/textDocument/foldingRange.h"
#include "LibLsp/lsp/textDocument/highlight.h"
#include "LibLsp/lsp/textDocument/hover.h"
#include "LibLsp/lsp/textDocument/prepareRename.h"
#include "LibLsp/lsp/textDocument/publishDiagnostics.h"
#include "LibLsp/lsp/textDocument/references.h"
#include "LibLsp/lsp/textDocument/rename.h"
#include "LibLsp/lsp/textDocument/signature_help.h"
#include "LibLsp/lsp/working_files.h"
#include "LibLsp/lsp/workspace/did_change_configuration.h"
#include "LibLsp/lsp/workspace/did_change_watched_files.h"
#include "LibLsp/lsp/workspace/symbol.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// LSP WatchKind bitmask (didChangeWatchedFiles.h):
// Create=1, Change=2, Delete=4.
#define WATCH_KIND_CREATE 1
#define WATCH_KIND_CHANGE 2
#define WATCH_KIND_DELETE 4

namespace {

// True when `normalizedPath` lies at or under `normalizedRoot` (lexical
// comparison, mirrors WorkspaceIndex::isInsideRoot). Both args already
// normalized by fblang::normalizePath.
bool isWithinNormalized(std::string const &normalizedPath,
                        std::string const &normalizedRoot) {
  if (normalizedPath.size() < normalizedRoot.size()) {
    return false;
  }
  if (normalizedPath.compare(0, normalizedRoot.size(), normalizedRoot) != 0) {
    return false;
  }
  if (normalizedPath.size() == normalizedRoot.size()) {
    return true;
  }
  char const next = normalizedPath[normalizedRoot.size()];
  return next == '/' || next == '\\';
}

// A directory that is its own project root: it holds any version-control
// checkout marker. `.git` is a directory for a regular checkout and a file for
// a worktree; fossil's `.fslckout`/`_FOSSIL_` (and the `.fossil` database) are
// files; the DVCS markers (`.hg`, `.svn`, `.bzr`, `.darcs`, `.pijul`, `_MTN`)
// are directories. `exists` accepts any of the two kinds.
bool isProjectRoot(std::filesystem::path const &dir) {
  static constexpr char const *const kVcsMarkers[] = {
      ".git",     ".hg",     ".svn",   ".bzr",   ".fslckout",
      "_FOSSIL_", ".fossil", ".darcs", ".pijul", "_MTN",
  };
  std::error_code ec;
  for (char const *marker : kVcsMarkers) {
    if (std::filesystem::exists(dir / marker, ec)) {
      return true;
    }
  }
  return false;
}

// Nearest ancestor of `start` (inclusive) at or below `limit` that is a
// project root. `start` itself may live outside `limit` (an opened file in a
// sibling tree); the search then yields nothing, it never widens past `limit`.
std::optional<std::filesystem::path>
nearestProjectRoot(std::filesystem::path start,
                   std::filesystem::path const &limit) {
  std::string const normLimit = fblang::normalizePath(limit);
  for (;;) {
    if (isProjectRoot(start)) {
      return start;
    }
    if (start == limit) {
      return std::nullopt; // reached the client root without any VCS marker
    }
    std::filesystem::path const parent = start.parent_path();
    if (parent == start ||
        !isWithinNormalized(fblang::normalizePath(parent), normLimit)) {
      return std::nullopt; // filesystem root, or the search would leave the
                           // client root
    }
    start = parent;
  }
}

// Nearest ancestor of `start` (inclusive) at or below `limit` holding a
// freebasicd.toml — the config-file root marker. Mirrors
// nearestProjectRoot: never widens past `limit`.
std::optional<std::filesystem::path>
nearestConfigRoot(std::filesystem::path start,
                  std::filesystem::path const &limit) {
  std::string const normLimit = fblang::normalizePath(limit);
  for (;;) {
    if (fblang::hasConfigFile(start)) {
      return start;
    }
    if (start == limit) {
      return std::nullopt; // reached the client root without any config marker
    }
    std::filesystem::path const parent = start.parent_path();
    if (parent == start ||
        !isWithinNormalized(fblang::normalizePath(parent), normLimit)) {
      return std::nullopt; // filesystem root, or the search would leave the
                           // client root
    }
    start = parent;
  }
}

// Unbounded upward walk for single-file mode: the nearest ancestor of `start`
// satisfying `pred`, stopping before the home folder and the drive root. A
// `.git` or `~/freebasicd.toml` at the personal directory must never capture
// every lone file, mirroring the source-layout walk's home guard
// (findSourceLayoutRoot).
std::filesystem::path homeDirectory(); // defined below
template <typename Pred>
std::optional<std::filesystem::path>
nearestMarkerAboveHome(std::filesystem::path start, Pred const &pred) {
  std::filesystem::path const home = homeDirectory();
  for (;;) {
    if (!home.empty() && start == home) {
      return std::nullopt; // home folder: fail path
    }
    std::filesystem::path const parent = start.parent_path();
    if (parent == start) {
      return std::nullopt; // drive root: nothing above
    }
    if (pred(start)) {
      return start;
    }
    start = parent;
  }
}

std::filesystem::path homeDirectory() {
  char const *home = std::getenv("HOME");
  if (home != nullptr && *home != '\0') {
    return home;
  }
  home = std::getenv("USERPROFILE");
  if (home != nullptr && *home != '\0') {
    return home;
  }
  return {};
}

// True when one of `dir`'s immediate children is a directory whose name is in
// the translated source/include layout catalog (language.cpp) — the marker
// that `dir` is a project root laid out as <root>/<src-or-inc>/... . Files
// named src/inc are not markers; only directories count.
bool hasSourceLayoutChild(std::filesystem::path const &dir) {
  std::error_code ec;
  std::filesystem::directory_iterator it(dir, ec);
  std::filesystem::directory_iterator const end;
  for (; !ec && it != end; it.increment(ec)) {
    std::filesystem::path const child = it->path();
    if (!std::filesystem::is_directory(child, ec)) {
      continue;
    }
    std::string const name = fblang::toLowerChars(child.filename().string());
    if (fblang::isSourceDirName(name) || fblang::isIncludeDirName(name)) {
      return true;
    }
  }
  return false;
}

// The deepest ancestor of `start` owning a source/include layout child, or
// nothing once the walk reaches the drive root or the home folder. The layout
// rule is the fallback when no version-control marker exists anywhere on the
// walk: `.../inner/src/file.bas` roots at `.../inner` (the first parent with a
// `src` child). The home folder ends the walk without checking it, so a
// personal `~/src` never swallows every project under the home directory.
std::optional<std::filesystem::path>
findSourceLayoutRoot(std::filesystem::path start) {
  std::filesystem::path const home = homeDirectory();
  for (;;) {
    std::filesystem::path const parent = start.parent_path();
    if (parent == start) {
      return std::nullopt; // drive root: nothing above
    }
    if (!home.empty() && start == home) {
      return std::nullopt; // home folder: fail path
    }
    if (hasSourceLayoutChild(start)) {
      return start;
    }
    start = parent;
  }
}

lsSymbolKind toLspSymbolKind(fblang::SymbolKind kind) {
  switch (kind) {
  case fblang::SymbolKind::Sub:
    return lsSymbolKind::Method;
  case fblang::SymbolKind::Function:
    return lsSymbolKind::Function;
  case fblang::SymbolKind::Property:
    return lsSymbolKind::Property;
  case fblang::SymbolKind::Constructor:
    return lsSymbolKind::Constructor;
  case fblang::SymbolKind::Destructor:
    return lsSymbolKind::Method;
  case fblang::SymbolKind::Operator:
    return lsSymbolKind::Operator;
  case fblang::SymbolKind::Type:
  case fblang::SymbolKind::Union:
    return lsSymbolKind::Struct;
  case fblang::SymbolKind::Enum:
    return lsSymbolKind::Enum;
  case fblang::SymbolKind::Namespace:
    return lsSymbolKind::Namespace;
  case fblang::SymbolKind::Const:
    return lsSymbolKind::Constant;
  case fblang::SymbolKind::Dim:
    return lsSymbolKind::Variable;
  case fblang::SymbolKind::Parameter:
    return lsSymbolKind::Parameter;
  case fblang::SymbolKind::Variable:
    return lsSymbolKind::Variable;
  case fblang::SymbolKind::Scope:
  case fblang::SymbolKind::Label:
    return lsSymbolKind::Unknown;
  }
  return lsSymbolKind::Unknown;
}

// Scope blocks are noise in an outline; recurse but don't emit them.
lsDocumentSymbol convertSymbol(std::string_view content,
                               fblang::Symbol const &s) {
  lsDocumentSymbol out;
  out.name = s.name;
  out.kind = toLspSymbolKind(s.kind);
  out.range = fblang::utf16Range(content, s.range.beg, s.range.end);
  out.selectionRange =
      fblang::utf16Range(content, s.selection.beg, s.selection.end);
  if (!s.signature.empty()) {
    out.detail.emplace(s.signature);
  }
  for (auto const &child : s.children) {
    if (child.kind == fblang::SymbolKind::Scope) {
      continue;
    }
    if (!out.children) {
      out.children.emplace();
    }
    out.children->push_back(convertSymbol(content, child));
  }
  return out;
}

// One language-layer diagnostic as the protocol carries it. The byte-offset
// range becomes the UTF-16 range of the document it was reported against, and
// the `code` rides along because M12's quick fixes key on it (and because a
// client groups its lightbulb entries by code).
lsDiagnostic toLsDiagnostic(std::string_view content,
                            fblang::Diagnostic const &d) {
  lsDiagnostic diag;
  diag.range = fblang::utf16Range(content, d.range.beg, d.range.end);
  diag.severity = static_cast<lsDiagnosticSeverity>(d.severity);
  if (!d.code.empty()) {
    diag.code.emplace(
        std::make_pair<optional<std::string>, optional<int>>(d.code, {}));
  }
  diag.source.emplace("freebasicd");
  diag.message = d.message;
  return diag;
}

std::vector<lsDiagnostic> convertDiagnostics(std::string_view content,
                                             fblang::ParseResult const &parse) {
  std::vector<lsDiagnostic> out;
  out.reserve(parse.diagnostics.size());
  for (auto const &d : parse.diagnostics) {
    out.push_back(toLsDiagnostic(content, d));
  }
  return out;
}

// Unresolved `#include`/`#include once` literals of an indexed entry become
// `include-not-found` Errors at the literal's own range. Only the open
// buffer's own edges are diagnosed (M6); inter-file closure diagnostics wait
// for pull diagnostics (M14). The diagnostics are built by the code-action
// module (M12) so a quick fix keyed on the code addresses exactly the range
// the publish reported.
void appendIncludeDiagnostics(std::string_view content,
                              fblang::IndexedFile const &entry,
                              std::vector<lsDiagnostic> *out) {
  for (fblang::Diagnostic const &d :
       fblang::unresolvedIncludeDiagnostics(entry.includes, content.size())) {
    out->push_back(toLsDiagnostic(content, d));
  }
}

// LSP 3.17 CodeActionKind matching: a `context.only` entry selects actions of
// that kind, and a dot-separated ancestor selects its descendants
// ("quickfix" serves "quickfix" and "quickfix.something", but not
// "quickfixes"). The kind must be a segment-aligned prefix, never a bare
// string prefix.
bool kindRequested(std::vector<std::string> const &only,
                   std::string const &kind) {
  for (std::string const &filter : only) {
    if (filter == kind) {
      return true;
    }
    if (filter.size() < kind.size() && kind[filter.size()] == '.' &&
        kind.compare(0, filter.size(), filter) == 0) {
      return true;
    }
  }
  return false;
}

// One quick fix as the client consumes it: a `CodeAction` carrying the kind we
// advertise, the diagnostic it answers, and a single-file WorkspaceEdit keyed
// by the request's own URI. A CodeAction with an `edit` is the shape the
// protocol defines for a server-side fix — the client applies the edit and the
// re-parse clears the diagnostic. A `Command` cannot express this: a Command is
// an id the client executes, and there is no standard id that means "apply this
// edit", so a fix shipped that way shows up in the menu and then does nothing.
CodeAction quickFixCodeAction(std::string const &uri, std::string_view content,
                              fblang::Diagnostic const &d,
                              fblang::QuickFix const &fix) {
  CodeAction action;
  action.title = fix.title;
  action.kind = std::string("quickfix");
  action.diagnostics.emplace();
  action.diagnostics->push_back(toLsDiagnostic(content, d));

  lsWorkspaceEdit edit;
  edit.changes.emplace();
  std::vector<lsTextEdit> edits;
  edits.reserve(fix.edits.size());
  for (fblang::TextEditBytes const &e : fix.edits) {
    lsTextEdit te;
    te.range = fblang::utf16Range(content, e.range.beg, e.range.end);
    te.newText = e.newText;
    edits.push_back(std::move(te));
  }
  (*edit.changes)[uri] = std::move(edits);
  action.edit.emplace(std::move(edit));
  return action;
}

// The diagnostics a `textDocument/codeAction` request should answer for: what
// the next publish would report inside `range` (parse diagnostics plus the
// document's unresolved includes), plus whatever the client listed in its
// context. Deriving our own set is what lets a client that sends an empty
// context — or sends one for a diagnostic we no longer publish — still get an
// answer.
std::vector<fblang::Diagnostic>
requestedDiagnostics(fblang::AnalyzedDoc const &doc, std::string_view content,
                     lsRange const &range,
                     std::vector<fblang::Diagnostic> const &published,
                     std::vector<lsDiagnostic> const &fromClient) {
  std::uint32_t const beg =
      fblang::byteOffsetForUtf16Position(content, range.start);
  std::uint32_t const end =
      fblang::byteOffsetForUtf16Position(content, range.end);
  auto inRange = [beg, end](fblang::SourceRange const &r) {
    // A zero-width request is a cursor: the diagnostic must cover the point.
    return r.beg <= end && r.end >= beg;
  };

  std::vector<fblang::Diagnostic> out;
  auto add = [&out](fblang::Diagnostic d) {
    for (fblang::Diagnostic const &seen : out) {
      if (seen.code == d.code && seen.range.beg == d.range.beg) {
        return; // the client restating a diagnostic we already have
      }
    }
    out.push_back(std::move(d));
  };
  for (fblang::Diagnostic const &d : published) {
    if (inRange(d.range)) {
      add(d);
    }
  }
  for (lsDiagnostic const &cd : fromClient) {
    if (!cd.code || !cd.code->first) {
      continue; // a code-less diagnostic names no fix
    }
    fblang::Diagnostic d;
    d.range.beg = fblang::byteOffsetForUtf16Position(content, cd.range.start);
    d.range.end = fblang::byteOffsetForUtf16Position(content, cd.range.end);
    d.severity = cd.severity ? static_cast<fblang::Severity>(*cd.severity)
                             : fblang::Severity::Error;
    d.code = *cd.code->first;
    add(std::move(d));
  }
  return out;
}

// Deepest symbol (by range nesting) covering `off`, or nullptr.
fblang::Symbol const *symbolAt(fblang::Symbol const &sym, std::uint32_t off) {
  if (off < sym.range.beg || off > sym.range.end) {
    return nullptr;
  }
  for (auto const &c : sym.children) {
    if (fblang::Symbol const *hit = symbolAt(c, off)) {
      return hit;
    }
  }
  return &sym;
}

fblang::Symbol const *deepestSymbolAt(std::vector<fblang::Symbol> const &roots,
                                      std::uint32_t off) {
  fblang::Symbol const *best = nullptr;
  for (auto const &r : roots) {
    if (fblang::Symbol const *hit = symbolAt(r, off)) {
      best = hit;
    }
  }
  return best;
}

lsCompletionItemKind completionKindFor(fblang::SymbolKind kind) {
  switch (kind) {
  case fblang::SymbolKind::Sub:
    return lsCompletionItemKind::Method;
  case fblang::SymbolKind::Function:
    return lsCompletionItemKind::Function;
  case fblang::SymbolKind::Property:
    return lsCompletionItemKind::Property;
  case fblang::SymbolKind::Constructor:
    return lsCompletionItemKind::Constructor;
  case fblang::SymbolKind::Destructor:
  case fblang::SymbolKind::Operator:
    return lsCompletionItemKind::Operator;
  case fblang::SymbolKind::Type:
  case fblang::SymbolKind::Union:
    return lsCompletionItemKind::Struct;
  case fblang::SymbolKind::Enum:
    return lsCompletionItemKind::Enum;
  case fblang::SymbolKind::Namespace:
    return lsCompletionItemKind::Module;
  case fblang::SymbolKind::Const:
    return lsCompletionItemKind::Constant;
  case fblang::SymbolKind::Dim:
  case fblang::SymbolKind::Parameter:
  case fblang::SymbolKind::Variable:
    return lsCompletionItemKind::Variable;
  case fblang::SymbolKind::Scope:
  case fblang::SymbolKind::Label:
    return lsCompletionItemKind::Text;
  }
  return lsCompletionItemKind::Text;
}

// Identifier character: letters, digits, underscore, or a type suffix.
bool isWordChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || fblang::isSuffixChar(c);
}

bool isProcKind(fblang::SymbolKind kind) {
  switch (kind) {
  case fblang::SymbolKind::Sub:
  case fblang::SymbolKind::Function:
  case fblang::SymbolKind::Property:
  case fblang::SymbolKind::Constructor:
  case fblang::SymbolKind::Destructor:
  case fblang::SymbolKind::Operator:
    return true;
  default:
    return false;
  }
}

char const *procKindName(fblang::SymbolKind kind) {
  switch (kind) {
  case fblang::SymbolKind::Sub:
    return "Sub";
  case fblang::SymbolKind::Function:
    return "Function";
  case fblang::SymbolKind::Property:
    return "Property";
  case fblang::SymbolKind::Constructor:
    return "Constructor";
  case fblang::SymbolKind::Destructor:
    return "Destructor";
  case fblang::SymbolKind::Operator:
    return "Operator";
  default:
    return "procedure";
  }
}

// Second paragraph of a declaration hover: what the symbol is and where it
// lives. Walks `decl`'s ancestors (via `roots`, which must own `decl`) to find
// the enclosing procedure and the declaration-scope blocks (for/if/with/...),
// so a usage hover reads like "Local variable in Sub X, inside the `for`
// block" instead of the bare declaration line.
std::string symbolKindLine(fblang::Symbol const *decl,
                           std::vector<fblang::Symbol> const &roots) {
  fblang::Symbol const *const parent = fblang::parentOf(roots, decl);
  fblang::Symbol const *proc = nullptr;
  std::vector<std::string> blocks; // innermost first
  for (fblang::Symbol const *cur = parent; cur != nullptr;
       cur = fblang::parentOf(roots, cur)) {
    if (isProcKind(cur->kind) && proc == nullptr) {
      proc = cur;
    }
    if (cur->kind == fblang::SymbolKind::Scope) {
      blocks.push_back(cur->name);
    }
  }
  auto blockSuffix = [&]() -> std::string {
    return blocks.empty() ? std::string()
                          : ", inside the `" + blocks.front() + "` block";
  };
  switch (decl->kind) {
  case fblang::SymbolKind::Dim:
    if (decl->loopVar) {
      // Counter declared in the header (`for i as integer = ...`), local to
      // its own loop: the `for` block is implied, so no block suffix.
      return proc != nullptr
                 ? "Loop counter in " + std::string(procKindName(proc->kind)) +
                       " `" + proc->name + "`."
                 : "Loop counter.";
    }
    if (proc != nullptr) {
      return "Local variable in " + std::string(procKindName(proc->kind)) +
             " `" + proc->name + "`" + blockSuffix() + ".";
    }
    return "Module-level variable" +
           (decl->shared ? std::string(" — `Shared` (visible inside "
                                       "procedures)")
                         : std::string()) +
           blockSuffix() + ".";
  case fblang::SymbolKind::Const:
    if (parent != nullptr && parent->kind == fblang::SymbolKind::Enum) {
      return "Enum member of `" + parent->name + "`.";
    }
    if (proc != nullptr) {
      return "Local constant in " + std::string(procKindName(proc->kind)) +
             " `" + proc->name + "`" + blockSuffix() + ".";
    }
    return "Module-level constant" + blockSuffix() + ".";
  case fblang::SymbolKind::Parameter:
    if (proc != nullptr) {
      return "Parameter of " + std::string(procKindName(proc->kind)) + " `" +
             proc->name + "`.";
    }
    return "Parameter.";
  case fblang::SymbolKind::Variable:
    if (parent != nullptr && parent->kind != fblang::SymbolKind::Scope) {
      return "Field of type `" + parent->name + "`.";
    }
    return "Field.";
  case fblang::SymbolKind::Label:
    return "Line label.";
  case fblang::SymbolKind::Type:
    return "User-defined type.";
  case fblang::SymbolKind::Union:
    return "Union.";
  case fblang::SymbolKind::Enum:
    return "Enumeration.";
  case fblang::SymbolKind::Namespace:
    return "Namespace.";
  case fblang::SymbolKind::Sub:
  case fblang::SymbolKind::Function:
  case fblang::SymbolKind::Property:
  case fblang::SymbolKind::Constructor:
  case fblang::SymbolKind::Destructor:
  case fblang::SymbolKind::Operator:
    return std::string(procKindName(decl->kind)) + ".";
  case fblang::SymbolKind::Scope:
    return "Declaration scope.";
  }
  return "Declaration.";
}

// A valid FreeBASIC identifier (a rename target must lex as a single
// identifier token): a letter or underscore followed by letters/digits/
// underscores, plus an optional trailing type-suffix char — and never a
// reserved keyword (PRINT, END, ... — a keyword base plus suffix also fails,
// mirroring the lexer) and never a bare `_` (that is a line-continuation
// symbol).
bool isValidIdentifier(std::string_view name) {
  if (name.empty()) {
    return false;
  }
  std::size_t const baseLen =
      name.size() - (fblang::isSuffixChar(name.back()) ? 1U : 0U);
  if (baseLen == 0 || (name[0] != '_' && (name[0] < 'a' || name[0] > 'z') &&
                       (name[0] < 'A' || name[0] > 'Z'))) {
    return false;
  }
  if (baseLen == 1 && name[0] == '_') {
    return false; // a lone `_` is the line-continuation symbol
  }
  for (std::size_t i = 0; i < baseLen; ++i) {
    char const c = name[i];
    bool const ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_';
    if (!ok) {
      return false;
    }
  }
  return !fblang::isReservedWord(name.substr(0, baseLen));
}

// Identifier being typed at `off` (bytes), or "" when the cursor is not on an
// identifier character run.
std::string completionPrefix(std::string_view content, std::uint32_t off) {
  std::size_t start = off;
  while (start > 0 && isWordChar(content[start - 1])) {
    --start;
  }
  return std::string(content.substr(start, off - start));
}

bool hasPrefix(std::string_view word, std::string_view prefix) {
  return word.size() >= prefix.size() &&
         word.substr(0, prefix.size()) == prefix;
}

char const *const kBlockOpeners[] = {
    "sub",    "function", "property", "operator",  "constructor", "destructor",
    "type",   "union",    "enum",     "namespace", "scope",       "if",
    "select", "with",     "extern",   "asm"};

// Two flat token arrays (5 ints per token) are equal at token index `a`
// (into `x`) and `b` (into `y`).
bool tokenEqual(std::vector<std::int32_t> const &x, std::size_t a,
                std::vector<std::int32_t> const &y, std::size_t b) {
  return std::equal(x.begin() + static_cast<std::ptrdiff_t>(a * 5),
                    x.begin() + static_cast<std::ptrdiff_t>(a * 5 + 5),
                    y.begin() + static_cast<std::ptrdiff_t>(b * 5));
}

// Single semantic-tokens edit from a common-prefix/suffix trim of two flat
// arrays. LSP's SemanticTokensEdit indexes the flat `data` array in element
// units (five integers per token).
SemanticTokensEdit diffTokenData(std::vector<std::int32_t> const &previous,
                                 std::vector<std::int32_t> const &current) {
  std::size_t const prevTokens = previous.size() / 5;
  std::size_t const curTokens = current.size() / 5;

  std::size_t prefix = 0;
  while (prefix < prevTokens && prefix < curTokens &&
         tokenEqual(previous, prefix, current, prefix)) {
    ++prefix;
  }
  std::size_t suffix = 0;
  while (suffix < prevTokens - prefix && suffix < curTokens - prefix &&
         tokenEqual(previous, prevTokens - 1 - suffix, current,
                    curTokens - 1 - suffix)) {
    ++suffix;
  }

  SemanticTokensEdit edit;
  edit.start = static_cast<unsigned>(prefix * 5);
  edit.deleteCount = static_cast<unsigned>((prevTokens - prefix - suffix) * 5);
  edit.data.assign(current.begin() + static_cast<std::ptrdiff_t>(prefix * 5),
                   current.end() - static_cast<std::ptrdiff_t>(suffix * 5));
  return edit;
}

} // namespace

FreeBasicServer::FreeBasicServer(lsp::LanguageSession &session)
    : session_(session) {}

void FreeBasicServer::setExitHandler(std::function<void()> exitHandler) {
  exitHandler_ = std::move(exitHandler);
}

// fblang::settingsForDir with parse-failure logging: a missing or
// comment-only freebasicd.toml is normal (defaults, nothing logged); a
// present-but-malformed one also keeps the defaults but says so on stderr
// instead of silently treating a broken config as defaults.
fblang::Settings settingsForDirLogged(std::filesystem::path const &dir) {
  bool ok = true;
  fblang::Settings const s = fblang::settingsForDir(dir, &ok);
  if (!ok) {
    (void)std::fprintf(
        stderr, "[freebasicd] %s\n",
        fblang::trf("%s/freebasicd.toml is not valid TOML; keeping defaults",
                    fblang::normalizePath(dir))
            .c_str());
  }
  return s;
}

void FreeBasicServer::ensureWorkspaceIndex(std::filesystem::path const &root) {
  if (root.empty()) {
    return;
  }
  std::string const normRoot = fblang::normalizePath(root);
  (void)std::fprintf(stderr, "[freebasicd] %s\n",
                     fblang::trf("workspace root: %s", normRoot).c_str());
  std::lock_guard<std::mutex> const lock(indexesMutex_);
  if (indexes_.find(normRoot) != indexes_.end()) {
    return;
  }
  auto index = std::make_shared<fblang::WorkspaceIndex>(root);
  index->open();
  index->applySettings(settingsForDirLogged(root));
  index->scan(true);
  indexes_[normRoot] = std::move(index);
}

// stderr note when the server set a root it *found* rather than the one the
// client passed (or, with no client root, the file's own directory): say how
// the root was identified so the choice reads as a decision. Silent when the
// chosen root is the client's as-is, which ensureWorkspaceIndex already logs.
void logDetectedRoot(FreeBasicServer::IndexRootChoice const &choice,
                     std::filesystem::path const &clientRoot) {
  std::string how;
  switch (choice.reason) {
  case FreeBasicServer::IndexRootChoice::Reason::VcsMarker:
    how = fblang::tr("version-control marker");
    break;
  case FreeBasicServer::IndexRootChoice::Reason::ConfigFile:
    how = fblang::trf("config file %s", fblang::kConfigFileName);
    break;
  case FreeBasicServer::IndexRootChoice::Reason::SourceLayout:
    how = fblang::tr("source/include directory");
    break;
  default:
    return; // ClientRoot / RegisteredRoot / SingleFile: not a found root
  }
  std::string const normRoot = fblang::normalizePath(choice.root);
  if (clientRoot.empty()) {
    (void)std::fprintf(
        stderr, "[freebasicd] %s\n",
        fblang::trf("workspace root %s (detected via %s; no client root)",
                    normRoot, how)
            .c_str());
    return;
  }
  (void)std::fprintf(
      stderr, "[freebasicd] %s\n",
      fblang::trf("workspace root %s (detected via %s; client root %s)",
                  normRoot, how, fblang::normalizePath(clientRoot))
          .c_str());
}

FreeBasicServer::IndexRootChoice
FreeBasicServer::chooseIndexRoot(std::filesystem::path const &openedFile) {
  std::string const normFile = fblang::normalizePath(openedFile);

  // Priority 0: the deepest registered workspace-folder root that contains
  // the file (multi-folder isolation). A file under a registered marker root
  // is served by exactly that root's index, never a sibling folder's.
  if (std::optional<std::filesystem::path> const registered =
          registeredFolderRootContaining(normFile)) {
    return {*registered, IndexRootChoice::Reason::RegisteredRoot};
  }

  // The applicable client root for this file: the deepest registered folder
  // containing it, else the session root when it contains the file, else none
  // (a file outside every registered folder is served single-document style —
  // multi-folder clients must never bleed one folder's scope into another).
  std::optional<std::filesystem::path> clientRoot;
  std::filesystem::path sessionRoot;
  {
    std::lock_guard<std::mutex> const lock(indexesMutex_);
    sessionRoot = sessionRoot_;
    for (std::filesystem::path const &folder : workspaceFolders_) {
      std::string const norm = fblang::normalizePath(folder);
      if (isWithinNormalized(normFile, norm)) {
        if (!clientRoot ||
            norm.size() > fblang::normalizePath(*clientRoot).size()) {
          clientRoot = folder;
        }
      }
    }
    if (!clientRoot && !sessionRoot_.empty() &&
        isWithinNormalized(normFile, fblang::normalizePath(sessionRoot_))) {
      clientRoot = sessionRoot_;
    }
  }

  if (clientRoot) {
    // Priority 1: a client root that is itself a workspace root (a
    // version-control marker or a config file) is used as-is.
    if (isProjectRoot(*clientRoot) || fblang::hasConfigFile(*clientRoot)) {
      return {*clientRoot, IndexRootChoice::Reason::ClientRoot};
    }
    // A broad client root (no marker of its own, e.g. an editor reporting the
    // home directory as the workspace) is narrowed to the opened document's
    // project, so sibling FreeBASIC projects under it are never swept into
    // the index: first by the nearest version-control marker, then by the
    // nearest config file, then — when none exists between the file and the
    // client root — by walking up to the drive root / home folder for a
    // parent holding a source/include directory (the project's own layout
    // names it).
    if (std::optional<std::filesystem::path> const project =
            nearestProjectRoot(openedFile, *clientRoot)) {
      return {*project, IndexRootChoice::Reason::VcsMarker};
    }
    if (std::optional<std::filesystem::path> const config =
            nearestConfigRoot(openedFile, *clientRoot)) {
      return {*config, IndexRootChoice::Reason::ConfigFile};
    }
    if (std::optional<std::filesystem::path> const project =
            findSourceLayoutRoot(openedFile.parent_path())) {
      return {*project, IndexRootChoice::Reason::SourceLayout};
    }
    return {*clientRoot, IndexRootChoice::Reason::ClientRoot};
  }
  // No applicable client root for this file — it lives outside every
  // registered folder and the session root — but a client root exists. Serve
  // the document single-document style through the primary/session index's
  // on-demand closure (never an index of its own, or its symbols would surface
  // in workspace/symbol). True single-file mode (no client root at all) falls
  // through to the marker/config/layout walk below.
  if (!sessionRoot.empty()) {
    return {sessionRoot, IndexRootChoice::Reason::ClientRoot};
  }
  // No client root: single-file mode. The project is named by the nearest
  // version-control marker, then by the nearest config file, then by a
  // source/include directory in an ancestor; otherwise the workspace is the
  // file's directory.
  if (std::optional<std::filesystem::path> const project =
          nearestMarkerAboveHome(openedFile.parent_path(), isProjectRoot)) {
    return {*project, IndexRootChoice::Reason::VcsMarker};
  }
  if (std::optional<std::filesystem::path> const config =
          nearestMarkerAboveHome(openedFile.parent_path(),
                                 fblang::hasConfigFile)) {
    return {*config, IndexRootChoice::Reason::ConfigFile};
  }
  if (std::optional<std::filesystem::path> const project =
          findSourceLayoutRoot(openedFile.parent_path())) {
    return {*project, IndexRootChoice::Reason::SourceLayout};
  }
  return {openedFile.parent_path(), IndexRootChoice::Reason::SingleFile};
}

void FreeBasicServer::registerHandlers() {
  session_.on(
      [this](td_initialize::request const &req) { return onInitialize(req); });
  session_.on(
      [this](td_shutdown::request const &req) { return onShutdown(req); });
  session_.on([this](Notify_Exit::notify const &) {
    if (exitHandler_) {
      exitHandler_();
    }
  });
  session_.on([this](Notify_InitializedNotification::notify const &notify) {
    onInitialized(notify);
  });
  session_.on(
      [this](Notify_WorkspaceDidChangeWatchedFiles::notify const &notify) {
        onWatchedFiles(notify);
      });
  session_.on(
      [this](Notify_WorkspaceDidChangeWorkspaceFolders::notify const &notify) {
        onWorkspaceFoldersChanged(notify);
      });
  session_.on(
      [this](Notify_WorkspaceDidChangeConfiguration::notify const &notify) {
        onDidChangeConfiguration(notify);
      });
  session_.on([this](Notify_TextDocumentDidOpen::notify &notify) {
    onDidOpen(notify);
  });
  session_.on([this](Notify_TextDocumentDidChange::notify const &notify) {
    onDidChange(notify);
  });
  session_.on([this](Notify_TextDocumentDidSave::notify const &notify) {
    onDidSave(notify);
  });
  session_.on([this](Notify_TextDocumentDidClose::notify const &notify) {
    onDidClose(notify);
  });
  session_.on(
      [this](td_symbol::request const &req) { return onDocumentSymbol(req); });
  session_.on([this](td_hover::request const &req) { return onHover(req); });
  session_.on([this](td_foldingRange::request const &req) {
    return onFoldingRange(req);
  });
  session_.on(
      [this](td_definition::request const &req) { return onDefinition(req); });
  session_.on(
      [this](td_references::request const &req) { return onReferences(req); });
  session_.on(
      [this](td_highlight::request const &req) { return onHighlight(req); });
  session_.on(
      [this](td_completion::request const &req) { return onCompletion(req); });
  session_.on([this](td_signatureHelp::request const &req) {
    return onSignatureHelp(req);
  });
  session_.on([this](td_prepareRename::request const &req) {
    return onPrepareRename(req);
  });
  session_.on([this](td_rename::request const &req) { return onRename(req); });
  session_.on(
      [this](wp_symbol::request const &req) { return onWorkspaceSymbol(req); });
  session_.on(
      [this](td_codeAction::request const &req) { return onCodeAction(req); });
  session_.on([this](td_semanticTokens_full::request const &req) {
    return onSemanticTokensFull(req);
  });
  session_.on([this](td_semanticTokens_full_delta::request const &req) {
    return onSemanticTokensDelta(req);
  });
  session_.on([this](td_semanticTokens_range::request const &req) {
    return onSemanticTokensRange(req);
  });
  session_.on(
      [this](td_inlayHint::request const &req) { return onInlayHint(req); });

  // The server->client client/registerCapability request is sent from the
  // `initialized` handler, after the parse/notification pools are running;
  // per RemoteEndPoint its response parser must be in place before
  // startProcessingMessages().
  session_.endpoint()
      .registerResponseParser<Req_ClientRegisterCapability::request>();
}

td_initialize::response
FreeBasicServer::onInitialize(td_initialize::request const &req) {
  td_initialize::response rsp;
  rsp.id = req.id;

  // The client's UI locale (LSP 3.16+, `ClientCapabilities.general.locale`)
  // selects the message catalog when the OS can install the tag; otherwise the
  // environment locale from initI18n() keeps serving.
  if (req.params.locale) {
    fblang::setClientLocale(*req.params.locale);
  }

  lsTextDocumentSyncOptions &sync =
      rsp.result.capabilities.textDocumentSync.emplace().second.emplace();
  sync.openClose = true;
  sync.change = lsTextDocumentSyncKind::Incremental;

  rsp.result.capabilities.documentSymbolProvider.emplace();
  rsp.result.capabilities.documentSymbolProvider->first.emplace(true);

  rsp.result.capabilities.hoverProvider.emplace(true);

  rsp.result.capabilities.foldingRangeProvider.emplace();
  rsp.result.capabilities.foldingRangeProvider->first.emplace(true);

  rsp.result.capabilities.definitionProvider.emplace();
  rsp.result.capabilities.definitionProvider->first.emplace(true);

  rsp.result.capabilities.referencesProvider.emplace();
  rsp.result.capabilities.referencesProvider->first.emplace(true);

  rsp.result.capabilities.documentHighlightProvider.emplace();
  rsp.result.capabilities.documentHighlightProvider->first.emplace(true);

  // renameProvider carries RenameOptions (not the bare-bool Either arm) so
  // prepareProvider=true reaches the client; serializing the bool first arm
  // would drop the options entirely.
  rsp.result.capabilities.renameProvider.emplace();
  rsp.result.capabilities.renameProvider->second.emplace();
  rsp.result.capabilities.renameProvider->second->prepareProvider.emplace(true);

  rsp.result.capabilities.completionProvider.emplace();
  rsp.result.capabilities.completionProvider->triggerCharacters.emplace();
  rsp.result.capabilities.completionProvider->triggerCharacters->emplace_back(
      ".");

  rsp.result.capabilities.signatureHelpProvider.emplace();
  rsp.result.capabilities.signatureHelpProvider->triggerCharacters.emplace_back(
      "(");
  rsp.result.capabilities.signatureHelpProvider->triggerCharacters.emplace_back(
      ",");

  rsp.result.capabilities.workspaceSymbolProvider.emplace();
  rsp.result.capabilities.workspaceSymbolProvider->first.emplace(true);

  // Code actions: quickfix only (M12). The options arm carries the kind list;
  // there is no codeAction/resolve request, so no resolveProvider.
  rsp.result.capabilities.codeActionProvider.emplace();
  rsp.result.capabilities.codeActionProvider->second.emplace();
  rsp.result.capabilities.codeActionProvider->second->codeActionKinds
      .emplace_back("quickfix");

  // Semantic tokens: the legend always; full + delta always; the viewport
  // `range` provider only when the client asks for it (clients that don't fall
  // back to `full`). SemanticTokensServerFull.delta defaults to false, so the
  // flag MUST be set or full/delta serializes as unsupported and clients never
  // send the delta request.
  rsp.result.capabilities.semanticTokensProvider.emplace();
  rsp.result.capabilities.semanticTokensProvider->legend.tokenTypes =
      fblang::semanticTokenTypes();
  rsp.result.capabilities.semanticTokensProvider->legend.tokenModifiers =
      fblang::semanticTokenModifiers();
  rsp.result.capabilities.semanticTokensProvider->full.emplace();
  rsp.result.capabilities.semanticTokensProvider->full->second.emplace();
  rsp.result.capabilities.semanticTokensProvider->full->second->delta = true;
  bool const rangeRequested =
      req.params.capabilities.textDocument &&
      req.params.capabilities.textDocument->semanticTokens &&
      req.params.capabilities.textDocument->semanticTokens->requests.range;
  if (rangeRequested) {
    // Bool arm true; no options payload needed — serializes `"range": true`.
    rsp.result.capabilities.semanticTokensProvider->range.emplace();
    rsp.result.capabilities.semanticTokensProvider->range->first.emplace(true);
  }

  // Inlay hints: options arm (like renameProvider), resolveProvider unset —
  // no resolve request is advertised or handled.
  rsp.result.capabilities.inlayHintProvider.emplace();
  rsp.result.capabilities.inlayHintProvider->second.emplace();

  // Workspace-level capabilities: folder support + change notifications are
  // advertised unconditionally, so a multi-folder client gets one index per
  // registered workspace root; the static watched-file watchers follow when
  // the client did not opt into dynamic registration (the dynamic path
  // registers the same watcher on `initialized`).
  rsp.result.capabilities.workspace.emplace();
  rsp.result.capabilities.workspace->workspaceFolders.supported.emplace(true);
  rsp.result.capabilities.workspace->workspaceFolders.changeNotifications
      .emplace();
  rsp.result.capabilities.workspace->workspaceFolders.changeNotifications
      ->second.emplace(true);
  watchedFilesDynamic_ =
      req.params.capabilities.workspace &&
      req.params.capabilities.workspace->didChangeWatchedFiles &&
      req.params.capabilities.workspace->didChangeWatchedFiles
          ->dynamicRegistration &&
      *req.params.capabilities.workspace->didChangeWatchedFiles
           ->dynamicRegistration;
  if (!watchedFilesDynamic_) {
    lsFileSystemWatcher watcher;
    watcher.globPattern = "**/*.{bas,bi}";
    watcher.kind.emplace(WATCH_KIND_CREATE | WATCH_KIND_CHANGE |
                         WATCH_KIND_DELETE);
    rsp.result.capabilities.workspace->didChangeWatchedFiles.emplace();
    rsp.result.capabilities.workspace->didChangeWatchedFiles->watchers
        .push_back(std::move(watcher));
  }

  // Workspace folders: every registered folder is recorded; folders that are
  // themselves workspace roots (a version-control marker or a
  // freebasicd.toml) get their own index right away, so files opened under
  // them are served without waiting for a didOpen. Broad folders defer to
  // per-document detection (chooseIndexRoot). workspaceFolderRoots_ feeds the
  // priority-0 containment lookup above.
  if (req.params.workspaceFolders) {
    for (WorkspaceFolder const &folder : *req.params.workspaceFolders) {
      std::filesystem::path const folderPath =
          folder.uri.GetAbsolutePath().path();
      bool marker = false;
      {
        std::lock_guard<std::mutex> const lock(indexesMutex_);
        workspaceFolders_.push_back(folderPath);
        if (isProjectRoot(folderPath) || fblang::hasConfigFile(folderPath)) {
          marker =
              workspaceFolderRoots_.insert(fblang::normalizePath(folderPath))
                  .second;
        }
      }
      if (marker) {
        ensureWorkspaceIndex(folderPath);
      }
    }
  }

  // Workspace root: rootUri wins over the first workspace folder; fall back
  // to the first opened file when neither is present (single-file mode).
  std::string rootPath;
  if (req.params.rootUri) {
    rootPath = req.params.rootUri->GetAbsolutePath().path();
  }
  if (rootPath.empty() && !workspaceFolders_.empty()) {
    rootPath = workspaceFolders_[0].string();
  }
  if (!rootPath.empty()) {
    {
      std::lock_guard<std::mutex> const lock(indexesMutex_);
      sessionRoot_ = rootPath;
    }
    // Create the index now only when the client root is itself a workspace
    // root (a version-control marker or config file). A broad root (e.g. the
    // home directory, which hosts several sibling projects) is narrowed to
    // the opened document's project on the first didOpen so unrelated trees
    // are never scanned or cached.
    if (isProjectRoot(rootPath) || fblang::hasConfigFile(rootPath)) {
      ensureWorkspaceIndex(rootPath);
    } else {
      (void)std::fprintf(
          stderr, "[freebasicd] %s\n",
          fblang::trf("workspace root %s has no version-control marker or "
                      "config file; index scope deferred to the first opened "
                      "document (detection: version-control marker, config "
                      "file, else source/include directory)",
                      rootPath)
              .c_str());
    }
  }

  return rsp;
}

td_shutdown::response
FreeBasicServer::onShutdown(td_shutdown::request const &req) {
  td_shutdown::response rsp;
  rsp.id = req.id;

  closeAllIndexes();

  lsp::Any result;
  result.SetJsonString("null", lsp::Any::kNullType);
  rsp.result = result;

  return rsp;
}

void FreeBasicServer::onInitialized(
    Notify_InitializedNotification::notify const &notify) {
  // The client has seen the initialize reply; a dynamic client now gets the
  // watched-file registration. Notifications are FIFO, so this cannot race
  // ahead of `initialized`.
  (void)notify;
  if (!watchedFilesDynamic_) {
    return;
  }

  Req_ClientRegisterCapability::request request =
      session_.endpoint()
          .createRequest<Req_ClientRegisterCapability::request>();

  Registration registration =
      Registration::Create("workspace/didChangeWatchedFiles");
  lsp::Any options;
  options.SetJsonString(
      R"({"watchers":[{"globPattern":"**/*.{bas,bi}","kind":7}]})",
      lsp::Any::kObjectType);
  registration.registerOptions.emplace(std::move(options));
  request.params.registrations.push_back(std::move(registration));

  session_.endpoint().send(request);
}

void FreeBasicServer::onWatchedFiles(
    Notify_WorkspaceDidChangeWatchedFiles::notify const &notify) {
  // The registered glob (**/*.{bas,bi}) already scopes the events; each
  // affected path is routed to the index whose workspace root contains it and
  // that workspace's debounced rescan converges any external edit. The
  // debounce and its async scan live in the index, so the notification FIFO
  // thread returns at once regardless of workspace size. Event kinds beyond
  // the path are intentionally ignored: a full-root scan is authoritative and
  // cheap for FB-sized files. A change outside every indexed root (a sibling
  // project that was never opened) touches no index.
  std::set<std::string> touched;
  for (lsFileEvent const &event : notify.params.changes) {
    std::string const norm =
        fblang::normalizePath(event.uri.GetAbsolutePath().path());
    std::shared_ptr<fblang::WorkspaceIndex> const index = indexFor(norm);
    if (!index) {
      continue;
    }
    std::string const owner = fblang::normalizePath(index->root());
    if (touched.insert(owner).second) {
      index->watchedFilesChanged();
    }
  }
}

// The read-path queries (indexFor/allIndexes) and the folder-table mutations
// all hold indexesMutex_; handlers snapshot a shared_ptr and never touch the
// table again, so these stay lock-safe against the sweep and folder churn.

std::shared_ptr<fblang::WorkspaceIndex>
FreeBasicServer::indexFor(std::string const &normalizedPath) const {
  std::lock_guard<std::mutex> const lock(indexesMutex_);
  std::shared_ptr<fblang::WorkspaceIndex> best;
  std::size_t bestLen = 0;
  for (auto const &[root, index] : indexes_) {
    if (isWithinNormalized(normalizedPath, root) && root.size() >= bestLen) {
      best = index;
      bestLen = root.size();
    }
  }
  if (best) {
    return best;
  }
  // Outside every index root (a document opened from a sibling project beside
  // the workspace): it gets no index of its own — workspace/symbol stays
  // strictly workspace-scoped — but the session-root index hosts its on-demand
  // closure so cross-file resolution still serves it (single-index behavior,
  // preserved across the M11 split). Null in true single-file mode before any
  // didOpen, or under a deferred broad root that never had an index created.
  if (sessionRoot_.empty()) {
    return nullptr;
  }
  auto const it = indexes_.find(fblang::normalizePath(sessionRoot_));
  return it == indexes_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<fblang::WorkspaceIndex>>
FreeBasicServer::allIndexes() const {
  std::lock_guard<std::mutex> const lock(indexesMutex_);
  std::vector<std::shared_ptr<fblang::WorkspaceIndex>> out;
  out.reserve(indexes_.size());
  for (auto const &[root, index] : indexes_) {
    (void)root;
    out.push_back(index);
  }
  return out;
}

fblang::Settings
FreeBasicServer::settingsForDocument(std::string const &normalizedPath) const {
  std::lock_guard<std::mutex> const lock(indexesMutex_);
  std::shared_ptr<fblang::WorkspaceIndex> best;
  std::size_t bestLen = 0;
  for (auto const &[root, index] : indexes_) {
    if (isWithinNormalized(normalizedPath, root) && root.size() >= bestLen) {
      best = index;
      bestLen = root.size();
    }
  }
  if (best) {
    return best->settings();
  }
  // No owning index (single-file mode before any didOpen, or an
  // outside-every-root document served resolution-only): the defaults.
  return {};
}

void FreeBasicServer::closeAllIndexes() {
  std::lock_guard<std::mutex> const lock(indexesMutex_);
  for (auto &[root, index] : indexes_) {
    (void)root;
    index->close();
  }
  indexes_.clear();
}

void FreeBasicServer::dropDetectedIndexesExcept(
    std::string const &keepNormRoot) {
  std::lock_guard<std::mutex> const lock(indexesMutex_);
  for (auto it = indexes_.begin(); it != indexes_.end();) {
    if (it->first == keepNormRoot ||
        workspaceFolderRoots_.find(it->first) != workspaceFolderRoots_.end()) {
      ++it;
    } else {
      it->second->close();
      it = indexes_.erase(it);
    }
  }
}

std::optional<std::filesystem::path>
FreeBasicServer::registeredFolderRootContaining(
    std::string const &normalizedPath) const {
  std::lock_guard<std::mutex> const lock(indexesMutex_);
  std::string best;
  for (std::string const &root : workspaceFolderRoots_) {
    if (isWithinNormalized(normalizedPath, root) && root.size() > best.size()) {
      best = root;
    }
  }
  if (best.empty()) {
    return std::nullopt;
  }
  return std::filesystem::path(best);
}

void FreeBasicServer::onWorkspaceFoldersChanged(
    Notify_WorkspaceDidChangeWorkspaceFolders::notify const &notify) {
  // Added folders join the registration order; marker folders (version-control
  // marker or config file) get a live index immediately, so workspace/symbol
  // serves them without waiting for a didOpen. Removed folders leave the fold
  // table; an index created for a removed root closes unless another
  // registered folder or the session root still needs it. Detected indexes
  // for nested projects inside a removed folder are left alone — they belong
  // to the (still open) projects themselves and are never swept in
  // multi-folder mode.
  for (WorkspaceFolder const &folder : notify.params.event.added) {
    std::filesystem::path const folderPath =
        folder.uri.GetAbsolutePath().path();
    bool marker = false;
    {
      std::lock_guard<std::mutex> const lock(indexesMutex_);
      workspaceFolders_.push_back(folderPath);
      if (isProjectRoot(folderPath) || fblang::hasConfigFile(folderPath)) {
        marker = workspaceFolderRoots_.insert(fblang::normalizePath(folderPath))
                     .second;
      }
    }
    if (marker) {
      ensureWorkspaceIndex(folderPath);
    }
  }
  for (WorkspaceFolder const &folder : notify.params.event.removed) {
    std::string const norm =
        fblang::normalizePath(folder.uri.GetAbsolutePath().path());
    std::string sessionNorm;
    bool stillNeeded = false;
    {
      std::lock_guard<std::mutex> const lock(indexesMutex_);
      sessionNorm = sessionRoot_.empty() ? std::string()
                                         : fblang::normalizePath(sessionRoot_);
      workspaceFolders_.erase(
          std::remove_if(workspaceFolders_.begin(), workspaceFolders_.end(),
                         [&](std::filesystem::path const &p) {
                           return fblang::normalizePath(p) == norm;
                         }),
          workspaceFolders_.end());
      workspaceFolderRoots_.erase(norm);
      // The removed folder's root index is only freed when nothing left in
      // the session needs it: the session root (a rootUri client), another
      // registered folder, or a surviving registered marker root.
      stillNeeded =
          norm == sessionNorm ||
          std::any_of(workspaceFolders_.begin(), workspaceFolders_.end(),
                      [&](std::filesystem::path const &p) {
                        return fblang::normalizePath(p) == norm;
                      }) ||
          workspaceFolderRoots_.count(norm) != 0;
    }
    if (!stillNeeded) {
      std::lock_guard<std::mutex> const lock(indexesMutex_);
      if (auto const it = indexes_.find(norm); it != indexes_.end()) {
        it->second->close();
        indexes_.erase(it);
      }
    }
  }
}

void FreeBasicServer::onDidChangeConfiguration(
    Notify_WorkspaceDidChangeConfiguration::notify const &notify) {
  // The payload is ignored: settings live in each root's freebasicd.toml,
  // so the notification is only a signal to re-read them. The re-read is
  // idempotent — no config change, no effect; an empty/comment-only file and
  // a missing one both mean defaults, unchanged.
  (void)notify;
  for (std::shared_ptr<fblang::WorkspaceIndex> const &index : allIndexes()) {
    fblang::Settings const fresh = settingsForDirLogged(index->root());
    fblang::Settings const old = index->settings();
    if (fresh == old) {
      continue;
    }
    bool const includeChanged = fresh.includePaths != old.includePaths;
    bool const flippedOff = old.diagnosticsOn && !fresh.diagnosticsOn;
    bool const flippedOn = fresh.diagnosticsOn && !old.diagnosticsOn;
    // Adopt the new settings for the whole root...
    index->applySettings(fresh);
    // ...and reconcile the root's open buffers with the gates. Private state
    // diffs drive only the work that matters: turning diagnostics off
    // publishes a single empty result per open buffer (publishDiagnostics has
    // no tombstones; the empty publish is the clear), turning them on
    // re-publishes what the buffers hold now, and an include-path change
    // re-resolves the buffers' include edges so a newly reachable header
    // resolves without an edit.
    std::string const normRoot = fblang::normalizePath(index->root());
    bool const rePublish = flippedOn || includeChanged;
    for (std::string const &normFile : openFiles_) {
      if (!isWithinNormalized(normFile, normRoot)) {
        continue; // belongs to another root (or to no root's open set)
      }
      std::shared_ptr<WorkingFile> const file =
          workingFiles_.GetFileByFilename(AbsolutePath(normFile));
      if (!file) {
        continue;
      }
      lsDocumentUri const uri = lsDocumentUri(AbsolutePath(normFile));
      if (flippedOff) {
        publishDiagnostics(uri, {});
      }
      if (rePublish) {
        reparseAndPublish(file, uri);
      }
    }
  }
}

void FreeBasicServer::onDidOpen(Notify_TextDocumentDidOpen::notify &notify) {
  std::filesystem::path const openedFile =
      notify.params.textDocument.uri.GetAbsolutePath().path();
  std::string const normFile = fblang::normalizePath(openedFile);
  IndexRootChoice const choice = chooseIndexRoot(openedFile);
  std::string const normChoice = fblang::normalizePath(choice.root);
  std::shared_ptr<fblang::WorkspaceIndex> const serving = indexFor(normFile);
  // A broad client root (no root marker of its own) is re-evaluated on every
  // open so switching to a sibling project re-roots the index to that
  // project: stale non-registered detected indexes are dropped then, while
  // registered workspace-folder roots are never swept. Single-file and
  // multi-folder sessions never sweep — their roots are final per open.
  bool deferredBroadRoot = false;
  std::filesystem::path sessionRoot;
  {
    std::lock_guard<std::mutex> const lock(indexesMutex_);
    sessionRoot = sessionRoot_;
    deferredBroadRoot = !sessionRoot.empty() && !isProjectRoot(sessionRoot) &&
                        !fblang::hasConfigFile(sessionRoot);
  }
  bool const alreadyRooted =
      serving && fblang::normalizePath(serving->root()) == normChoice;
  // A choice root that does not even contain the opened file is the
  // outside-the-workspace fallback (a sibling-project document): it must never
  // create or re-root an index — the document is served through the hosting
  // index's on-demand closure alone, and workspace/symbol stays clean.
  bool const choiceContainsFile = isWithinNormalized(normFile, normChoice);
  if (!serving || (deferredBroadRoot && !alreadyRooted)) {
    // A root the server found itself (version-control marker, config file, or
    // source/include layout), other than the client's own, gets an
    // explanatory stderr note.
    bool const notThePassedRoot =
        sessionRoot.empty() || normChoice != fblang::normalizePath(sessionRoot);
    if ((choice.reason == IndexRootChoice::Reason::VcsMarker ||
         choice.reason == IndexRootChoice::Reason::ConfigFile ||
         choice.reason == IndexRootChoice::Reason::SourceLayout) &&
        notThePassedRoot) {
      logDetectedRoot(choice, sessionRoot);
    }
    if (choiceContainsFile) {
      if (deferredBroadRoot) {
        dropDetectedIndexesExcept(normChoice);
      }
      ensureWorkspaceIndex(choice.root);
    }
  }
  std::shared_ptr<WorkingFile> const file =
      workingFiles_.OnOpen(notify.params.textDocument);
  if (!file) {
    return;
  }
  openFiles_.insert(normFile);
  reparseAndPublish(file, notify.params.textDocument.uri);
}

void FreeBasicServer::onDidChange(
    Notify_TextDocumentDidChange::notify const &notify) {
  std::shared_ptr<WorkingFile> const file =
      workingFiles_.OnChange(notify.params);
  if (!file) {
    return;
  }
  reparseAndPublish(file, notify.params.textDocument.uri);
}

void FreeBasicServer::onDidSave(
    Notify_TextDocumentDidSave::notify const &notify) {
  std::shared_ptr<WorkingFile> const file =
      workingFiles_.OnSave(notify.params.textDocument);
  if (!file) {
    return;
  }
  reparseAndPublish(file, notify.params.textDocument.uri);
}

void FreeBasicServer::onDidClose(
    Notify_TextDocumentDidClose::notify const &notify) {
  if (!workingFiles_.OnClose(notify.params.textDocument)) {
    return;
  }
  std::string const normPath = fblang::normalizePath(
      notify.params.textDocument.uri.GetAbsolutePath().path());
  openFiles_.erase(normPath);
  // The buffer's cache entries are live-truth pinned to the buffer's bytes;
  // drop them so a later didOpen of the same file starts fresh (the disk is
  // master for closed files).
  analysisCache_.removePath(normPath);
  publishDiagnostics(notify.params.textDocument.uri, {});
}

void FreeBasicServer::reparseAndPublish(
    std::shared_ptr<WorkingFile> const &file, lsDocumentUri const &uri) {
  std::string const path = uri.GetAbsolutePath().path();
  std::string const normPath = fblang::normalizePath(path);
  std::shared_ptr<fblang::AnalysisCache::Entry const> const cached =
      analysisCache_.get(normPath, file->GetContentNoLock(),
                         static_cast<std::uint64_t>(file->version),
                         /*fromBuffer=*/true, /*insert=*/true);
  std::string_view const content = cached->content;
  fblang::AnalyzedDoc const &doc = cached->analysis;
  std::vector<lsDiagnostic> diags = convertDiagnostics(content, doc.parse);

  std::shared_ptr<fblang::WorkspaceIndex> const index = indexFor(normPath);
  if (index) {
    std::string const ext =
        fblang::toLowerChars(std::filesystem::path(path).extension().string());
    if (ext == ".bas" || ext == ".bi") {
      std::uint64_t mtime = 0;
      std::uint64_t size = 0;
      fblang::statFile(path, &mtime, &size);
      // Open-buffer entries come from the live buffer, not disk: scan's
      // mtime/size cache-hit must never accept them, or an unsaved edit
      // would shadow the source scan is about to read. Include targets
      // still resolve against disk, and unresolved ones publish
      // include-not-found. The entry's roots copy the cached analysis
      // (which stays pinned for the request path).
      fblang::IndexedFile entry = fblang::indexedFileFromAnalysis(
          normPath, mtime, size, doc, index->root(), false,
          index->includeDirs());
      appendIncludeDiagnostics(content, entry, &diags);
      index->upsert(std::move(entry));
    }
  }

  if (settingsForDocument(normPath).diagnosticsOn) {
    publishDiagnostics(uri, std::move(diags));
  }
}

td_symbol::response
FreeBasicServer::onDocumentSymbol(td_symbol::request const &req) {
  td_symbol::response rsp;
  rsp.id = req.id;

  std::shared_ptr<fblang::AnalysisCache::Entry const> const cached =
      cachedRequestAnalysis(req.params.textDocument.uri);
  if (!cached) {
    return rsp;
  }
  std::string_view const content = cached->content;
  for (auto const &root : cached->analysis.parse.roots) {
    if (root.kind == fblang::SymbolKind::Scope) {
      continue;
    }
    rsp.result.push_back(convertSymbol(content, root));
  }
  return rsp;
}

td_hover::response FreeBasicServer::onHover(td_hover::request const &req) {
  td_hover::response rsp;
  rsp.id = req.id;

  std::shared_ptr<fblang::AnalysisCache::Entry const> const cached =
      cachedRequestAnalysis(req.params.textDocument.uri);
  if (!cached) {
    return rsp;
  }
  std::string_view const content = cached->content;
  std::uint32_t const offset =
      fblang::byteOffsetForUtf16Position(content, req.params.position);
  std::string const normPath = fblang::normalizePath(
      req.params.textDocument.uri.GetAbsolutePath().path());
  fblang::AnalyzedDoc const &doc = cached->analysis;

  // The hovered word in the requesting document (a usage and its declaration
  // live in different files when resolution is cross-file).
  fblang::SourceRange const tokRange = fblang::tokenRangeAt(doc, offset);

  auto setRange = [&](fblang::SourceRange const &r) {
    if (r.beg < r.end && r.end <= content.size()) {
      rsp.result.range.emplace(fblang::utf16Range(content, r.beg, r.end));
    }
  };

  // Member access (`.` / `->`): hover shows the field declaration, not the
  // enclosing procedure. `expr.member` resolves through the base variable's
  // declared type (across the include closure) and each intermediate member's
  // own declared type for chained access (`.sectors(i).floorHeight`). The
  // closure may live in a sibling project outside the workspace root (the
  // requesting document's own project), so warm it on demand first. The walk
  // returns a pointer into a workspace entry, and `access` carries that
  // entry's pin (MemberAccess::file), so a concurrent rescan cannot free it
  // under this handler.
  ensureRequestClosure(normPath);
  std::shared_ptr<fblang::WorkspaceIndex> const index = indexFor(normPath);
  fblang::MemberAccess const access = fblang::resolveMemberAccess(
      doc, normPath, offset, index ? index.get() : nullptr);
  if (access.member != nullptr) {
    std::string markdown;
    if (!access.member->signature.empty()) {
      markdown = "```basic\n" + access.member->signature + "\n```";
    } else {
      markdown = "`" + access.member->name + "`";
    }
    if (access.direct && !access.baseName.empty()) {
      markdown += access.enumMember
                      ? "\n\nEnum member of `" + access.baseName + "`."
                      : "\n\nMember of `" + access.baseName + "` (`" +
                            access.ownerTypeName + "`).";
    } else {
      markdown += access.enumMember
                      ? "\n\nEnum member of `" + access.ownerTypeName + "`."
                      : "\n\nMember of `" + access.ownerTypeName + "`.";
    }
    if (!access.member->doc.empty()) {
      markdown += "\n\n---\n" + access.member->doc;
    }
    rsp.result.contents.second.emplace(
        MarkupContent{std::string("markdown"), std::move(markdown)});
    setRange(tokRange);
    return rsp;
  }

  // The cursor is on a `expr.member` / `with`-implicit member access whose
  // chain root variable resolved, but the declared type — and so the member
  // itself — could not be pinned down (type unknown, unindexed, or the member
  // is missing). Name the owning variable anyway; falling through here would
  // point at a colliding identifier or the enclosing routine, which is worse
  // than the honest "Member of `x`." when no type info exists.
  if (access.memberAccess) {
    std::string markdown = "Member of `" + access.baseName + "`.";
    if (!access.ownerTypeName.empty()) {
      markdown = "Member of `" + access.baseName + "` (`" +
                 access.ownerTypeName + "`).";
    }
    rsp.result.contents.second.emplace(
        MarkupContent{std::string("markdown"), std::move(markdown)});
    setRange(tokRange);
    return rsp;
  }

  // Hover on a variable *usage* shows the declaration it resolves to: a use
  // must show the declaring Dim/Const/Param, not the enclosing symbol.
  fblang::CrossDecl const target = resolveAtOrAcross(doc, normPath, offset);
  if (target.decl != nullptr &&
      target.decl->kind != fblang::SymbolKind::Scope) {
    std::string markdown;
    if (!target.decl->signature.empty()) {
      markdown = "```basic\n" + target.decl->signature + "\n```";
    } else {
      markdown = "`" + target.decl->name + "`";
    }
    std::vector<fblang::Symbol> const &roots =
        target.file ? target.file->roots : doc.parse.roots;
    markdown += "\n\n" + symbolKindLine(target.decl, roots);
    if (!target.decl->doc.empty()) {
      markdown += "\n\n---\n" + target.decl->doc;
    }
    rsp.result.contents.second.emplace(
        MarkupContent{std::string("markdown"), std::move(markdown)});
    if (tokRange.beg < tokRange.end) {
      setRange(tokRange);
    } else {
      setRange(target.decl->selection);
    }
    return rsp;
  }

  // A reserved keyword never names a user symbol. Inside a procedure body a
  // keyword was falling through to the enclosing procedure's signature
  // (hover `as`, `with`, `end` and get the SUB's header), while at module
  // level the same word shows its wiki link. Match module level: a keyword
  // token goes straight to the intrinsic/wiki-link renderer below, unless
  // the keyword opens the very declaration that starts there (`type Map`,
  // `sub run`) — the declaration is more useful than a generic link.
  fblang::Token const *hoveredTok = nullptr;
  for (fblang::Token const &t : doc.tokens) {
    if (t.beg <= offset && offset <= t.end) {
      hoveredTok = &t;
      break;
    }
  }
  fblang::Symbol const *const innermost =
      deepestSymbolAt(doc.parse.roots, offset);
  bool const keywordHover =
      hoveredTok != nullptr && hoveredTok->kind == fblang::TokenKind::Keyword;
  bool const keywordOpensDeclaration =
      keywordHover && innermost != nullptr &&
      innermost->kind != fblang::SymbolKind::Scope &&
      innermost->range.beg == hoveredTok->beg;

  // No declaration under the cursor: hover on the enclosing declaration for
  // context. Declaration-scope (Scope) nodes are structure, not symbols, so
  // climb past them — a Scope node's `name` is the opener word ("if") and was
  // being shown as the whole hover.
  fblang::Symbol const *encl =
      keywordHover && !keywordOpensDeclaration ? nullptr : innermost;
  while (encl != nullptr && encl->kind == fblang::SymbolKind::Scope) {
    encl = fblang::parentOf(doc.parse, encl);
  }
  if (encl != nullptr) {
    std::string markdown;
    if (!encl->signature.empty()) {
      markdown = "```basic\n" + encl->signature + "\n```";
    } else {
      markdown = "`" + encl->name + "`";
    }
    if (!encl->doc.empty()) {
      markdown += "\n\n---\n" + encl->doc;
    }
    rsp.result.contents.second.emplace(
        MarkupContent{std::string("markdown"), std::move(markdown)});
    if (tokRange.beg < tokRange.end) {
      setRange(tokRange);
    } else {
      setRange(encl->selection);
    }
    return rsp;
  }

  // No user symbol here: the intrinsic catalog owns most callables; fall
  // back to a plain reserved-keyword wiki link for the rest.
  if (offset < content.size() && isWordChar(content[offset])) {
    std::uint32_t beg = offset;
    while (beg > 0 && isWordChar(content[beg - 1])) {
      --beg;
    }
    std::uint32_t end = offset;
    while (end < content.size() && isWordChar(content[end])) {
      ++end;
    }
    std::string_view const raw = content.substr(beg, end - beg);
    if (fblang::Intrinsic const *fn = fblang::intrinsicFor(raw)) {
      std::string markdown = "```basic\n" + std::string(fn->signature) +
                             "\n```\n\nFreeBASIC intrinsic";
      std::string const url = fblang::intrinsicDocsUrl(*fn);
      if (!url.empty()) {
        markdown += " — [FreeBASIC docs](" + url + ")";
      }
      rsp.result.contents.second.emplace(
          MarkupContent{std::string("markdown"), std::move(markdown)});
      rsp.result.range.emplace(fblang::utf16Range(content, beg, end));
      return rsp;
    }
    std::string bare = fblang::toLowerChars(std::string(raw));
    if (!bare.empty() && fblang::isSuffixChar(bare.back())) {
      bare.pop_back();
    }
    std::string const url = fblang::keywordDocsUrl(bare);
    if (!url.empty()) {
      rsp.result.contents.second.emplace(
          MarkupContent{std::string("markdown"), "`" + bare +
                                                     "` — FreeBASIC keyword\n\n"
                                                     "[FreeBASIC docs](" +
                                                     url + ")"});
      rsp.result.range.emplace(fblang::utf16Range(content, beg, end));
      return rsp;
    }
  }
  return rsp;
}

td_foldingRange::response
FreeBasicServer::onFoldingRange(td_foldingRange::request const &req) {
  td_foldingRange::response rsp;
  rsp.id = req.id;

  std::shared_ptr<fblang::AnalysisCache::Entry const> const cached =
      cachedRequestAnalysis(req.params.textDocument.uri);
  if (!cached) {
    return rsp;
  }
  std::string_view const content = cached->content;

  for (auto const &br : cached->analysis.parse.blockRanges) {
    lsPosition const start = fblang::utf16Position(content, br.beg);
    lsPosition const closer = fblang::utf16Position(content, br.end);
    if (closer.line <= start.line) {
      continue; // single-line construct: nothing to fold
    }

    FoldingRange fr;
    fr.startLine = static_cast<int>(start.line);
    fr.startCharacter = static_cast<int>(start.character);
    fr.endLine =
        static_cast<int>(closer.line) - 1; // keep the END keyword line visible
    // -1 character clamps to the end of the fold line in UTF-16 units.
    fr.endCharacter = static_cast<int>(
        fblang::utf16Position(content, fblang::byteOffsetForUtf16Position(
                                           content, lsPosition(fr.endLine, -1)))
            .character);
    rsp.result.push_back(fr);
  }
  return rsp;
}

td_definition::response
FreeBasicServer::onDefinition(td_definition::request const &req) {
  td_definition::response rsp;
  rsp.id = req.id;

  std::shared_ptr<fblang::AnalysisCache::Entry const> const cached =
      cachedRequestAnalysis(req.params.textDocument.uri);
  if (!cached) {
    return rsp;
  }
  std::string_view const content = cached->content;
  std::uint32_t const offset =
      fblang::byteOffsetForUtf16Position(content, req.params.position);
  std::string const normPath = fblang::normalizePath(
      req.params.textDocument.uri.GetAbsolutePath().path());

  fblang::AnalyzedDoc const &doc = cached->analysis;
  fblang::CrossDecl const target = resolveAtOrAcross(doc, normPath, offset);
  if (target.decl == nullptr) {
    return rsp;
  }

  lsLocation loc;
  if (!target.file) {
    loc = lsLocation(req.params.textDocument.uri,
                     fblang::utf16Range(content, target.decl->selection.beg,
                                        target.decl->selection.end));
  } else {
    // Remote declaration: convert against the target file's own content
    // (open buffer or disk), and reference it by its own URI. The index
    // snapshot that owns `target.decl` is pinned by `target.file`.
    std::optional<std::string> const remote = contentForPath(target.file->path);
    if (!remote) {
      return rsp;
    }
    loc = lsLocation(lsDocumentUri(AbsolutePath(target.file->path)),
                     fblang::utf16Range(*remote, target.decl->selection.beg,
                                        target.decl->selection.end));
  }
  rsp.result.first.emplace();
  rsp.result.first->push_back(std::move(loc));
  return rsp;
}

td_references::response
FreeBasicServer::onReferences(td_references::request const &req) {
  td_references::response rsp;
  rsp.id = req.id;

  std::shared_ptr<fblang::AnalysisCache::Entry const> const cached =
      cachedRequestAnalysis(req.params.textDocument.uri);
  if (!cached) {
    return rsp;
  }
  std::string_view const content = cached->content;
  std::uint32_t const offset =
      fblang::byteOffsetForUtf16Position(content, req.params.position);
  std::string const normPath = fblang::normalizePath(
      req.params.textDocument.uri.GetAbsolutePath().path());

  fblang::AnalyzedDoc const &doc = cached->analysis;
  fblang::CrossDecl const target = resolveAtOrAcross(doc, normPath, offset);
  if (target.decl == nullptr) {
    return rsp;
  }

  bool const includeDecl = !req.params.context.includeDeclaration ||
                           *req.params.context.includeDeclaration;
  fblang::SourceRange const sel = target.decl->selection;

  // Closed (M7) file closure only: the requesting file plus every file in its
  // transitive include closure, one textual module. A usage is a reference
  // when re-resolving it (shadowing-aware) lands on the target declaration;
  // same-named locals elsewhere never match. Out-of-closure byKey hits are
  // excluded by construction.
  struct Site {
    std::string path;
    fblang::SourceRange range;
  };
  std::vector<Site> sites;
  if (includeDecl) {
    sites.push_back(Site{target.file ? target.file->path : normPath, sel});
  }

  auto collect = [&](fblang::AnalyzedDoc const &d, std::string const &fpath) {
    // The decl's own file is parsed afresh here, so its module root is a
    // different Symbol object than the index entry the cross-file requests
    // compare against. Re-resolve the decl to that local identity (tier 1)
    // and compare pointer-wise; shadowing locals in the owner file correctly
    // fail the match.
    bool const ownerFile =
        fpath == (target.file ? target.file->path : normPath);
    fblang::Symbol const *const ownerLocal =
        ownerFile && target.file ? fblang::resolveAt(d, sel.beg) : nullptr;
    for (fblang::Token const &t : d.tokens) {
      if (t.kind != fblang::TokenKind::Identifier ||
          fblang::toLowerChars(t.text()) != target.decl->key) {
        continue;
      }
      if (t.beg == sel.beg && t.end == sel.end) {
        continue; // the declaration name token itself
      }
      bool const match = ownerLocal
                             ? fblang::resolveAt(d, t.beg) == ownerLocal
                             : resolveAtOrAcross(d, fpath, t.beg) == target;
      if (match) {
        sites.push_back(Site{fpath, {t.beg, t.end}});
      }
    }
  };

  // The cross-file re-resolutions below only work while the index that owns
  // `target` (via `target.file`) stays alive; pin the serving snapshot for the
  // whole walk so a concurrent folder change cannot free it.
  std::shared_ptr<fblang::WorkspaceIndex> const index = indexFor(normPath);
  collect(doc, normPath);
  if (index) {
    for (std::string const &closurePath : index->transitiveIncludes(normPath)) {
      std::shared_ptr<fblang::DocumentContent const> const remote =
          contentForPathAnalysis(closurePath);
      if (!remote) {
        continue;
      }
      collect(remote->analysis,
              fblang::normalizePath(std::filesystem::path(closurePath)));
    }
  }

  std::sort(sites.begin(), sites.end(), [](Site const &a, Site const &b) {
    if (a.path != b.path) {
      return a.path < b.path;
    }
    return a.range.beg < b.range.beg;
  });
  for (Site const &s : sites) {
    std::optional<std::string> const source = contentForPath(s.path);
    if (!source) {
      continue;
    }
    rsp.result.push_back(
        lsLocation(lsDocumentUri(AbsolutePath(s.path)),
                   fblang::utf16Range(*source, s.range.beg, s.range.end)));
  }
  return rsp;
}

td_highlight::response
FreeBasicServer::onHighlight(td_highlight::request const &req) {
  td_highlight::response rsp;
  rsp.id = req.id;

  std::shared_ptr<fblang::AnalysisCache::Entry const> const cached =
      cachedRequestAnalysis(req.params.textDocument.uri);
  if (!cached) {
    return rsp;
  }
  std::string_view content = cached->content;
  std::uint32_t const offset =
      fblang::byteOffsetForUtf16Position(content, req.params.position);
  std::string const normPath = fblang::normalizePath(
      req.params.textDocument.uri.GetAbsolutePath().path());

  fblang::AnalyzedDoc const &doc = cached->analysis;
  fblang::CrossDecl const target = resolveAtOrAcross(doc, normPath, offset);
  if (target.decl == nullptr) {
    return rsp;
  }

  auto add = [&](fblang::SourceRange range) {
    lsDocumentHighlight hl;
    hl.range = fblang::utf16Range(content, range.beg, range.end);
    hl.kind.emplace(lsDocumentHighlightKind::Text);
    rsp.result.push_back(hl);
  };

  // Highlight is per-document: the declaration's in-document usages only, so
  // a remote declaration contributes its identically-named sites here (each
  // re-resolved against the closure) and no foreign range.
  fblang::SourceRange const sel = target.decl->selection;
  if (!target.file) {
    add(sel);
  }
  for (fblang::Token const &t : doc.tokens) {
    if (t.kind != fblang::TokenKind::Identifier ||
        fblang::toLowerChars(t.text()) != target.decl->key) {
      continue;
    }
    if (!target.file && t.beg == sel.beg && t.end == sel.end) {
      continue; // the declaration name token, already added above
    }
    if (resolveAtOrAcross(doc, normPath, t.beg) == target) {
      add({t.beg, t.end});
    }
  }
  return rsp;
}

td_prepareRename::response
FreeBasicServer::onPrepareRename(td_prepareRename::request const &req) {
  td_prepareRename::response rsp;
  rsp.id = req.id;

  std::shared_ptr<fblang::AnalysisCache::Entry const> const cached =
      cachedRequestAnalysis(req.params.textDocument.uri);
  if (!cached) {
    return rsp;
  }
  std::string_view const content = cached->content;
  std::uint32_t const offset =
      fblang::byteOffsetForUtf16Position(content, req.params.position);
  std::string const normPath = fblang::normalizePath(
      req.params.textDocument.uri.GetAbsolutePath().path());

  fblang::AnalyzedDoc const &doc = cached->analysis;
  fblang::CrossDecl const target = resolveAtOrAcross(doc, normPath, offset);
  if (target.decl == nullptr) {
    // Keyword, non-identifier, or an unknown name: not renameable (the
    // paired response serializes as JSON null).
    return rsp;
  }

  // The requesting document's token under the cursor plus the current name
  // as the placeholder. The range must come from the requesting file (the
  // resolved declaration may live in another file, whose coordinates are
  // meaningless here); converting the decl's own selection against this
  // content would produce a garbage range. The pair's first element
  // (lsRange) stays empty; the writer reflects the `second`
  // PrepareRenameResult when it is set.
  fblang::SourceRange const tokRange = fblang::tokenRangeAt(doc, offset);
  PrepareRenameResult result;
  result.range = fblang::utf16Range(content, tokRange.beg, tokRange.end);
  result.placeholder = target.decl->name;
  rsp.result.second.emplace(std::move(result));
  return rsp;
}

td_rename::response FreeBasicServer::onRename(td_rename::request const &req) {
  td_rename::response rsp;
  rsp.id = req.id;

  // An invalid new name cannot lex as a single identifier token; reject it
  // up front (keywords, a lone `_`, digit-leading, or suffix-only names).
  if (!isValidIdentifier(req.params.newName)) {
    throw lsp::RequestError(lsErrorCodes::InvalidParams,
                            "invalid new name: \"" + req.params.newName + "\"");
  }

  std::shared_ptr<fblang::AnalysisCache::Entry const> const cached =
      cachedRequestAnalysis(req.params.textDocument.uri);
  if (!cached) {
    return rsp;
  }
  std::string_view const content = cached->content;
  std::uint32_t const offset =
      fblang::byteOffsetForUtf16Position(content, req.params.position);
  std::string const normPath = fblang::normalizePath(
      req.params.textDocument.uri.GetAbsolutePath().path());

  fblang::AnalyzedDoc const &doc = cached->analysis;
  fblang::CrossDecl const target = resolveAtOrAcross(doc, normPath, offset);
  if (target.decl == nullptr) {
    throw lsp::RequestError(
        lsErrorCodes::InvalidParams,
        "the position does not reference a renameable symbol");
  }
  std::string const newKey = fblang::toLowerChars(req.params.newName);

  // The collision guard and the rename-site sweep below both walk the index
  // that serves the requesting document; pin its snapshot for the whole
  // handler so a concurrent re-root cannot free it out from under the walk
  // (occurrencesAcross returns byte ranges, but the guard compares against
  // `target`'s live module-scope roots).
  std::shared_ptr<fblang::WorkspaceIndex> const index = indexFor(normPath);

  // Collision guard: renaming into a key that an unrelated module-scope
  // declaration of the requesting file or its include closure already owns
  // would fold two declarations into one textual module. The renamed symbol
  // itself (same key, or the same declaration under the new key) is exempt.
  if (index && newKey != target.decl->key) {
    std::vector<std::string> guardPaths = {normPath};
    for (std::string const &p : index->transitiveIncludes(normPath)) {
      guardPaths.push_back(p);
    }
    for (std::string const &p : guardPaths) {
      std::shared_ptr<fblang::IndexedFile const> const f = index->fileAt(p);
      if (!f) {
        continue;
      }
      for (fblang::Symbol const &root : f->roots) {
        if (root.key != newKey) {
          continue;
        }
        std::string const ownerPath =
            target.file ? target.file->path : normPath;
        bool const isTarget =
            f->path == ownerPath &&
            root.selection.beg == target.decl->selection.beg &&
            root.selection.end == target.decl->selection.end;
        if (!isTarget) {
          throw lsp::RequestError(
              lsErrorCodes::InvalidParams,
              "new name \"" + req.params.newName +
                  "\" collides with an existing declaration");
        }
      }
    }
  }

  // The rename site set: requesting file + include closure + reverse
  // reachability, every token re-resolved shadowing-aware so a same-named
  // local that shadows the declaration is untouched. Ranges are byte offsets
  // into the exact content the shared content seam serves, so the same
  // provider converts them to UTF-16 below.
  std::vector<fblang::OccurrenceSite> const sites = fblang::occurrencesAcross(
      doc, normPath, offset, index.get(), [this](std::string const &p) {
        return contentForPathAnalysis(std::filesystem::path(p));
      });
  if (sites.empty()) {
    return rsp;
  }

  // Group the sorted sites by file; the version stays unset on every edit
  // (null for clients: disk content is master for closed files, and open
  // buffers match by uri). LSP 3.16 semantics.
  struct FileEdits {
    std::string path;
    std::vector<fblang::OccurrenceSite> sites;
  };
  std::vector<FileEdits> groups;
  for (fblang::OccurrenceSite const &s : sites) {
    if (groups.empty() || groups.back().path != s.file) {
      groups.push_back(FileEdits{s.file, {}});
    }
    groups.back().sites.push_back(s);
  }

  rsp.result.documentChanges.emplace();
  for (FileEdits const &g : groups) {
    std::optional<std::string> const src = contentForPath(g.path);
    if (!src) {
      continue;
    }
    lsTextDocumentEdit edit;
    edit.textDocument.uri = lsDocumentUri(AbsolutePath(g.path));
    edit.textDocument.version = std::nullopt;
    for (fblang::OccurrenceSite const &s : g.sites) {
      lsTextEdit te;
      te.range = fblang::utf16Range(*src, s.range.beg, s.range.end);
      te.newText = req.params.newName;
      edit.edits.push_back(std::move(te));
    }
    rsp.result.documentChanges->emplace_back(
        lsWorkspaceEdit::Either{std::move(edit), std::nullopt});
  }
  return rsp;
}

td_completion::response
FreeBasicServer::onCompletion(td_completion::request const &req) {
  td_completion::response rsp;
  rsp.id = req.id;

  std::shared_ptr<fblang::AnalysisCache::Entry const> const cached =
      cachedRequestAnalysis(req.params.textDocument.uri);
  if (!cached) {
    return rsp;
  }
  std::string_view const content = cached->content;
  std::uint32_t const offset =
      fblang::byteOffsetForUtf16Position(content, req.params.position);
  std::string const prefix =
      fblang::toLowerChars(completionPrefix(content, offset));

  fblang::AnalyzedDoc const &doc = cached->analysis;

  std::string const normPath = fblang::normalizePath(
      req.params.textDocument.uri.GetAbsolutePath().path());
  // The requesting document may open from a sibling project outside the
  // workspace root; warm its live-buffer include closure so member access can
  // find the owner type across files (mirrors onHover). The member-completion
  // walk returns pointers into a workspace entry, and `mc` carries that
  // entry's pin (`mc.owner.file`) for as long as it is in scope — same
  // contract as MemberAccess::file.
  ensureRequestClosure(normPath);
  std::shared_ptr<fblang::WorkspaceIndex> const index = indexFor(normPath);

  // Context-aware member completion: a `.`/`->` chain (including the
  // `with`-implicit leading dot and qualified `EnumName.member`) completes
  // only the base object's accessible members — never FreeBASIC keywords,
  // globals, or intrinsics (FreeBASIC.md §7 Access sections). Non-public
  // members are gated:
  // they appear only inside a member procedure of the owner type.
  fblang::MemberCompletion const mc = fblang::resolveMemberCompletion(
      doc, normPath, offset, index ? index.get() : nullptr);
  if (mc.memberAccess) {
    for (fblang::Symbol const *m : mc.members) {
      if (!hasPrefix(fblang::toLowerChars(m->name), prefix)) {
        continue;
      }
      lsCompletionItem item;
      item.label = m->name;
      item.kind.emplace(completionKindFor(m->kind));
      if (!m->signature.empty()) {
        item.detail.emplace(m->signature);
      }
      if (!m->doc.empty()) {
        item.documentation.emplace();
        item.documentation->second.emplace(
            MarkupContent{std::string("markdown"), std::string(m->doc)});
      }
      rsp.result.items.push_back(std::move(item));
    }
    return rsp;
  }

  for (std::string_view const w : fblang::reservedWords()) {
    if (fblang::intrinsicFor(w) != nullptr) {
      // The catalog owns this name: one richer item (signature + wiki page)
      // replaces the bare keyword entry below.
      continue;
    }
    if (!hasPrefix(w, prefix)) {
      continue;
    }
    lsCompletionItem item;
    item.label = std::string(w);
    item.kind.emplace(lsCompletionItemKind::Keyword);
    std::string const url = fblang::keywordDocsUrl(w);
    if (!url.empty()) {
      item.documentation.emplace();
      item.documentation->second.emplace(MarkupContent{
          std::string("markdown"), std::string("**FreeBASIC keyword**\n\n[") +
                                       std::string(w) + "](" + url + ")"});
    }
    rsp.result.items.push_back(std::move(item));
  }

  for (char const *opener : kBlockOpeners) {
    fblang::BlockCloser closer;
    if (!fblang::blockForOpener(opener, &closer) || !closer.needsEnd) {
      continue;
    }
    std::string const label = "end " + std::string(closer.closeWord);
    if (!hasPrefix(label, prefix)) {
      continue;
    }
    lsCompletionItem item;
    item.label = label;
    item.kind.emplace(lsCompletionItemKind::Snippet);
    item.insertText.emplace(fblang::closerDisplay(closer));
    rsp.result.items.push_back(std::move(item));
  }

  std::vector<std::string> seen;
  for (fblang::Symbol const *sym : fblang::visibleSymbols(doc, offset)) {
    if (!hasPrefix(sym->key, prefix)) {
      continue;
    }
    bool dup = false;
    for (auto const &k : seen) {
      if (k == sym->key) {
        dup = true;
        break;
      }
    }
    if (dup) {
      continue;
    }
    seen.push_back(sym->key);
    lsCompletionItem item;
    item.label = sym->name;
    item.kind.emplace(completionKindFor(sym->kind));
    if (!sym->signature.empty()) {
      item.detail.emplace(sym->signature);
    }
    rsp.result.items.push_back(std::move(item));
  }

  // Closure module-scope roots come behind the in-file symbols, deduped by
  // key: an inner-scope name shadows a same-named closure global (the first
  // entry in `seen` won). The storage gate applies to the closure the same
  // way it does in-file: from inside a procedure body, plain module-level Dim
  // roots of included headers are not visible.
  if (index) {
    bool const storageGated = fblang::insideProcedureBody(
        doc.parse, fblang::innermostScope(doc.parse, offset));
    // The requesting document may open from a sibling project outside the
    // workspace root; stay on the live-buffer include closure (see
    // ensureRequestClosure above) so its module-level names complete too.
    for (std::string const &closurePath : index->transitiveIncludes(normPath)) {
      std::shared_ptr<fblang::IndexedFile const> const closure =
          index->fileAt(closurePath);
      if (!closure) {
        continue;
      }
      for (fblang::Symbol const &root : closure->roots) {
        if (root.key.empty() || !hasPrefix(root.key, prefix)) {
          continue;
        }
        if (storageGated && root.kind == fblang::SymbolKind::Dim &&
            !root.shared) {
          continue;
        }
        bool dup = false;
        for (auto const &k : seen) {
          if (k == root.key) {
            dup = true;
            break;
          }
        }
        if (dup) {
          continue;
        }
        seen.push_back(root.key);
        lsCompletionItem item;
        item.label = root.name;
        item.kind.emplace(completionKindFor(root.kind));
        if (!root.signature.empty()) {
          item.detail.emplace(root.signature);
        }
        rsp.result.items.push_back(std::move(item));
      }
    }
  }

  // Built-in intrinsics come last. The catalog owns their canonical spelling,
  // signature, and wiki page; `seen` still wins so a user symbol shadows a
  // header-provided name (e.g. a local `Format`). Statement rows appear only
  // where a statement may begin, so expression completion stays call-shaped.
  bool const atStatement = fblang::statementPosition(doc.tokens, offset);
  for (fblang::Intrinsic const *fn : fblang::intrinsics()) {
    if (fn->kind == fblang::IntrinsicKind::Statement && !atStatement) {
      continue;
    }
    std::string_view const signature = fn->signature;
    std::size_t const cut = signature.find_first_of(" (");
    std::string_view const name =
        cut == std::string_view::npos ? signature : signature.substr(0, cut);
    std::string const labelLower = fblang::toLowerChars(std::string(name));
    if (!hasPrefix(labelLower, prefix)) {
      continue;
    }
    bool dup = false;
    for (auto const &k : seen) {
      std::string bare = k; // seen keys carry a type suffix; compare bare
      if (!bare.empty() && fblang::isSuffixChar(bare.back())) {
        bare.pop_back();
      }
      if (bare == fn->key) {
        dup = true;
        break;
      }
    }
    if (dup) {
      continue;
    }
    seen.emplace_back(fn->key);
    lsCompletionItem item;
    item.label = std::string(name);
    item.kind.emplace(fn->kind == fblang::IntrinsicKind::Function
                          ? lsCompletionItemKind::Function
                          : lsCompletionItemKind::Keyword);
    item.detail.emplace(std::string(signature));
    std::string const url = fblang::intrinsicDocsUrl(*fn);
    if (!url.empty()) {
      item.documentation.emplace();
      item.documentation->second.emplace(MarkupContent{
          std::string("markdown"), std::string("**FreeBASIC intrinsic**\n\n[") +
                                       std::string(name) + "](" + url + ")"});
    }
    rsp.result.items.push_back(std::move(item));
  }
  return rsp;
}

td_signatureHelp::response
FreeBasicServer::onSignatureHelp(td_signatureHelp::request const &req) {
  td_signatureHelp::response rsp;
  rsp.id = req.id;

  std::shared_ptr<fblang::AnalysisCache::Entry const> const cached =
      cachedRequestAnalysis(req.params.textDocument.uri);
  if (!cached) {
    return rsp;
  }
  std::string_view const content = cached->content;
  std::uint32_t const offset =
      fblang::byteOffsetForUtf16Position(content, req.params.position);

  // The analyzed document already carries the full token stream; reuse it
  // instead of re-lexing the buffer per signatureHelp request.
  std::vector<fblang::Token> const &toks = cached->analysis.tokens;
  if (toks.empty()) {
    return rsp;
  }

  // Stack of unclosed '(' with the identifier callee right before each.
  std::vector<int> openStack;
  std::vector<int> calleeStack;
  for (std::size_t i = 0; i < toks.size(); ++i) {
    fblang::Token const &t = toks[i];
    if (t.beg > offset) {
      break;
    }
    if (t.kind != fblang::TokenKind::Symbol) {
      continue;
    }
    std::string_view const s = t.text();
    if (s == ")") {
      if (!openStack.empty()) {
        openStack.pop_back();
        calleeStack.pop_back();
      }
      continue;
    }
    if (s != "(") {
      continue;
    }
    int callee = -1;
    for (std::size_t j = i; j > 0; --j) {
      fblang::Token const &prev = toks[j - 1];
      if (prev.kind == fblang::TokenKind::Newline) {
        break;
      }
      if (prev.kind == fblang::TokenKind::Identifier ||
          prev.kind == fblang::TokenKind::Keyword) {
        // Keywords can be callees too: `mid$` lexes as a keyword, and the
        // intrinsic catalog resolves it below when no user declaration does.
        callee = static_cast<int>(j - 1);
        break;
      }
    }
    openStack.push_back(static_cast<int>(i));
    calleeStack.push_back(callee);
  }
  if (openStack.empty() || calleeStack.back() < 0) {
    return rsp;
  }
  int const openIdx = openStack.back();
  int const nameIdx = calleeStack.back();

  fblang::Token const &calleeTok = toks[static_cast<std::size_t>(nameIdx)];
  fblang::Symbol const *decl =
      fblang::resolveAt(cached->analysis, calleeTok.beg);
  bool userCallable = false;
  if (decl != nullptr) {
    switch (decl->kind) {
    case fblang::SymbolKind::Sub:
    case fblang::SymbolKind::Function:
    case fblang::SymbolKind::Property:
    case fblang::SymbolKind::Constructor:
    case fblang::SymbolKind::Destructor:
    case fblang::SymbolKind::Operator:
      userCallable = true;
      break;
    default:
      break;
    }
  }

  fblang::Intrinsic const *intr = nullptr;
  if (!userCallable) {
    // No user declaration: a built-in function still gets a signature.
    intr = fblang::intrinsicFor(calleeTok.text());
    if (intr == nullptr || intr->kind != fblang::IntrinsicKind::Function) {
      return rsp;
    }
  }

  lsSignatureInformation info;
  if (intr != nullptr) {
    info.label = std::string(intr->signature);
    for (std::string_view const label : fblang::signatureParamLabels(*intr)) {
      lsParameterInformation pi;
      pi.label = std::string(label);
      info.parameters.push_back(std::move(pi));
    }
  } else {
    info.label = decl->signature.empty() ? decl->name : decl->signature;
    for (auto const &p : decl->children) {
      if (p.kind == fblang::SymbolKind::Parameter) {
        lsParameterInformation pi;
        pi.label = p.name;
        info.parameters.push_back(std::move(pi));
      }
    }
  }

  int activeParam = 0;
  int depth = 0;
  for (std::size_t i = static_cast<std::size_t>(openIdx) + 1; i < toks.size();
       ++i) {
    fblang::Token const &t = toks[i];
    if (t.beg >= offset) {
      break;
    }
    if (t.kind != fblang::TokenKind::Symbol) {
      continue;
    }
    std::string_view const s = t.text();
    if (s == "(") {
      ++depth;
    } else if (s == ")") {
      if (depth > 0) {
        --depth;
      }
    } else if (s == "," && depth == 0) {
      ++activeParam;
    }
  }

  rsp.result.signatures.push_back(std::move(info));
  rsp.result.activeSignature.emplace(0);
  rsp.result.activeParameter.emplace(activeParam);
  return rsp;
}

wp_symbol::response
FreeBasicServer::onWorkspaceSymbol(wp_symbol::request const &req) {
  wp_symbol::response rsp;
  // Multi-workspace aggregation (M11): every live index contributes, one
  // sorted hit list for the whole session. Overlapping roots (a registered
  // folder inside a broad session root) can scan the same file twice, so
  // files are deduped by path. The aggregated vector pins every snapshot for
  // the whole reply build.
  std::vector<std::shared_ptr<fblang::WorkspaceIndex>> const indexes =
      allIndexes();
  if (indexes.empty()) {
    return rsp;
  }

  std::string const query = fblang::toLowerChars(req.params.query);

  struct Match {
    fblang::Symbol const *sym;
    std::string container;
  };
  // `file` is the pinned snapshot entry, not a pointer into it: a background
  // scan replaces entries wholesale (upsert), so a raw pointer dies with the
  // old entry mid-reply — while this loop is off doing file I/O per file.
  struct FileMatches {
    std::shared_ptr<fblang::IndexedFile const> file;
    std::vector<Match> matches;
  };

  std::vector<FileMatches> hits;

  std::function<void(fblang::Symbol const &, std::string const &,
                     std::vector<Match> &)>
      collect = [&query, &collect](fblang::Symbol const &s,
                                   std::string const &container,
                                   std::vector<Match> &into) {
        std::string const key = fblang::toLowerChars(s.key);
        std::string const name = fblang::toLowerChars(s.name);
        if (key.find(query) != std::string::npos ||
            name.find(query) != std::string::npos) {
          into.push_back(Match{&s, container});
        }
        std::string const next =
            container.empty() ? s.name : container + "." + s.name;
        for (auto const &c : s.children) {
          collect(c, next, into);
        }
      };

  std::set<std::string> seenFiles;
  for (auto const &idx : indexes) {
    for (auto const &file : idx->snapshot()) {
      if (!seenFiles.insert(file->path).second) {
        continue;
      }
      std::vector<Match> matches;
      for (auto const &root : file->roots) {
        collect(root, {}, matches);
      }
      if (matches.empty()) {
        continue;
      }
      hits.push_back(FileMatches{file, std::move(matches)});
    }
  }

  for (auto &hit : hits) {
    std::optional<std::string> const src = contentForPath(hit.file->path);
    if (!src) {
      continue;
    }
    std::string_view const content = *src;
    for (auto &m : hit.matches) {
      lsSymbolInformation info;
      info.name = m.sym->name;
      info.kind = toLspSymbolKind(m.sym->kind);
      info.location =
          lsLocation(lsDocumentUri(AbsolutePath(hit.file->path)),
                     fblang::utf16Range(content, m.sym->selection.beg,
                                        m.sym->selection.end));
      if (!m.container.empty()) {
        info.containerName.emplace(m.container);
      }
      rsp.result.push_back(std::move(info));
    }
  }
  return rsp;
}

td_codeAction::response
FreeBasicServer::onCodeAction(td_codeAction::request const &req) {
  td_codeAction::response rsp;
  rsp.id = req.id;

  // Every fix we offer is a quickfix, so the `context.only` filter is a
  // server-side gate: a refactor-only or source-only request gets nothing.
  if (req.params.context.only &&
      !kindRequested(*req.params.context.only, "quickfix")) {
    return rsp;
  }

  std::string const path = req.params.textDocument.uri.GetAbsolutePath().path();
  std::string const normPath = fblang::normalizePath(path);
  // Quick fixes are keyed off the published diagnostics; with diagnostics off
  // the client is never told about any, so there is nothing to fix (M11's
  // gate, the same one the publish path obeys).
  if (!settingsForDocument(normPath).diagnosticsOn) {
    return rsp;
  }

  std::shared_ptr<fblang::AnalysisCache::Entry const> const cached =
      cachedRequestAnalysis(req.params.textDocument.uri);
  if (!cached) {
    return rsp;
  }
  std::string_view const content = cached->content;
  fblang::AnalyzedDoc const &doc = cached->analysis;

  // The set of diagnostics the next publish would carry: the parse
  // diagnostics plus this document's own unresolved include edges (the
  // `include-not-found` set M6 publishes, taken from the same live entry).
  std::vector<fblang::Diagnostic> published = doc.parse.diagnostics;
  std::shared_ptr<fblang::WorkspaceIndex> const index = indexFor(normPath);
  if (index) {
    if (std::shared_ptr<fblang::IndexedFile const> const entry =
            index->fileAt(normPath)) {
      std::vector<fblang::Diagnostic> const unresolved =
          fblang::unresolvedIncludeDiagnostics(entry->includes, content.size());
      published.insert(published.end(), unresolved.begin(), unresolved.end());
    }
  }

  // One snapshot for the whole reply: a fix provider may mine it for candidate
  // targets, and a concurrent folder add/remove cannot pull the vector a
  // provider is reading out from under it.
  std::filesystem::path const documentPath(path);
  std::vector<std::shared_ptr<fblang::IndexedFile const>> const files =
      index ? index->snapshot()
            : std::vector<std::shared_ptr<fblang::IndexedFile const>>{};

  fblang::QuickFixContext ctx;
  ctx.content = content;
  ctx.doc = &doc;
  ctx.documentPath = &documentPath;
  if (index) {
    ctx.workspaceFiles = &files;
    // The document's own include-resolution seam, so a candidate literal is
    // offered only when the next publish would resolve it.
    ctx.resolveInclude = [&path, index](std::string const &literal) {
      return fblang::resolveIncludeTarget(literal, path, index->root(),
                                          fblang::defaultSystemIncludeDir(),
                                          index->includeDirs());
    };
  }

  for (fblang::Diagnostic const &d :
       requestedDiagnostics(doc, content, req.params.range, published,
                            req.params.context.diagnostics)) {
    fblang::QuickFixProvider const *const provider =
        fblang::quickFixProviderFor(d.code);
    if (provider == nullptr) {
      continue; // a diagnostic with no registered fix offers nothing
    }
    for (fblang::QuickFix const &fix : (*provider)(d, ctx)) {
      // The edit is keyed by the request's URI verbatim (not its path): a
      // `changes` key the client cannot match to a document is an edit it
      // silently drops.
      rsp.result.emplace_back();
      rsp.result.back().second = quickFixCodeAction(
          req.params.textDocument.uri.raw_uri_, content, d, fix);
    }
  }
  return rsp;
}

td_semanticTokens_full::response FreeBasicServer::onSemanticTokensFull(
    td_semanticTokens_full::request const &req) {
  td_semanticTokens_full::response rsp;
  rsp.id = req.id;

  std::string const normPath = fblang::normalizePath(
      req.params.textDocument.uri.GetAbsolutePath().path());
  if (!settingsForDocument(normPath).semanticTokensOn) {
    // Feature gate (M11): the capability stays advertised, but a session that
    // disabled semantic tokens gets a valid empty result with a fresh, cached
    // baseline (a delta diffed against the empty set stays consistent), never
    // an error or a null result.
    SemanticTokens tokens;
    tokens.data.clear();
    tokens.resultId.emplace(storeDelta({}));
    rsp.result.emplace(std::move(tokens));
    return rsp;
  }

  std::shared_ptr<fblang::AnalysisCache::Entry const> const cached =
      cachedRequestAnalysis(req.params.textDocument.uri);
  if (!cached) {
    return rsp;
  }
  std::string_view const content = cached->content;
  fblang::AnalyzedDoc const &doc = cached->analysis;
  std::vector<std::int32_t> const data =
      fblang::encodeTokenData(fblang::semanticTokens(doc, content));

  SemanticTokens tokens;
  tokens.data = data;
  tokens.resultId.emplace(storeDelta(data));
  rsp.result.emplace(std::move(tokens));
  return rsp;
}

td_semanticTokens_full_delta::response FreeBasicServer::onSemanticTokensDelta(
    td_semanticTokens_full_delta::request const &req) {
  td_semanticTokens_full_delta::response rsp;
  rsp.id = req.id;

  std::string const normPath = fblang::normalizePath(
      req.params.textDocument.uri.GetAbsolutePath().path());
  if (!settingsForDocument(normPath).semanticTokensOn) {
    // Feature gate (M11): a full-empty variant with a fresh resultId — never
    // null, so a client holding a stale baseline keeps a consistent view.
    SemanticTokensOrDelta out;
    out.tokens.emplace();
    out.resultId.emplace(storeDelta({}));
    rsp.result.emplace(std::move(out));
    return rsp;
  }

  std::shared_ptr<fblang::AnalysisCache::Entry const> const cached =
      cachedRequestAnalysis(req.params.textDocument.uri);
  if (!cached) {
    return rsp;
  }
  std::string_view const content = cached->content;
  fblang::AnalyzedDoc const &doc = cached->analysis;
  std::vector<std::int32_t> const current =
      fblang::encodeTokenData(fblang::semanticTokens(doc, content));

  std::vector<std::int32_t> previous;
  bool known = false;
  {
    std::lock_guard<std::mutex> const lock(deltaMutex_);
    auto const it = deltaCache_.find(req.params.previousResultId);
    if (it != deltaCache_.end()) {
      previous = it->second;
      known = true;
    }
  }

  SemanticTokensOrDelta out;
  if (!known) {
    // Unknown id — a stale id, a range resultId (never cached), or a fresh
    // client: fall back to a full response with a new baseline.
    out.tokens.emplace(current);
    out.resultId.emplace(storeDelta(current));
  } else if (previous == current) {
    // Identical content: an empty edit list, resultId unchanged.
    out.edits.emplace();
    out.resultId.emplace(req.params.previousResultId);
  } else {
    out.edits.emplace();
    out.edits->push_back(diffTokenData(previous, current));
    out.resultId.emplace(storeDelta(current));
  }
  rsp.result.emplace(std::move(out));
  return rsp;
}

td_semanticTokens_range::response FreeBasicServer::onSemanticTokensRange(
    td_semanticTokens_range::request const &req) {
  td_semanticTokens_range::response rsp;
  rsp.id = req.id;

  std::string const normPath = fblang::normalizePath(
      req.params.textDocument.uri.GetAbsolutePath().path());
  if (!settingsForDocument(normPath).semanticTokensOn) {
    // Feature gate (M11): an empty data set with a fresh, *uncached* resultId
    // — a range result is never stored, exactly like the enabled path, so it
    // can never be diffed against.
    SemanticTokens tokens;
    {
      std::lock_guard<std::mutex> const lock(deltaMutex_);
      tokens.resultId.emplace("st" + std::to_string(nextResultId_++));
    }
    rsp.result.emplace(std::move(tokens));
    return rsp;
  }

  std::shared_ptr<fblang::AnalysisCache::Entry const> const cached =
      cachedRequestAnalysis(req.params.textDocument.uri);
  if (!cached) {
    return rsp;
  }
  std::string_view const content = cached->content;
  fblang::AnalyzedDoc const &doc = cached->analysis;
  std::vector<fblang::SemanticTokenEntry> const inRange = fblang::filterTokens(
      fblang::semanticTokens(doc, content), req.params.range.start.line,
      req.params.range.end.line);

  SemanticTokens tokens;
  tokens.data = fblang::encodeTokenData(inRange);
  // Fresh resultId, but never stored: a delta diffed against a viewport-scoped
  // set would corrupt the client. An unknown id already falls back to `full`.
  {
    std::lock_guard<std::mutex> const lock(deltaMutex_);
    tokens.resultId.emplace("st" + std::to_string(nextResultId_++));
  }
  rsp.result.emplace(std::move(tokens));
  return rsp;
}

td_inlayHint::response
FreeBasicServer::onInlayHint(td_inlayHint::request const &req) {
  td_inlayHint::response rsp;
  rsp.id = req.id;

  std::string const normPath = fblang::normalizePath(
      req.params.textDocument.uri.GetAbsolutePath().path());
  if (!settingsForDocument(normPath).inlayHintsOn) {
    return rsp; // feature gate (M11): advertised but off — empty result
  }

  std::shared_ptr<fblang::AnalysisCache::Entry const> const cached =
      cachedRequestAnalysis(req.params.textDocument.uri);
  if (!cached) {
    return rsp;
  }
  std::string_view const content = cached->content;
  fblang::AnalyzedDoc const &doc = cached->analysis;
  for (fblang::InlayHintItem const &item : fblang::inlayHints(doc, content)) {
    lsPosition const pos = fblang::utf16Position(content, item.bytePos);
    if (pos.line < req.params.range.start.line ||
        pos.line > req.params.range.end.line) {
      continue; // outside the requested viewport
    }
    lsInlayHint hint;
    hint.position = pos;
    hint.label = item.label;
    rsp.result.push_back(std::move(hint));
  }
  return rsp;
}

std::string FreeBasicServer::storeDelta(std::vector<std::int32_t> const &data) {
  std::lock_guard<std::mutex> const lock(deltaMutex_);
  std::string const id = "st" + std::to_string(nextResultId_++);
  deltaCache_[id] = data;
  deltaOrder_.push_back(id);
  constexpr std::size_t kMaxCachedResults = 64;
  if (deltaCache_.size() > kMaxCachedResults) {
    std::string const oldest = deltaOrder_.front();
    deltaOrder_.erase(deltaOrder_.begin());
    deltaCache_.erase(oldest);
  }
  return id;
}

void FreeBasicServer::publishDiagnostics(
    lsDocumentUri const &uri, std::vector<lsDiagnostic> diagnostics) {
  Notify_TextDocumentPublishDiagnostics::notify publish;
  publish.params.uri = uri;
  publish.params.diagnostics = std::move(diagnostics);
  session_.endpoint().send(publish);
}

std::optional<std::string>
FreeBasicServer::contentForPath(std::filesystem::path const &path) {
  std::shared_ptr<fblang::DocumentContent const> const dc =
      contentForPathAnalysis(path);
  if (!dc) {
    return std::nullopt;
  }
  return dc->content;
}

std::shared_ptr<fblang::DocumentContent const>
FreeBasicServer::contentForPathAnalysis(std::filesystem::path const &path) {
  std::string const normPath = fblang::normalizePath(path);
  // An open buffer is live truth: unsaved edits must drive range conversion
  // (and, via the index, resolution) even before they hit disk. The cache
  // entry was filled by that buffer's didOpen/didChange (insert=false here:
  // request threads never own open-buffer inserts).
  if (std::shared_ptr<WorkingFile> const file =
          workingFiles_.GetFileByFilename(AbsolutePath(path.string()))) {
    return analysisCache_.get(normPath, file->GetContentNoLock(),
                              static_cast<std::uint64_t>(file->version),
                              /*fromBuffer=*/true, /*insert=*/false);
  }
  // Closed file: a disk read, content-addressed and self-warming
  // (fromBuffer=false entries are FIFO-evicted past the cache cap).
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return nullptr;
  }
  std::string const disk{std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>()};
  return analysisCache_.get(normPath, disk, 0, /*fromBuffer=*/false,
                            /*insert=*/true);
}

fblang::AnalysisCache::Stats FreeBasicServer::analysisStats() const {
  return analysisCache_.stats();
}

void FreeBasicServer::ensureRequestClosure(std::string const &normPath) {
  std::shared_ptr<fblang::WorkspaceIndex> const index = indexFor(normPath);
  ensureRequestClosure(normPath, index);
}

void FreeBasicServer::ensureRequestClosure(
    std::string const &normPath,
    std::shared_ptr<fblang::WorkspaceIndex> const &index) {
  if (!index) {
    return;
  }
  // On-demand closure for a document outside the workspace root: the resolver
  // serves each closure file from the live open buffer when the client has
  // one, else from disk, both through the content-addressed analysis cache
  // (repeat requests reuse the single analysis per (path, content)). The
  // snapshot is captured into the callback so the walk stays pinned even if
  // the serving index is re-rooted mid-ensure.
  index->ensureClosure(normPath, [this, index](std::string const &p) {
    std::shared_ptr<fblang::DocumentContent const> const dc =
        contentForPathAnalysis(p);
    if (!dc) {
      return std::shared_ptr<fblang::IndexedFile const>();
    }
    std::uint64_t mtime = 0;
    std::uint64_t size = 0;
    fblang::statFile(p, &mtime, &size);
    bool const fromBuffer =
        workingFiles_.GetFileByFilename(AbsolutePath(p)) != nullptr;
    return std::make_shared<fblang::IndexedFile const>(
        fblang::indexedFileFromAnalysis(p, mtime, size, dc->analysis,
                                        index->root(), !fromBuffer,
                                        index->includeDirs()));
  });
}

std::shared_ptr<fblang::AnalysisCache::Entry const>
FreeBasicServer::cachedRequestAnalysis(lsDocumentUri const &uri) {
  std::shared_ptr<WorkingFile> const file =
      workingFiles_.GetFileByFilename(uri.GetAbsolutePath());
  if (!file) {
    return nullptr;
  }
  return analysisCache_.get(fblang::normalizePath(uri.GetAbsolutePath().path()),
                            file->GetContentNoLock(),
                            static_cast<std::uint64_t>(file->version),
                            /*fromBuffer=*/true, /*insert=*/false);
}

fblang::CrossDecl
FreeBasicServer::resolveAtOrAcross(fblang::AnalyzedDoc const &doc,
                                   std::string const &normalizedPath,
                                   std::uint32_t off) {
  // A requesting document opened from outside the workspace root has no scan
  // entry; build its include closure on demand so cross-file resolution serves
  // it (see ensureClosure). The local snapshot pins the serving index for the
  // resolution walk; the returned CrossDecl pins it beyond (resolve.h).
  std::shared_ptr<fblang::WorkspaceIndex> const index =
      indexFor(normalizedPath);
  ensureRequestClosure(normalizedPath, index);
  if (index) {
    return fblang::resolveAcross(doc, normalizedPath, off, *index);
  }
  if (fblang::Symbol const *const local = fblang::resolveAt(doc, off)) {
    return fblang::CrossDecl{nullptr, local};
  }
  return {};
}
