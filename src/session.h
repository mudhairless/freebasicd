#pragma once

#include "LibLsp/lsp/LanguageSession.h"
#include "LibLsp/lsp/client/registerCapability.h"
#include "LibLsp/lsp/general/exit.h"
#include "LibLsp/lsp/general/initialize.h"
#include "LibLsp/lsp/general/initialized.h"
#include "LibLsp/lsp/general/lsTextDocumentClientCapabilities.h"
#include "LibLsp/lsp/general/shutdown.h"
#include "LibLsp/lsp/lsAny.h"
#include "LibLsp/lsp/textDocument/SemanticTokens.h"
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
#include "LibLsp/lsp/textDocument/inlayHint.h"
#include "LibLsp/lsp/textDocument/prepareRename.h"
#include "LibLsp/lsp/textDocument/publishDiagnostics.h"
#include "LibLsp/lsp/textDocument/references.h"
#include "LibLsp/lsp/textDocument/rename.h"
#include "LibLsp/lsp/textDocument/signature_help.h"

#include "LibLsp/lsp/working_files.h"

// WorkspaceSymbolParams is defined in the extension headers, not lsp/symbol.h;
// include it or the wp_symbol request type instantiates with an incomplete
// params type and the runtime parser can never build `workspace/symbol`.
#include "LibLsp/lsp/extention/jdtls/WorkspaceSymbolParams.h"
#include "LibLsp/lsp/workspace/did_change_watched_files.h"
#include "LibLsp/lsp/workspace/symbol.h"

#include "analysis_cache.h"
#include "index.h"
#include "resolve.h"
#include "semantic_tokens_lsp.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class FreeBasicServer {
public:
  explicit FreeBasicServer(lsp::LanguageSession &session);

  void registerHandlers();
  void setExitHandler(std::function<void()> exitHandler);

  // Test hook: a snapshot of the content-addressed analysis-cache counters,
  // used by session_integration to assert that repeat requests reuse the one
  // cached analysis instead of re-parsing the buffer.
  fblang::AnalysisCache::Stats analysisStats() const;

