#include "session.h"

#include "index.h"
#include "inlay_hints.h"
#include "language.h"
#include "lexer.h"
#include "parser.h"
#include "resolve.h"
#include "semantic_tokens.h"
#include "symbols.h"
#include "utf16.h"

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
#include "LibLsp/lsp/workspace/did_change_watched_files.h"
#include "LibLsp/lsp/workspace/symbol.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
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

std::vector<lsDiagnostic> convertDiagnostics(std::string_view content,
                                             fblang::ParseResult const &parse) {
  std::vector<lsDiagnostic> out;
  out.reserve(parse.diagnostics.size());
  for (auto const &d : parse.diagnostics) {
    lsDiagnostic diag;
    diag.range = fblang::utf16Range(content, d.range.beg, d.range.end);
    diag.severity = static_cast<lsDiagnosticSeverity>(d.severity);
    if (!d.code.empty()) {
      diag.code.emplace(
          std::make_pair<optional<std::string>, optional<int>>(d.code, {}));
    }
    diag.source.emplace("freebasiclsp");
    diag.message = d.message;
    out.push_back(std::move(diag));
  }
  return out;
}

// Unresolved `#include`/`#include once` literals of an indexed entry become
// `include-not-found` Errors at the literal's own range. Only the open
// buffer's own edges are diagnosed (M6); inter-file closure diagnostics wait
// for pull diagnostics (M14).
void appendIncludeDiagnostics(std::string_view content,
                              fblang::IndexedFile const &entry,
                              std::vector<lsDiagnostic> *out) {
  for (auto const &e : entry.includes) {
    if (!e.target.empty()) {
      continue;
    }
    if (e.targetRange.beg >= e.targetRange.end ||
        e.targetRange.end > content.size()) {
      continue; // a directive with no filename literal
    }
    lsDiagnostic diag;
    diag.range =
        fblang::utf16Range(content, e.targetRange.beg, e.targetRange.end);
    diag.severity = static_cast<lsDiagnosticSeverity>(fblang::Severity::Error);
    diag.code.emplace(std::make_pair<optional<std::string>, optional<int>>(
        std::string("include-not-found"), {}));
    diag.source.emplace("freebasiclsp");
    diag.message = "include file not found: \"" + e.literal + "\"";
    out->push_back(std::move(diag));
  }
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
  return !fblang::isReservedWord(fblang::toLowerChars(name.substr(0, baseLen)));
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

void FreeBasicServer::ensureWorkspaceIndex(std::filesystem::path const &root) {
  if (root.empty()) {
    return;
  }
  std::string const normRoot = fblang::normalizePath(root);
  (void)std::fprintf(stderr, "[freebasiclsp] workspace root: %s\n",
                     normRoot.c_str());
  if (index_ && fblang::normalizePath(index_->root()) == normRoot) {
    return;
  }
  if (index_) {
    index_->close();
  }
  index_ = std::make_unique<fblang::WorkspaceIndex>(root);
  index_->open();
  index_->scan(true);
}

std::optional<std::filesystem::path>
FreeBasicServer::chooseIndexRoot(std::filesystem::path const &openedFile) {
  if (!sessionRoot_.empty()) {
    // A client root that is itself a project root is used as-is.
    if (isProjectRoot(sessionRoot_)) {
      return sessionRoot_;
    }
    // A broad client root (no version-control marker of its own, e.g. an
    // editor that reports the home directory as the workspace) is narrowed
    // to the opened document's project, so sibling FreeBASIC projects under
    // it are never swept into the index.
    if (std::optional<std::filesystem::path> const project =
            nearestProjectRoot(openedFile, sessionRoot_)) {
      return project;
    }
    return sessionRoot_;
  }
  // No client root: single-file mode, the workspace is the file's directory.
  return openedFile.parent_path();
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

  // Watched-file negotiation: a client with
  // workspace.didChangeWatchedFiles.dynamicRegistration gets the watcher
  // via client/registerCapability on `initialized`; everyone else is served
  // the static watchers right here.
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
    rsp.result.capabilities.workspace.emplace();
    rsp.result.capabilities.workspace->didChangeWatchedFiles.emplace();
    rsp.result.capabilities.workspace->didChangeWatchedFiles->watchers
        .push_back(std::move(watcher));
  }

  // Workspace root: rootUri wins over workspaceFolders; fall back to the
  // first opened file when neither is present (single-file mode).
  std::string rootPath;
  if (req.params.rootUri) {
    rootPath = req.params.rootUri->GetAbsolutePath().path();
  }
  if (rootPath.empty() && req.params.workspaceFolders &&
      !req.params.workspaceFolders->empty()) {
    rootPath = (*req.params.workspaceFolders)[0].uri.GetAbsolutePath().path();
  }
  if (!rootPath.empty()) {
    sessionRoot_ = rootPath;
    // Create the index now only when the client root is itself a project
    // root. A broad root (e.g. the home directory, which hosts several
    // sibling projects) is narrowed to the opened document's project on the
    // first didOpen so unrelated trees are never scanned or cached.
    if (isProjectRoot(rootPath)) {
      ensureWorkspaceIndex(rootPath);
    } else {
      (void)std::fprintf(
          stderr,
          "[freebasiclsp] workspace root %s has no version-control "
          "marker; index scope deferred to the first opened document\n",
          rootPath.c_str());
    }
  }

  return rsp;
}

