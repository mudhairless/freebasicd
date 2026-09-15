#include "session.h"

#include "language.h"
#include "lexer.h"
#include "parser.h"
#include "resolve.h"
#include "utf16.h"

#include <fstream>
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

lsCompletionItemKind completionKindFor(fblang::SymbolKind kind)
{
    switch (kind)
    {
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
bool isWordChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || fblang::isSuffixChar(c);
}

// Identifier being typed at `off` (bytes), or "" when the cursor is not on an
// identifier character run.
std::string completionPrefix(std::string_view content, std::uint32_t off)
{
    std::size_t start = off;
    while (start > 0 && isWordChar(content[start - 1]))
    {
        --start;
    }
    return std::string(content.substr(start, off - start));
}

bool hasPrefix(std::string_view word, std::string_view prefix)
{
    return word.size() >= prefix.size() && word.substr(0, prefix.size()) == prefix;
}

char const* const kBlockOpeners[] = {"sub",       "function", "property", "operator", "constructor",
                                     "destructor", "type",     "union",    "enum",     "namespace",
                                     "scope",     "if",       "select",   "with",     "extern",
                                     "asm"};

}  // namespace

FreeBasicServer::FreeBasicServer(lsp::LanguageSession& session) : session_(session)
{
}

void FreeBasicServer::setExitHandler(std::function<void()> exitHandler)
{
    exitHandler_ = std::move(exitHandler);
}

void FreeBasicServer::setIndexCacheDir(std::filesystem::path cacheDir)
{
    indexCacheDir_ = std::move(cacheDir);
}

void FreeBasicServer::ensureWorkspaceIndex(std::filesystem::path const& root)
{
    if (root.empty())
    {
        return;
    }
    if (index_ && fblang::normalizePath(root) == fblang::normalizePath(index_->root()))
    {
        return;
    }
    if (index_)
    {
        index_->close();
    }
    index_ = std::make_unique<fblang::WorkspaceIndex>(root, indexCacheDir_);
    index_->open();
    index_->scan(true);
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
    session_.on([this](td_definition::request const& req) { return onDefinition(req); });
    session_.on([this](td_references::request const& req) { return onReferences(req); });
    session_.on([this](td_highlight::request const& req) { return onHighlight(req); });
    session_.on([this](td_completion::request const& req) { return onCompletion(req); });
    session_.on([this](td_signatureHelp::request const& req) { return onSignatureHelp(req); });
    session_.on([this](wp_symbol::request const& req) { return onWorkspaceSymbol(req); });
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

    rsp.result.capabilities.definitionProvider.emplace();
    rsp.result.capabilities.definitionProvider->first.emplace(true);

    rsp.result.capabilities.referencesProvider.emplace();
    rsp.result.capabilities.referencesProvider->first.emplace(true);

    rsp.result.capabilities.documentHighlightProvider.emplace();
    rsp.result.capabilities.documentHighlightProvider->first.emplace(true);

    rsp.result.capabilities.completionProvider.emplace();
    rsp.result.capabilities.completionProvider->triggerCharacters.emplace();
    rsp.result.capabilities.completionProvider->triggerCharacters->emplace_back(".");

    rsp.result.capabilities.signatureHelpProvider.emplace();
    rsp.result.capabilities.signatureHelpProvider->triggerCharacters.emplace_back("(");
    rsp.result.capabilities.signatureHelpProvider->triggerCharacters.emplace_back(",");

    rsp.result.capabilities.workspaceSymbolProvider.emplace();
    rsp.result.capabilities.workspaceSymbolProvider->first.emplace(true);

    // Workspace root: rootUri wins over workspaceFolders; fall back to the
    // first opened file when neither is present (single-file mode).
    std::string rootPath;
    if (req.params.rootUri)
    {
        rootPath = req.params.rootUri->GetAbsolutePath().path();
    }
    if (rootPath.empty() && req.params.workspaceFolders && !req.params.workspaceFolders->empty())
    {
        rootPath = (*req.params.workspaceFolders)[0].uri.GetAbsolutePath().path();
    }
    if (!rootPath.empty())
    {
        ensureWorkspaceIndex(rootPath);
    }

    return rsp;
}

td_shutdown::response FreeBasicServer::onShutdown(td_shutdown::request const& req)
{
    td_shutdown::response rsp;
    rsp.id = req.id;

    if (index_)
    {
        index_->close();
    }

    lsp::Any result;
    result.SetJsonString("null", lsp::Any::kNullType);
    rsp.result = result;

    return rsp;
}

