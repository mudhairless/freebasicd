#include "session.h"

#include <utility>

FreeBasicServer::FreeBasicServer(lsp::LanguageSession& session) : session_(session)
{
}

void FreeBasicServer::setExitHandler(std::function<void()> exitHandler)
{
    exitHandler_ = std::move(exitHandler);
}

void FreeBasicServer::registerHandlers()
{
    session_.on([this](td_initialize::request const& req) { return onInitialize(req); });
    session_.on([this](td_shutdown::request const& req) { return onShutdown(req); });
    session_.on(
        [this](Notify_Exit::notify const&)
        {
            if (exitHandler_)
            {
                exitHandler_();
            }
        });
    session_.on([this](Notify_TextDocumentDidOpen::notify& notify) { onDidOpen(notify); });
    session_.on([this](Notify_TextDocumentDidChange::notify const& notify) { onDidChange(notify); });
    session_.on([this](Notify_TextDocumentDidSave::notify const& notify) { onDidSave(notify); });
    session_.on([this](Notify_TextDocumentDidClose::notify const& notify) { onDidClose(notify); });
}

td_initialize::response FreeBasicServer::onInitialize(td_initialize::request const& req)
{
    td_initialize::response rsp;
    rsp.id = req.id;

    lsTextDocumentSyncOptions& sync = rsp.result.capabilities.textDocumentSync.emplace().second.emplace();
    sync.openClose = true;
    sync.change = lsTextDocumentSyncKind::Incremental;

    return rsp;
}

td_shutdown::response FreeBasicServer::onShutdown(td_shutdown::request const& req)
{
    td_shutdown::response rsp;
    rsp.id = req.id;

    lsp::Any result;
    result.SetJsonString("null", lsp::Any::kNullType);
    rsp.result = result;

    return rsp;
}

void FreeBasicServer::onDidOpen(Notify_TextDocumentDidOpen::notify& notify)
{
    std::shared_ptr<WorkingFile> file = workingFiles_.OnOpen(notify.params.textDocument);
    if (!file)
    {
        return;
    }
    publishDiagnostics(notify.params.textDocument.uri, {});
}

void FreeBasicServer::onDidChange(Notify_TextDocumentDidChange::notify const& notify)
{
    std::shared_ptr<WorkingFile> file = workingFiles_.OnChange(notify.params);
    if (!file)
    {
        return;
    }
    publishDiagnostics(notify.params.textDocument.uri, {});
}

void FreeBasicServer::onDidSave(Notify_TextDocumentDidSave::notify const& notify)
{
    std::shared_ptr<WorkingFile> file = workingFiles_.OnSave(notify.params.textDocument);
    if (!file)
    {
        return;
    }
    publishDiagnostics(notify.params.textDocument.uri, {});
}

void FreeBasicServer::onDidClose(Notify_TextDocumentDidClose::notify const& notify)
{
    if (!workingFiles_.OnClose(notify.params.textDocument))
    {
        return;
    }
    publishDiagnostics(notify.params.textDocument.uri, {});
}

void FreeBasicServer::publishDiagnostics(lsDocumentUri const& uri, std::vector<lsDiagnostic> diagnostics)
{
    Notify_TextDocumentPublishDiagnostics::notify publish;
    publish.params.uri = uri;
    publish.params.diagnostics = std::move(diagnostics);
    session_.endpoint().send(publish);
}