private:
  lsp::LanguageSession &session_;
  std::function<void()> exitHandler_;

  WorkingFiles workingFiles_;

  // Client negotiated `workspace/didChangeWatchedFiles` in initialize; a
  // dynamic client is registered via client/registerCapability on the
  // `initialized` notification, a static client is served the watchers in
  // the initialize reply.
  bool watchedFilesDynamic_ = false;

  // In-memory per-workspace symbol index (M4); null until a workspace root
  // is known (initialize or first opened file). Never persisted to disk.
  std::unique_ptr<fblang::WorkspaceIndex> index_;

  // Content-addressed analysis memo (M10). Every request-path handler reads
  // the open buffer through this, and the cross-file providers
  // (references/rename closures, remote range conversion) share it with the
  // open buffers, so one (path, content) pair is analyzed at most once per
  // version and repeat requests never re-parse.
  fblang::AnalysisCache analysisCache_;

  // Client-provided workspace root (`rootUri` / `workspaceFolders`), kept so a
  // later didOpen can narrow it to the opened document's project (see
  // onDidOpen); empty when the client sent none.
  std::filesystem::path sessionRoot_;

  // Semantic-tokens delta cache (M9). Only `full` results are stored: a
  // `range` result carries a fresh resultId but is never cached, so a delta can
  // never be diffed against a viewport-scoped token set. Handlers run on the
  // pool concurrently, so every access holds deltaMutex_; responses are built
  // from a copied snapshot, lock-free.
  std::mutex deltaMutex_;
  std::unordered_map<std::string, std::vector<std::int32_t>> deltaCache_;
  std::vector<std::string> deltaOrder_; // insertion order, for eviction
  std::uint64_t nextResultId_ = 1;

  void ensureWorkspaceIndex(std::filesystem::path const &root);
  std::optional<std::filesystem::path>
  chooseIndexRoot(std::filesystem::path const &openedFile);

  void onInitialized(Notify_InitializedNotification::notify const &notify);
  void
  onWatchedFiles(Notify_WorkspaceDidChangeWatchedFiles::notify const &notify);

  td_shutdown::response onShutdown(td_shutdown::request const &req);
  void onDidOpen(Notify_TextDocumentDidOpen::notify &notify);
  void onDidChange(Notify_TextDocumentDidChange::notify const &notify);
  void onDidSave(Notify_TextDocumentDidSave::notify const &notify);
  void onDidClose(Notify_TextDocumentDidClose::notify const &notify);

  td_initialize::response onInitialize(td_initialize::request const &req);
  td_symbol::response onDocumentSymbol(td_symbol::request const &req);
  td_hover::response onHover(td_hover::request const &req);
  td_foldingRange::response onFoldingRange(td_foldingRange::request const &req);
  td_definition::response onDefinition(td_definition::request const &req);
  td_references::response onReferences(td_references::request const &req);
  td_highlight::response onHighlight(td_highlight::request const &req);
  td_completion::response onCompletion(td_completion::request const &req);
  td_signatureHelp::response
  onSignatureHelp(td_signatureHelp::request const &req);
  td_prepareRename::response
  onPrepareRename(td_prepareRename::request const &req);
  td_rename::response onRename(td_rename::request const &req);
  wp_symbol::response onWorkspaceSymbol(wp_symbol::request const &req);
  td_semanticTokens_full::response
  onSemanticTokensFull(td_semanticTokens_full::request const &req);
  td_semanticTokens_full_delta::response
  onSemanticTokensDelta(td_semanticTokens_full_delta::request const &req);
  td_semanticTokens_range::response
  onSemanticTokensRange(td_semanticTokens_range::request const &req);
  td_inlayHint::response onInlayHint(td_inlayHint::request const &req);

  // Allocate a fresh resultId ("st<counter>") and record `data` under it as
  // the current delta baseline, evicting the oldest entry past a fixed cap.
  std::string storeDelta(std::vector<std::int32_t> const &data);

  void reparseAndPublish(std::shared_ptr<WorkingFile> const &file,
                         lsDocumentUri const &uri);
  void publishDiagnostics(lsDocumentUri const &uri,
                          std::vector<lsDiagnostic> diagnostics);

  // Live content of `path`: the open buffer when the client has one on the
  // wire, else the file on disk. Lifts the workspace/symbol ifstream pattern
  // so every remote reply converts ranges against the target's own content.
  std::optional<std::string> contentForPath(std::filesystem::path const &path);

  // Analysis-carrying variant of contentForPath: content+analysis for a file,
  // fed through the content-addressed cache. Open buffers and closed files
  // both go through it, so repeated reads (references/rename closures, range
  // conversion) reuse the single analysis per (path, content).
  std::shared_ptr<fblang::DocumentContent const>
  contentForPathAnalysis(std::filesystem::path const &path);

  // Cached analysis of the request document's open buffer, or nullptr when the
  // file is not open. Handlers read this instead of re-analyzing per request;
  // a miss analyzes locally without inserting (the didChange fill owns the
  // open-buffer inserts).
  std::shared_ptr<fblang::AnalysisCache::Entry const>
  cachedRequestAnalysis(lsDocumentUri const &uri);

  // resolveAcross over the workspace, falling back to in-file-only resolution
  // when no index exists (single-file mode). The returned CrossDecl has
  // `file == nullptr` in both cases; the caller keeps `doc` alive.
  fblang::CrossDecl resolveAtOrAcross(fblang::AnalyzedDoc const &doc,
                                      std::string const &normalizedPath,
                                      std::uint32_t off);

  // A document opened from outside the workspace root (an editor working on a
  // sibling project beside the client root) has no workspace-scan entry: build
  // its #include closure on demand so cross-file resolution serves it too.
  // Resolution-only — the entries never surface through workspace/symbol (see
  // WorkspaceIndex::ensureClosure).
  void ensureRequestClosure(std::string const &normalizedPath);
};