void FreeBasicServer::onDidOpen(Notify_TextDocumentDidOpen::notify& notify)
{
    if (!index_)
    {
        ensureWorkspaceIndex(std::filesystem::path(notify.params.textDocument.uri.GetAbsolutePath().path()).parent_path());
    }
    std::shared_ptr<WorkingFile> const file = workingFiles_.OnOpen(notify.params.textDocument);
    if (!file)
    {
        return;
    }
    reparseAndPublish(file, notify.params.textDocument.uri);
}

void FreeBasicServer::onDidChange(Notify_TextDocumentDidChange::notify const& notify)
{
    std::shared_ptr<WorkingFile> const file = workingFiles_.OnChange(notify.params);
    if (!file)
    {
        return;
    }
    reparseAndPublish(file, notify.params.textDocument.uri);
}

void FreeBasicServer::onDidSave(Notify_TextDocumentDidSave::notify const& notify)
{
    std::shared_ptr<WorkingFile> const file = workingFiles_.OnSave(notify.params.textDocument);
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
    std::string_view const content = file->GetContentNoLock();
    fblang::AnalyzedDoc doc = fblang::analyze(content);
    publishDiagnostics(uri, convertDiagnostics(content, doc.parse));

    if (!index_)
    {
        return;
    }
    std::string const path = uri.GetAbsolutePath().path();
    std::string const ext = fblang::toLowerChars(std::filesystem::path(path).extension().string());
    if (ext != ".bas" && ext != ".bi")
    {
        return;
    }
    std::uint64_t mtime = 0;
    std::uint64_t size = 0;
    fblang::statFile(path, &mtime, &size);
    // Open-buffer entries are never persisted: an unsaved buffer must not be
    // written to the disk cache as on-disk truth, nor satisfy scan's
    // mtime/size cache-hit. include targets still resolve against disk.
    index_->upsert(fblang::indexedFileFromAnalysis(
        fblang::normalizePath(path), mtime, size, std::move(doc), index_->root(),
        /*persisted=*/false));
    index_->flushSoon();
}

