#include "session.h"

#include "parser.h"
#include "utf16.h"

#include <utility>

namespace {

lsSymbolKind toLspSymbolKind(fblang::SymbolKind kind)
{
    switch (kind)
    {
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
            return lsSymbolKind::Struct;
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
lsDocumentSymbol convertSymbol(std::string_view content, fblang::Symbol const& s)
{
    lsDocumentSymbol out;
    out.name = s.name;
    out.kind = toLspSymbolKind(s.kind);
    out.range = fblang::utf16Range(content, s.range.beg, s.range.end);
    out.selectionRange = fblang::utf16Range(content, s.selection.beg, s.selection.end);
    if (!s.signature.empty())
    {
        out.detail.emplace(s.signature);
    }
    for (auto const& child : s.children)
    {
        if (child.kind == fblang::SymbolKind::Scope)
        {
            continue;
        }
        if (!out.children)
        {
            out.children.emplace();
        }
        out.children->push_back(convertSymbol(content, child));
    }
    return out;
}

std::vector<lsDiagnostic> convertDiagnostics(std::string_view content, fblang::ParseResult const& parse)
{
    std::vector<lsDiagnostic> out;
    out.reserve(parse.diagnostics.size());
    for (auto const& d : parse.diagnostics)
    {
        lsDiagnostic diag;
        diag.range = fblang::utf16Range(content, d.range.beg, d.range.end);
        diag.severity = static_cast<lsDiagnosticSeverity>(d.severity);
        if (!d.code.empty())
        {
            diag.code.emplace(std::make_pair<optional<std::string>, optional<int>>(d.code, {}));
        }
        diag.source.emplace("freebasiclsp");
        diag.message = d.message;
        out.push_back(std::move(diag));
    }
    return out;
}

// Deepest symbol (by range nesting) covering `off`, or nullptr.
fblang::Symbol const* symbolAt(fblang::Symbol const& sym, std::uint32_t off)
{
    if (off < sym.range.beg || off > sym.range.end)
    {
        return nullptr;
    }
    for (auto const& c : sym.children)
    {
        if (fblang::Symbol const* hit = symbolAt(c, off))
        {
            return hit;
        }
    }
    return &sym;
}

fblang::Symbol const* deepestSymbolAt(std::vector<fblang::Symbol> const& roots, std::uint32_t off)
{
    fblang::Symbol const* best = nullptr;
    for (auto const& r : roots)
    {
        if (fblang::Symbol const* hit = symbolAt(r, off))
        {
            best = hit;
        }
    }
    return best;
}

}  // namespace

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
    session_.on([this](td_symbol::request const& req) { return onDocumentSymbol(req); });
    session_.on([this](td_hover::request const& req) { return onHover(req); });
    session_.on([this](td_foldingRange::request const& req) { return onFoldingRange(req); });
}

td_initialize::response FreeBasicServer::onInitialize(td_initialize::request const& req)
{
    td_initialize::response rsp;
    rsp.id = req.id;

    lsTextDocumentSyncOptions& sync = rsp.result.capabilities.textDocumentSync.emplace().second.emplace();
    sync.openClose = true;
    sync.change = lsTextDocumentSyncKind::Incremental;

    rsp.result.capabilities.documentSymbolProvider.emplace();
    rsp.result.capabilities.documentSymbolProvider->first.emplace(true);

    rsp.result.capabilities.hoverProvider.emplace(true);

    rsp.result.capabilities.foldingRangeProvider.emplace();
    rsp.result.capabilities.foldingRangeProvider->first.emplace(true);

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
    reparseAndPublish(file, notify.params.textDocument.uri);
}

void FreeBasicServer::onDidChange(Notify_TextDocumentDidChange::notify const& notify)
{
    std::shared_ptr<WorkingFile> file = workingFiles_.OnChange(notify.params);
    if (!file)
    {
        return;
    }
    reparseAndPublish(file, notify.params.textDocument.uri);
}

void FreeBasicServer::onDidSave(Notify_TextDocumentDidSave::notify const& notify)
{
    std::shared_ptr<WorkingFile> file = workingFiles_.OnSave(notify.params.textDocument);
    if (!file)
    {
        return;
    }
    reparseAndPublish(file, notify.params.textDocument.uri);
}

void FreeBasicServer::onDidClose(Notify_TextDocumentDidClose::notify const& notify)
{
    if (!workingFiles_.OnClose(notify.params.textDocument))
    {
        return;
    }
    publishDiagnostics(notify.params.textDocument.uri, {});
}

void FreeBasicServer::reparseAndPublish(std::shared_ptr<WorkingFile> const& file, lsDocumentUri const& uri)
{
    std::string_view content = file->GetContentNoLock();
    fblang::ParseResult parse = fblang::parseDocument(content);
    publishDiagnostics(uri, convertDiagnostics(content, parse));
}

td_symbol::response FreeBasicServer::onDocumentSymbol(td_symbol::request const& req)
{
    td_symbol::response rsp;
    rsp.id = req.id;

    std::shared_ptr<WorkingFile> file = workingFiles_.GetFileByFilename(req.params.textDocument.uri.GetAbsolutePath());
    if (!file)
    {
        return rsp;
    }
    std::string_view content = file->GetContentNoLock();
    fblang::ParseResult parse = fblang::parseDocument(content);
    for (auto const& root : parse.roots)
    {
        if (root.kind == fblang::SymbolKind::Scope)
        {
            continue;
        }
        rsp.result.push_back(convertSymbol(content, root));
    }
    return rsp;
}

td_hover::response FreeBasicServer::onHover(td_hover::request const& req)
{
    td_hover::response rsp;
    rsp.id = req.id;

    std::shared_ptr<WorkingFile> file =
        workingFiles_.GetFileByFilename(req.params.textDocument.uri.GetAbsolutePath());
    if (!file)
    {
        return rsp;
    }
    std::string_view content = file->GetContentNoLock();
    std::uint32_t const offset = fblang::byteOffsetForUtf16Position(content, req.params.position);

    fblang::ParseResult parse = fblang::parseDocument(content);
    fblang::Symbol const* sym = deepestSymbolAt(parse.roots, offset);
    if (!sym)
    {
        return rsp;
    }

    std::string markdown;
    if (!sym->signature.empty())
    {
        markdown = "```basic\n" + sym->signature + "\n```";
    }
    else
    {
        markdown = "`" + sym->name + "`";
    }
    if (!sym->doc.empty())
    {
        markdown += "\n\n---\n" + sym->doc;
    }

    rsp.result.contents.second.emplace(MarkupContent{std::string("markdown"), std::move(markdown)});
    rsp.result.range.emplace(fblang::utf16Range(content, sym->selection.beg, sym->selection.end));
    return rsp;
}

td_foldingRange::response FreeBasicServer::onFoldingRange(td_foldingRange::request const& req)
{
    td_foldingRange::response rsp;
    rsp.id = req.id;

    std::shared_ptr<WorkingFile> file =
        workingFiles_.GetFileByFilename(req.params.textDocument.uri.GetAbsolutePath());
    if (!file)
    {
        return rsp;
    }
    std::string_view content = file->GetContentNoLock();
    fblang::ParseResult parse = fblang::parseDocument(content);

    for (auto const& br : parse.blockRanges)
    {
        lsPosition const start = fblang::utf16Position(content, br.beg);
        lsPosition const closer = fblang::utf16Position(content, br.end);
        if (closer.line <= start.line)
        {
            continue;  // single-line construct: nothing to fold
        }

        FoldingRange fr;
        fr.startLine = start.line;
        fr.startCharacter = start.character;
        fr.endLine = closer.line - 1;  // keep the END keyword line visible
        // -1 character clamps to the end of the fold line in UTF-16 units.
        fr.endCharacter = fblang::utf16Position(
            content, fblang::byteOffsetForUtf16Position(content, lsPosition(fr.endLine, -1)))
                              .character;
        rsp.result.push_back(fr);
    }
    return rsp;
}

void FreeBasicServer::publishDiagnostics(lsDocumentUri const& uri, std::vector<lsDiagnostic> diagnostics)
{
    Notify_TextDocumentPublishDiagnostics::notify publish;
    publish.params.uri = uri;
    publish.params.diagnostics = std::move(diagnostics);
    session_.endpoint().send(publish);
}