td_shutdown::response
FreeBasicServer::onShutdown(td_shutdown::request const &req) {
  td_shutdown::response rsp;
  rsp.id = req.id;

  if (index_) {
    index_->close();
  }

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
  // The registered glob (**/*.{bas,bi}) already scopes the events; re-statting
  // the whole root converges any external .bi edit. The debounce and its
  // async scan live in the index, so the notification FIFO thread returns at
  // once regardless of workspace size. Event details are intentionally
  // ignored: a full-root scan is authoritative and cheap for FB-sized files.
  (void)notify;
  if (index_) {
    index_->watchedFilesChanged();
  }
}

void FreeBasicServer::onDidOpen(Notify_TextDocumentDidOpen::notify &notify) {
  std::filesystem::path const openedFile =
      notify.params.textDocument.uri.GetAbsolutePath().path();
  if (std::optional<std::filesystem::path> const root =
          chooseIndexRoot(openedFile)) {
    // A broad client root (no VCS marker) is re-evaluated on every open so
    // switching to a sibling project re-roots the index to that project.
    bool const deferredBroadRoot =
        !sessionRoot_.empty() && !isProjectRoot(sessionRoot_);
    bool const alreadyRooted =
        index_ &&
        fblang::normalizePath(index_->root()) == fblang::normalizePath(*root);
    if (!index_ || (deferredBroadRoot && !alreadyRooted)) {
      ensureWorkspaceIndex(*root);
    }
  }
  std::shared_ptr<WorkingFile> const file =
      workingFiles_.OnOpen(notify.params.textDocument);
  if (!file) {
    return;
  }
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
  // The buffer's cache entries are live-truth pinned to the buffer's bytes;
  // drop them so a later didOpen of the same file starts fresh (the disk is
  // master for closed files).
  analysisCache_.removePath(fblang::normalizePath(
      notify.params.textDocument.uri.GetAbsolutePath().path()));
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

  if (index_) {
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
          normPath, mtime, size, doc, index_->root(), false);
      appendIncludeDiagnostics(content, entry, &diags);
      index_->upsert(std::move(entry));
    }
  }

  publishDiagnostics(uri, std::move(diags));
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

  // No declaration under the cursor: hover on the enclosing declaration for
  // context. Declaration-scope (Scope) nodes are structure, not symbols, so
  // climb past them — a Scope node's `name` is the opener word ("if") and was
  // being shown as the whole hover.
  fblang::Symbol const *encl = deepestSymbolAt(doc.parse.roots, offset);
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

  collect(doc, normPath);
  if (index_) {
    for (std::string const &closurePath :
         index_->transitiveIncludes(normPath)) {
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

  // Collision guard: renaming into a key that an unrelated module-scope
  // declaration of the requesting file or its include closure already owns
  // would fold two declarations into one textual module. The renamed symbol
  // itself (same key, or the same declaration under the new key) is exempt.
  if (index_ && newKey != target.decl->key) {
    std::vector<std::string> guardPaths = {normPath};
    for (std::string const &p : index_->transitiveIncludes(normPath)) {
      guardPaths.push_back(p);
    }
    for (std::string const &p : guardPaths) {
      std::shared_ptr<fblang::IndexedFile const> const f = index_->fileAt(p);
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
      doc, normPath, offset, index_.get(), [this](std::string const &p) {
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
  if (index_) {
    bool const storageGated = fblang::insideProcedureBody(
        doc.parse, fblang::innermostScope(doc.parse, offset));
    std::string const normPath = fblang::normalizePath(
        req.params.textDocument.uri.GetAbsolutePath().path());
    for (std::string const &closurePath :
         index_->transitiveIncludes(normPath)) {
      std::shared_ptr<fblang::IndexedFile const> const closure =
          index_->fileAt(closurePath);
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
  if (!index_) {
    return rsp;
  }

  std::string const query = fblang::toLowerChars(req.params.query);

  struct Match {
    fblang::Symbol const *sym;
    std::string container;
  };
  struct FileMatches {
    fblang::IndexedFile const *file;
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

  for (auto const &file : index_->snapshot()) {
    std::vector<Match> matches;
    for (auto const &root : file->roots) {
      collect(root, {}, matches);
    }
    if (matches.empty()) {
      continue;
    }
    hits.push_back(FileMatches{file.get(), std::move(matches)});
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

td_semanticTokens_full::response FreeBasicServer::onSemanticTokensFull(
    td_semanticTokens_full::request const &req) {
  td_semanticTokens_full::response rsp;
  rsp.id = req.id;

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
                                   std::uint32_t off) const {
  if (index_) {
    return fblang::resolveAcross(doc, normalizedPath, off, *index_);
  }
  if (fblang::Symbol const *const local = fblang::resolveAt(doc, off)) {
    return fblang::CrossDecl{nullptr, local};
  }
  return {};
}