td_symbol::response FreeBasicServer::onDocumentSymbol(td_symbol::request const& req)
{
    td_symbol::response rsp;
    rsp.id = req.id;

    std::shared_ptr<WorkingFile> const file = workingFiles_.GetFileByFilename(req.params.textDocument.uri.GetAbsolutePath());
    if (!file)
    {
        return rsp;
    }
    std::string_view const content = file->GetContentNoLock();
    fblang::ParseResult const parse = fblang::parseDocument(content);
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

    std::shared_ptr<WorkingFile> const file =
        workingFiles_.GetFileByFilename(req.params.textDocument.uri.GetAbsolutePath());
    if (!file)
    {
        return rsp;
    }
    std::string_view const content = file->GetContentNoLock();
    std::uint32_t const offset = fblang::byteOffsetForUtf16Position(content, req.params.position);

    fblang::ParseResult const parse = fblang::parseDocument(content);
    fblang::Symbol const* sym = deepestSymbolAt(parse.roots, offset);
    if (!sym)
    {
        // No user symbol here: hover a reserved keyword with its wiki link.
        if (offset < content.size() && isWordChar(content[offset]))
        {
            std::uint32_t beg = offset;
            while (beg > 0 && isWordChar(content[beg - 1]))
            {
                --beg;
            }
            std::uint32_t end = offset;
            while (end < content.size() && isWordChar(content[end]))
            {
                ++end;
            }
            std::string const word =
                fblang::toLowerChars(std::string(content.substr(beg, end - beg)));
            std::string const url = fblang::keywordDocsUrl(word);
            if (!url.empty())
            {
                rsp.result.contents.second.emplace(
                    MarkupContent{std::string("markdown"),
                                  "`" + word + "` — FreeBASIC keyword\n\n"
                                  "[FreeBASIC docs](" + url + ")"});
                rsp.result.range.emplace(fblang::utf16Range(content, beg, end));
                return rsp;
            }
        }
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

    std::shared_ptr<WorkingFile> const file =
        workingFiles_.GetFileByFilename(req.params.textDocument.uri.GetAbsolutePath());
    if (!file)
    {
        return rsp;
    }
    std::string_view const content = file->GetContentNoLock();
    fblang::ParseResult const parse = fblang::parseDocument(content);

    for (auto const& br : parse.blockRanges)
    {
        lsPosition const start = fblang::utf16Position(content, br.beg);
        lsPosition const closer = fblang::utf16Position(content, br.end);
        if (closer.line <= start.line)
        {
            continue;  // single-line construct: nothing to fold
        }

        FoldingRange fr;
        fr.startLine = static_cast<int>(start.line);
        fr.startCharacter = static_cast<int>(start.character);
        fr.endLine = static_cast<int>(closer.line) - 1;  // keep the END keyword line visible
        // -1 character clamps to the end of the fold line in UTF-16 units.
        fr.endCharacter = static_cast<int>(fblang::utf16Position(
            content, fblang::byteOffsetForUtf16Position(content, lsPosition(fr.endLine, -1)))
                              .character);
        rsp.result.push_back(fr);
    }
    return rsp;
}

td_definition::response FreeBasicServer::onDefinition(td_definition::request const& req)
{
    td_definition::response rsp;
    rsp.id = req.id;

    std::shared_ptr<WorkingFile> const file =
        workingFiles_.GetFileByFilename(req.params.textDocument.uri.GetAbsolutePath());
    if (!file)
    {
        return rsp;
    }
    std::string_view const content = file->GetContentNoLock();
    std::uint32_t const offset = fblang::byteOffsetForUtf16Position(content, req.params.position);

    fblang::ParseResult const parse = fblang::parseDocument(content);
    fblang::Symbol const* decl = fblang::resolveAt(parse, content, offset);
    if (!decl)
    {
        return rsp;
    }
    rsp.result.first.emplace();
    rsp.result.first->push_back(
        lsLocation(req.params.textDocument.uri, fblang::utf16Range(content, decl->selection.beg,
                                                                   decl->selection.end)));
    return rsp;
}

td_references::response FreeBasicServer::onReferences(td_references::request const& req)
{
    td_references::response rsp;
    rsp.id = req.id;

    std::shared_ptr<WorkingFile> const file =
        workingFiles_.GetFileByFilename(req.params.textDocument.uri.GetAbsolutePath());
    if (!file)
    {
        return rsp;
    }
    std::string_view const content = file->GetContentNoLock();
    std::uint32_t const offset = fblang::byteOffsetForUtf16Position(content, req.params.position);

    fblang::ParseResult const parse = fblang::parseDocument(content);
    fblang::Symbol const* decl = fblang::resolveAt(parse, content, offset);
    if (!decl)
    {
        return rsp;
    }

    bool const includeDecl = !req.params.context.includeDeclaration || *req.params.context.includeDeclaration;
    if (includeDecl)
    {
        rsp.result.push_back(lsLocation(req.params.textDocument.uri,
                                        fblang::utf16Range(content, decl->selection.beg,
                                                           decl->selection.end)));
    }
    for (auto const& ref : fblang::occurrencesOf(parse, content, *decl))
    {
        rsp.result.push_back(
            lsLocation(req.params.textDocument.uri, fblang::utf16Range(content, ref.beg, ref.end)));
    }
    return rsp;
}

td_highlight::response FreeBasicServer::onHighlight(td_highlight::request const& req)
{
    td_highlight::response rsp;
    rsp.id = req.id;

    std::shared_ptr<WorkingFile> const file =
        workingFiles_.GetFileByFilename(req.params.textDocument.uri.GetAbsolutePath());
    if (!file)
    {
        return rsp;
    }
    std::string_view content = file->GetContentNoLock();
    std::uint32_t const offset = fblang::byteOffsetForUtf16Position(content, req.params.position);

    fblang::ParseResult const parse = fblang::parseDocument(content);
    fblang::Symbol const* decl = fblang::resolveAt(parse, content, offset);
    if (!decl)
    {
        return rsp;
    }

    auto add = [&](fblang::SourceRange range)
    {
        lsDocumentHighlight hl;
        hl.range = fblang::utf16Range(content, range.beg, range.end);
        hl.kind.emplace(lsDocumentHighlightKind::Text);
        rsp.result.push_back(hl);
    };
    add(decl->selection);
    for (auto const& ref : fblang::occurrencesOf(parse, content, *decl))
    {
        add(ref);
    }
    return rsp;
}

td_completion::response FreeBasicServer::onCompletion(td_completion::request const& req)
{
    td_completion::response rsp;
    rsp.id = req.id;

    std::shared_ptr<WorkingFile> const file =
        workingFiles_.GetFileByFilename(req.params.textDocument.uri.GetAbsolutePath());
    if (!file)
    {
        return rsp;
    }
    std::string_view const content = file->GetContentNoLock();
    std::uint32_t const offset = fblang::byteOffsetForUtf16Position(content, req.params.position);
    std::string const prefix = fblang::toLowerChars(completionPrefix(content, offset));

    fblang::ParseResult const parse = fblang::parseDocument(content);

    for (std::string_view const w : fblang::reservedWords())
    {
        if (!hasPrefix(w, prefix))
        {
            continue;
        }
        lsCompletionItem item;
        item.label = std::string(w);
        item.kind.emplace(lsCompletionItemKind::Keyword);
        std::string const url = fblang::keywordDocsUrl(w);
        if (!url.empty())
        {
            item.documentation.emplace();
            item.documentation->second.emplace(MarkupContent{std::string("markdown"),
                std::string("**FreeBASIC keyword**\n\n[") + std::string(w) + "](" + url + ")"});
        }
        rsp.result.items.push_back(std::move(item));
    }

    for (char const* opener : kBlockOpeners)
    {
        fblang::BlockCloser closer;
        if (!fblang::blockForOpener(opener, &closer) || !closer.needsEnd)
        {
            continue;
        }
        std::string const label = "end " + std::string(closer.closeWord);
        if (!hasPrefix(label, prefix))
        {
            continue;
        }
        lsCompletionItem item;
        item.label = label;
        item.kind.emplace(lsCompletionItemKind::Snippet);
        item.insertText.emplace(fblang::closerDisplay(closer));
        rsp.result.items.push_back(std::move(item));
    }

    std::vector<std::string> seen;
    for (fblang::Symbol const* sym : fblang::visibleSymbols(parse, offset))
    {
        if (!hasPrefix(sym->key, prefix))
        {
            continue;
        }
        bool dup = false;
        for (auto const& k : seen)
        {
            if (k == sym->key)
            {
                dup = true;
                break;
            }
        }
        if (dup)
        {
            continue;
        }
        seen.push_back(sym->key);
        lsCompletionItem item;
        item.label = sym->name;
        item.kind.emplace(completionKindFor(sym->kind));
        if (!sym->signature.empty())
        {
            item.detail.emplace(sym->signature);
        }
        rsp.result.items.push_back(std::move(item));
    }
    return rsp;
}

td_signatureHelp::response FreeBasicServer::onSignatureHelp(td_signatureHelp::request const& req)
{
    td_signatureHelp::response rsp;
    rsp.id = req.id;

    std::shared_ptr<WorkingFile> const file =
        workingFiles_.GetFileByFilename(req.params.textDocument.uri.GetAbsolutePath());
    if (!file)
    {
        return rsp;
    }
    std::string_view const content = file->GetContentNoLock();
    std::uint32_t const offset = fblang::byteOffsetForUtf16Position(content, req.params.position);

    fblang::Lexer lx(content);
    std::vector<fblang::Token> toks;
    for (;;)
    {
        fblang::Token const t = lx.next();
        toks.push_back(t);
        if (t.kind == fblang::TokenKind::Eof)
        {
            break;
        }
    }
    if (toks.empty())
    {
        return rsp;
    }

    // Stack of unclosed '(' with the identifier callee right before each.
    std::vector<int> openStack;
    std::vector<int> calleeStack;
    for (std::size_t i = 0; i < toks.size(); ++i)
    {
        fblang::Token const& t = toks[i];
        if (t.beg > offset)
        {
            break;
        }
        if (t.kind != fblang::TokenKind::Symbol)
        {
            continue;
        }
        std::string_view const s = t.text();
        if (s == ")")
        {
            if (!openStack.empty())
            {
                openStack.pop_back();
                calleeStack.pop_back();
            }
            continue;
        }
        if (s != "(")
        {
            continue;
        }
        int callee = -1;
        for (std::size_t j = i; j > 0; --j)
        {
            fblang::Token const& prev = toks[j - 1];
            if (prev.kind == fblang::TokenKind::Newline)
            {
                break;
            }
            if (prev.kind == fblang::TokenKind::Identifier)
            {
                callee = static_cast<int>(j - 1);
                break;
            }
            if (prev.kind == fblang::TokenKind::Keyword)
            {
                break;
            }
        }
        openStack.push_back(static_cast<int>(i));
        calleeStack.push_back(callee);
    }
    if (openStack.empty() || calleeStack.back() < 0)
    {
        return rsp;
    }
    int const openIdx = openStack.back();
    int const nameIdx = calleeStack.back();

    fblang::ParseResult const parse = fblang::parseDocument(content);
    fblang::Token const& calleeTok = toks[static_cast<std::size_t>(nameIdx)];
    fblang::Symbol const* decl = fblang::resolveAt(parse, content, calleeTok.beg);
    if (!decl)
    {
        return rsp;
    }
    switch (decl->kind)
    {
        case fblang::SymbolKind::Sub:
        case fblang::SymbolKind::Function:
        case fblang::SymbolKind::Property:
        case fblang::SymbolKind::Constructor:
        case fblang::SymbolKind::Destructor:
        case fblang::SymbolKind::Operator:
            break;
        default:
            return rsp;
    }

    lsSignatureInformation info;
    info.label = decl->signature.empty() ? decl->name : decl->signature;
    for (auto const& p : decl->children)
    {
        if (p.kind == fblang::SymbolKind::Parameter)
        {
            lsParameterInformation pi;
            pi.label = p.name;
            info.parameters.push_back(std::move(pi));
        }
    }

    int activeParam = 0;
    int depth = 0;
    for (std::size_t i = static_cast<std::size_t>(openIdx) + 1; i < toks.size(); ++i)
    {
        fblang::Token const& t = toks[i];
        if (t.beg >= offset)
        {
            break;
        }
        if (t.kind != fblang::TokenKind::Symbol)
        {
            continue;
        }
        std::string_view const s = t.text();
        if (s == "(")
        {
            ++depth;
        }
        else if (s == ")")
        {
            if (depth > 0)
            {
                --depth;
            }
        }
        else if (s == "," && depth == 0)
        {
            ++activeParam;
        }
    }

    rsp.result.signatures.push_back(std::move(info));
    rsp.result.activeSignature.emplace(0);
    rsp.result.activeParameter.emplace(activeParam);
    return rsp;
}

wp_symbol::response FreeBasicServer::onWorkspaceSymbol(wp_symbol::request const& req)
{
    wp_symbol::response rsp;
    if (!index_)
    {
        return rsp;
    }

    std::string const query = fblang::toLowerChars(req.params.query);

    struct Match {
        fblang::Symbol const* sym;
        std::string container;
    };
    struct FileMatches {
        fblang::IndexedFile const* file;
        std::vector<Match> matches;
    };

    std::vector<FileMatches> hits;

    std::function<void(fblang::Symbol const&, std::string const&, std::vector<Match>&)> collect =
        [&query, &collect](fblang::Symbol const& s, std::string const& container,
                           std::vector<Match>& into) {
            std::string const key = fblang::toLowerChars(s.key);
            std::string const name = fblang::toLowerChars(s.name);
            if (key.find(query) != std::string::npos || name.find(query) != std::string::npos)
            {
                into.push_back(Match{&s, container});
            }
            std::string const next = container.empty() ? s.name : container + "." + s.name;
            for (auto const& c : s.children)
            {
                collect(c, next, into);
            }
        };

    for (auto const& file : index_->snapshot())
    {
        std::vector<Match> matches;
        for (auto const& root : file->roots)
        {
            collect(root, {}, matches);
        }
        if (matches.empty())
        {
            continue;
        }
        hits.push_back(FileMatches{file.get(), std::move(matches)});
    }

    for (auto& hit : hits)
    {
        std::string content;
        {
            std::ifstream in(hit.file->path, std::ios::binary);
            if (!in)
            {
                continue;
            }
            content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        for (auto& m : hit.matches)
        {
            lsSymbolInformation info;
            info.name = m.sym->name;
            info.kind = toLspSymbolKind(m.sym->kind);
            info.location = lsLocation(lsDocumentUri(AbsolutePath(hit.file->path)),
                                       fblang::utf16Range(content, m.sym->selection.beg,
                                                          m.sym->selection.end));
            if (!m.container.empty())
            {
                info.containerName.emplace(m.container);
            }
            rsp.result.push_back(std::move(info));
        }
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