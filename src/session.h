/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

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
#include "LibLsp/lsp/textDocument/callHierarchy.h"
#include "LibLsp/lsp/textDocument/code_action.h"
#include "LibLsp/lsp/textDocument/code_lens.h"
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
#include "LibLsp/lsp/textDocument/implementation.h"
#include "LibLsp/lsp/textDocument/inlayHint.h"
#include "LibLsp/lsp/textDocument/prepareRename.h"
#include "LibLsp/lsp/textDocument/publishDiagnostics.h"
#include "LibLsp/lsp/textDocument/references.h"
#include "LibLsp/lsp/textDocument/rename.h"
#include "LibLsp/lsp/textDocument/selectionRange.h"
#include "LibLsp/lsp/textDocument/signature_help.h"
#include "LibLsp/lsp/textDocument/type_definition.h"

// The type-hierarchy request types are local (src/type_hierarchy_lsp.h): the
// vendored td_typeHierarchy is typed as a bare TypeHierarchyItem where the
// protocol says TypeHierarchyItem[] | null, and supertypes/subtypes ship no
// request type at all.
#include "type_hierarchy_lsp.h"

#include "LibLsp/lsp/working_files.h"

// WorkspaceSymbolParams is defined in the extension headers, not lsp/symbol.h;
// include it or the wp_symbol request type instantiates with an incomplete
// params type and the runtime parser can never build `workspace/symbol`.
#include "LibLsp/lsp/extention/jdtls/WorkspaceSymbolParams.h"
#include "LibLsp/lsp/workspace/didChangeWorkspaceFolders.h"
#include "LibLsp/lsp/workspace/did_change_configuration.h"
#include "LibLsp/lsp/workspace/did_change_watched_files.h"
#include "LibLsp/lsp/workspace/execute_command.h"
#include "LibLsp/lsp/workspace/symbol.h"

