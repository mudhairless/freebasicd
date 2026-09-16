#pragma once

#include "LibLsp/lsp/LanguageSession.h"
#include "LibLsp/lsp/client/registerCapability.h"
#include "LibLsp/lsp/general/exit.h"
#include "LibLsp/lsp/general/initialize.h"
#include "LibLsp/lsp/general/initialized.h"
#include "LibLsp/lsp/general/lsTextDocumentClientCapabilities.h"
#include "LibLsp/lsp/general/shutdown.h"
#include "LibLsp/lsp/lsAny.h"
#include "LibLsp/lsp/textDocument/did_change.h"
#include "LibLsp/lsp/textDocument/did_close.h"
#include "LibLsp/lsp/textDocument/did_open.h"
#include "LibLsp/lsp/textDocument/did_save.h"
#include "LibLsp/lsp/textDocument/completion.h"
#include "LibLsp/lsp/textDocument/declaration_definition.h"
#include "LibLsp/lsp/textDocument/document_symbol.h"
#include "LibLsp/lsp/textDocument/foldingRange.h"
#include "LibLsp/lsp/textDocument/highlight.h"
#include "LibLsp/lsp/textDocument/hover.h"
#include "LibLsp/lsp/textDocument/publishDiagnostics.h"
#include "LibLsp/lsp/textDocument/prepareRename.h"
#include "LibLsp/lsp/textDocument/references.h"
#include "LibLsp/lsp/textDocument/rename.h"
#include "LibLsp/lsp/textDocument/signature_help.h"

#include "LibLsp/lsp/working_files.h"

// WorkspaceSymbolParams is defined in the extension headers, not lsp/symbol.h;
// include it or the wp_symbol request type instantiates with an incomplete
// params type and the runtime parser can never build `workspace/symbol`.
#include "LibLsp/lsp/workspace/symbol.h"
#include "LibLsp/lsp/extention/jdtls/WorkspaceSymbolParams.h"
#include "LibLsp/lsp/workspace/did_change_watched_files.h"

#include "index.h"
#include "resolve.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class FreeBasicServer
{
public:
    explicit FreeBasicServer(lsp::LanguageSession& session);

    void registerHandlers();
    void setExitHandler(std::function<void()> exitHandler);

private:
    lsp::LanguageSession& session_;
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

    // Client-provided workspace root (`rootUri` / `workspaceFolders`), kept so a
    // later didOpen can narrow it to the opened document's project (see
    // onDidOpen); empty when the client sent none.
    std::filesystem::path sessionRoot_;

    void ensureWorkspaceIndex(std::filesystem::path const& root);
    std::optional<std::filesystem::path> chooseIndexRoot(std::filesystem::path const& openedFile);

    void onInitialized(Notify_InitializedNotification::notify const& notify);
    void onWatchedFiles(Notify_WorkspaceDidChangeWatchedFiles::notify const& notify);

    td_shutdown::response onShutdown(td_shutdown::request const& req);
    void onDidOpen(Notify_TextDocumentDidOpen::notify& notify);
    void onDidChange(Notify_TextDocumentDidChange::notify const& notify);
    void onDidSave(Notify_TextDocumentDidSave::notify const& notify);
    void onDidClose(Notify_TextDocumentDidClose::notify const& notify);

    td_initialize::response onInitialize(td_initialize::request const& req);
    td_symbol::response onDocumentSymbol(td_symbol::request const& req);
    td_hover::response onHover(td_hover::request const& req);
    td_foldingRange::response onFoldingRange(td_foldingRange::request const& req);
    td_definition::response onDefinition(td_definition::request const& req);
    td_references::response onReferences(td_references::request const& req);
    td_highlight::response onHighlight(td_highlight::request const& req);
    td_completion::response onCompletion(td_completion::request const& req);
    td_signatureHelp::response onSignatureHelp(td_signatureHelp::request const& req);
    td_prepareRename::response onPrepareRename(td_prepareRename::request const& req);
    td_rename::response onRename(td_rename::request const& req);
    wp_symbol::response onWorkspaceSymbol(wp_symbol::request const& req);

    void reparseAndPublish(std::shared_ptr<WorkingFile> const& file, lsDocumentUri const& uri);
    void publishDiagnostics(lsDocumentUri const& uri, std::vector<lsDiagnostic> diagnostics);

    // Live content of `path`: the open buffer when the client has one on the
    // wire, else the file on disk. Lifts the workspace/symbol ifstream pattern
    // so every remote reply converts ranges against the target's own content.
    std::optional<std::string> contentForPath(std::filesystem::path const& path);

    // resolveAcross over the workspace, falling back to in-file-only resolution
    // when no index exists (single-file mode). The returned CrossDecl has
    // `file == nullptr` in both cases; the caller keeps `doc` alive.
    fblang::CrossDecl resolveAtOrAcross(fblang::AnalyzedDoc const& doc,
                                        std::string const& normalizedPath,
                                        std::uint32_t off) const;
};