#pragma once

#include "LibLsp/lsp/LanguageSession.h"
#include "LibLsp/lsp/general/exit.h"
#include "LibLsp/lsp/general/initialize.h"
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
#include "LibLsp/lsp/textDocument/references.h"
#include "LibLsp/lsp/textDocument/signature_help.h"

#include "LibLsp/lsp/working_files.h"

// WorkspaceSymbolParams is defined in the extension headers, not lsp/symbol.h;
// include it or the wp_symbol request type instantiates with an incomplete
// params type and the runtime parser can never build `workspace/symbol`.
#include "LibLsp/lsp/workspace/symbol.h"
#include "LibLsp/lsp/extention/jdtls/WorkspaceSymbolParams.h"

#include "index.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

class FreeBasicServer
{
public:
    explicit FreeBasicServer(lsp::LanguageSession& session);

    void registerHandlers();
    void setExitHandler(std::function<void()> exitHandler);
    void setIndexCacheDir(std::filesystem::path cacheDir);

private:
    lsp::LanguageSession& session_;
    std::function<void()> exitHandler_;

    WorkingFiles workingFiles_;

    // Durable per-workspace symbol index (M4); null until a workspace root is
    // known (initialize or first opened file).
    std::unique_ptr<fblang::WorkspaceIndex> index_;
    std::filesystem::path indexCacheDir_;  // override for tests (default = platform data dir)

    void ensureWorkspaceIndex(std::filesystem::path const& root);

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
    wp_symbol::response onWorkspaceSymbol(wp_symbol::request const& req);

    void reparseAndPublish(std::shared_ptr<WorkingFile> const& file, lsDocumentUri const& uri);
    void publishDiagnostics(lsDocumentUri const& uri, std::vector<lsDiagnostic> diagnostics);
};