#include "analysis_cache.h"
#include "call_hierarchy.h"
#include "call_hierarchy_lsp.h"
#include "code_actions.h"
#include "code_lens.h"
#include "index.h"
#include "resolve.h"
#include "selection.h"
#include "selection_lsp.h"
#include "semantic_tokens_lsp.h"
#include "settings.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class FreeBasicServer {
public:
  explicit FreeBasicServer(lsp::LanguageSession &session);

  // Why an index root was chosen, so stderr can say whether the server took
  // the client's root as-is or found a project root itself (and from which
  // signal) instead of the one it was passed.
  struct IndexRootChoice {
    std::filesystem::path root;
    enum class Reason {
      ClientRoot,     // the client-provided root, used as-is
      RegisteredRoot, // a registered workspace-folder root containing the file
      VcsMarker,      // nearest version-control-marked ancestor within/at it
      ConfigFile,     // nearest ancestor holding a freebasicd.toml
      SourceLayout,   // no VCS/config marker; ancestor holding a source/include
                      // child
      SingleFile,     // no client root; the opened file's own directory
    } reason = Reason::ClientRoot;
  };

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

  // Normalized paths of the buffers currently open on the wire, tracked in
  // onDidOpen/onDidClose (WorkingFiles offers no iteration API). Only touched
  // on the LSP notification FIFO thread (didOpen/didClose/didChange-
  // Configuration), which serializes them; request handlers never read it.
  std::set<std::string> openFiles_;

  // Client negotiated `workspace/didChangeWatchedFiles` in initialize; a
  // dynamic client is registered via client/registerCapability on the
  // `initialized` notification, a static client is served the watchers in
  // the initialize reply.
  bool watchedFilesDynamic_ = false;

  // Pull diagnostics (M14): the client advertised `textDocument.diagnostic`
  // (LSP 3.17), so the server advertises `diagnosticProvider`, answers
  // `textDocument/diagnostic` and `workspace/diagnostic`, and stops pushing
  // `publishDiagnostics` — the spec asks a server to prefer pull once it is
  // negotiated, and a client that supports both must never see the same
  // diagnostics twice. A client without the capability keeps the push path
  // exactly as before. Only touched on the notification FIFO thread at
  // initialize; request handlers read it lock-free.
  bool pullDiagnostics_ = false;
  // The client can receive the `workspace/diagnostic/refresh` hint
  // (workspace.diagnostic.refreshSupport): the server asks it to re-pull
  // when diagnostics changed out-of-band of a pull. Without it the client
  // re-pulls on its own schedule.
  bool diagnosticsRefreshSupported_ = false;
  // The client accepts `relatedDocuments` on a full report
  // (textDocument.diagnostic.relatedDocumentSupport): the include closure a
  // pull on one document can surface alongside its own problems.
  bool relatedDocumentSupport_ = false;

  // In-memory per-workspace symbol indexes (M11): one index per workspace
  // root, keyed by normalized root. Registered client workspace folders that
  // bear a root marker (a version-control marker or freebasicd.toml) are
  // indexed at initialize and tracked in workspaceFolderRoots_; broad folders
  // defer to per-document detection; single-file mode roots at the opened
  // file's project or its own directory. Handlers snapshot a shared_ptr under
  // indexesMutex_ and run their queries against it, so a concurrent folder
  // add/remove or re-root sweep never invalidates an in-flight request.
  // Nothing is ever persisted to disk.
  mutable std::mutex indexesMutex_;
  std::map<std::string, std::shared_ptr<fblang::WorkspaceIndex>> indexes_;
  // Registered client workspace folders, in registration order.
  std::vector<std::filesystem::path> workspaceFolders_;
  // Normalized roots of the registered folders that are themselves workspace
  // roots (version-control marker or config file): the priority-0 candidate
  // set for a file opened inside them (multi-folder isolation).
  std::set<std::string> workspaceFolderRoots_;

  // Content-addressed analysis memo (M10). Every request-path handler reads
  // the open buffer through this, and the cross-file providers
  // (references/rename closures, remote range conversion) share it with the
  // open buffers, so one (path, content) pair is analyzed at most once per
  // version and repeat requests never re-parse.
  fblang::AnalysisCache analysisCache_;

  // Client-provided workspace root (`rootUri`, or the first workspace folder
  // when no rootUri is sent), kept so a later didOpen can narrow a broad root
  // to the opened document's project (see chooseIndexRoot / onDidOpen); empty
  // when the client sent none. Additional workspace folders are tracked in
  // workspaceFolders_ / workspaceFolderRoots_ above.
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
  IndexRootChoice chooseIndexRoot(std::filesystem::path const &openedFile);

  // The index serving `normalizedPath`: the deepest index whose root contains
  // it, falling back to the session-root index for documents opened outside
  // every index root (their on-demand closure is hosted there, resolution-only
  // — never surfaced by workspace/symbol). Null only in single-file mode
  // before a didOpen, or under a deferred broad root no index was ever created
  // for. Handlers snapshot the returned shared_ptr and hold it while their raw
  // result pointers (member access walks) are in use.
  std::shared_ptr<fblang::WorkspaceIndex>
  indexFor(std::string const &normalizedPath) const;
  // Snapshot of every live index, for workspace/symbol aggregation.
  std::vector<std::shared_ptr<fblang::WorkspaceIndex>> allIndexes() const;
  // The settings governing `normalizedPath`: those of its owning index root,
  // or the defaults when no index serves it (single-file mode before any
  // didOpen). Used to gate diagnostics/semantic-tokens/inlay-hints.
  fblang::Settings settingsForDocument(std::string const &normalizedPath) const;
  void closeAllIndexes();
  // Close every index except `keepNormRoot` and the registered marker roots:
  // re-rooting under a broad client root leaves only the focused project's
  // index alive (single-file and multi-folder sessions never call this).
  void dropDetectedIndexesExcept(std::string const &keepNormRoot);
  // The deepest registered marker-root containing `normalizedPath` (priority
  // 0 of chooseIndexRoot), or nullopt.
  std::optional<std::filesystem::path>
  registeredFolderRootContaining(std::string const &normalizedPath) const;

  void onInitialized(Notify_InitializedNotification::notify const &notify);
  void
  onWatchedFiles(Notify_WorkspaceDidChangeWatchedFiles::notify const &notify);
  void onWorkspaceFoldersChanged(
      Notify_WorkspaceDidChangeWorkspaceFolders::notify const &notify);
  // Settings for every index root live in freebasicd.toml files, not in
  // client configuration chunks: the didChangeConfiguration payload is ignored
  // and the notification is only a signal to re-read each root's config file
  // (idempotent — a no-op when nothing changed) and re-apply whatever did.
  void onDidChangeConfiguration(
      Notify_WorkspaceDidChangeConfiguration::notify const &notify);

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
  // Quick fixes (M12). LspCpp models the response as a bare command list, so
  // each fix ships as `Command{title, "", [WorkspaceEdit]}` — the empty command
  // name is the client's cue to apply `arguments[0]` itself — and the kind
  // filter in `context.only` is honored here rather than by the client.
  td_codeAction::response onCodeAction(td_codeAction::request const &req);
  td_semanticTokens_full::response
  onSemanticTokensFull(td_semanticTokens_full::request const &req);
  td_semanticTokens_full_delta::response
  onSemanticTokensDelta(td_semanticTokens_full_delta::request const &req);
  td_semanticTokens_range::response
  onSemanticTokensRange(td_semanticTokens_range::request const &req);
  td_inlayHint::response onInlayHint(td_inlayHint::request const &req);
  // Type go-to (M15): the type a symbol has (`dim v as base_t`, a field inside
  // a type body, the type a `sub t.go()` qualifies) and the far end of a member
  // procedure's declare <-> defined edge. Both answer one location, and both
  // answer nothing rather than a guess when the symbol carries no type.
  td_typeDefinition::response
  onTypeDefinition(td_typeDefinition::request const &req);
  td_implementation::response
  onImplementation(td_implementation::request const &req);
  // Type hierarchy (M15): prepare returns the type under the cursor with its
  // parents and children already filled, and the two follow-ups take an item
  // the client echoes back — identified by `uri` + `selectionRange`, exactly as
  // callHierarchyIdentity is, so `TypeHierarchyItem::data` stays unset.
  td_typeHierarchyPrepare::response
  onPrepareTypeHierarchy(td_typeHierarchyPrepare::request const &req);
  td_typeHierarchySupertypes::response
  onSupertypes(td_typeHierarchySupertypes::request const &req);
  td_typeHierarchySubtypes::response
  onSubtypes(td_typeHierarchySubtypes::request const &req);
  // Expand selection (M13): the token / statement / block / file chain at each
  // requested position, nested outward. One response entry per requested
  // position, which is the mapping the client assumes.
  td_selectionRange::response
  onSelectionRange(td_selectionRange::request const &req);

  // Call hierarchy (M13). All three carry the same item shape, and the two
  // follow-ups take an item the client echoes back from prepare or from an
  // outgoing call — identified by its `uri` + `selectionRange`, which are both
  // in the item and both needed to convert the ranges that come back, so
  // `CallHierarchyItem::data` stays unset (it is optional in the protocol).
  td_prepareCallHierarchy::response
  onPrepareCallHierarchy(td_prepareCallHierarchy::request const &req);
  td_callHierarchyOutgoingCalls::response
  onOutgoingCalls(td_callHierarchyOutgoingCalls::request const &req);
  td_incomingCalls::response
  onIncomingCalls(td_incomingCalls::request const &req);

  // Code lens (M13): one "N references" lens per declaration, plus the command
  // its click sends back. The two are one feature — a lens has no edit field,
  // so `executeCommandProvider` + `workspace/executeCommand` is what makes the
  // annotation clickable at all.
  td_codeLens::response onCodeLens(td_codeLens::request const &req);
  wp_executeCommand::response
  onExecuteCommand(wp_executeCommand::request const &req);

  // Pull diagnostics (M14): `textDocument/diagnostic` answers one document's
  // report (full, or unchanged against the client's previousResultId, with
  // the include closure's reports as relatedDocuments), and
  // `workspace/diagnostic` answers a report per workspace document the client
  // holds or that newly carries diagnostics. `workspace/diagnostic/refresh`
  // is the server->client hint that cached reports are stale.
  td_diagnostic::response
  onDocumentDiagnostic(td_diagnostic::request const &req);
  workspace_diagnostic::response
  onWorkspaceDiagnostic(workspace_diagnostic::request const &req);

  // The `workspace/diagnostic/refresh` hint, sent when pull mode is
  // negotiated, refresh support exists, and a diagnostic-relevant change
  // landed (a buffer edit, a close, a config change, or a watched-files
  // rescan completing). Safe from any thread: the endpoint send is locked.
  void notifyDiagnosticsRefresh();

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

  // One type-hierarchy node per entry of `types`, each converted against the
  // content of the file that declares it — a node's `uri`, `range` and
  // `selectionRange` all describe one file, and the items in a list are
  // routinely in different ones. A type whose file cannot be read is skipped
  // rather than answered at an offset measured against bytes nobody can
  // produce; a client shows a partial hierarchy and can still drill into it,
  // where a wrong range is worse than a missing node.
  std::vector<TypeHierarchyItem>
  hierarchyItems(std::vector<fblang::TypeItem> const &types);

  // Which files a reference walk sweeps. Both name the same declaration; they
  // differ in what the caller is asking about.
  enum class ReferenceScope {
    // The request document plus its forward include closure — one textual
    // module. What `textDocument/references` answers from: the editor asked
    // about this document, so the answer is the references it can see.
    Closure,
    // The same, plus every indexed file whose own closure reaches the
    // declaration's file (resolve.h's `referencingFiles`). What the code lens
    // counts: a header's references are its includers' call sites, so a
    // per-file count would read 0 on every declaration in every header.
    Workspace,
  };

  // Every reference site of `target` among the candidates `scope` names, sorted
  // by (file, byte offset) and excluding the declaration's own name token. One
  // definition of "the references to this declaration" behind all three of its
  // callers — textDocument/references, the code lens count, and the
  // `freebasicd.showReferences` command — so the number a lens shows and the
  // list its click returns cannot drift apart.
  //
  // A site matches by declaration *identity* (owning file plus name-token
  // range, resolve.h), never by Symbol pointer: every candidate is parsed into
  // its own analysis, so one declaration is a different object in each of them,
  // and a same-named local in a candidate file resolves to its own identity and
  // is correctly not a match. `index` is the serving snapshot the caller holds
  // for the whole walk; `target.file` pins the entry `target.decl` may point
  // into.
  std::vector<fblang::OccurrenceSite>
  referenceSites(fblang::AnalyzedDoc const &doc, std::string const &docPath,
                 fblang::CrossDecl const &target,
                 std::shared_ptr<fblang::WorkspaceIndex> const &index,
                 ReferenceScope scope);

  // A document opened from outside the workspace root (an editor working on a
  // sibling project beside the client root) has no workspace-scan entry: build
  // its #include closure on demand so cross-file resolution serves it too.
  // Resolution-only — the entries never surface through workspace/symbol (see
  // WorkspaceIndex::ensureClosure). The index-holding overload pins the
  // snapshot for the whole walk.
  void ensureRequestClosure(std::string const &normalizedPath);
  void
  ensureRequestClosure(std::string const &normalizedPath,
                       std::shared_ptr<fblang::WorkspaceIndex> const &index);
};
