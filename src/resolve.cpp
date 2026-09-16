#include "resolve.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "language.h"
#include "lexer.h"
#include "parser.h"

namespace fblang {

namespace {

// Deepest node of `sym` (inclusive, so a cursor on the closer token still
// lands inside the block) that contains `off`, or nullptr.
Symbol const* deepestNesting(Symbol const& sym, std::uint32_t off)
{
    if (off < sym.range.beg || off > sym.range.end)
    {
        return nullptr;
    }
    for (auto const& c : sym.children)
    {
        if (Symbol const* hit = deepestNesting(c, off))
        {
            return hit;
        }
    }
    return &sym;
}

bool isScopeKind(SymbolKind kind)
{
    switch (kind)
    {
        case SymbolKind::Sub:
        case SymbolKind::Function:
        case SymbolKind::Property:
        case SymbolKind::Constructor:
        case SymbolKind::Destructor:
        case SymbolKind::Operator:
        case SymbolKind::Type:
        case SymbolKind::Union:
        case SymbolKind::Enum:
        case SymbolKind::Namespace:
        case SymbolKind::Scope:
            return true;
        case SymbolKind::Const:
        case SymbolKind::Dim:
        case SymbolKind::Label:
        case SymbolKind::Parameter:
        case SymbolKind::Variable:
            return false;
    }
    return false;
}

Symbol const* findParent(Symbol const& cur, Symbol const* node)
{
    for (auto const& c : cur.children)
    {
        if (&c == node)
        {
            return &cur;
        }
        if (Symbol const* p = findParent(c, node))
        {
            return p;
        }
    }
    return nullptr;
}

Symbol const* parentOf(ParseResult const& parse, Symbol const* node)
{
    for (auto const& root : parse.roots)
    {
        if (Symbol const* p = findParent(root, node))
        {
            return p;
        }
    }
    return nullptr;
}

// Identifier token at `off`, or nullptr. A cursor between two characters is
// considered inside a token that spans it.
Token const* tokenAt(std::vector<Token> const& tokens, std::uint32_t off)
{
    for (auto const& t : tokens)
    {
        if (t.kind == TokenKind::Identifier && t.beg <= off && off <= t.end)
        {
            return &t;
        }
    }
    return nullptr;
}

std::vector<Token> lexAll(std::string_view src)
{
    Lexer lx(src);
    std::vector<Token> out;
    for (;;)
    {
        Token const t = lx.next();
        out.push_back(t);
        if (t.kind == TokenKind::Eof)
        {
            return out;
        }
    }
}

SourceRange rangeOf(Token const& t)
{
    return {t.beg, t.end};
}

// The declaration a usage at `off` resolves to, over a pre-lexed stream. This
// is the single resolution walk shared by analyze's occurrence sweep and the
// on-demand ParseResult legacy API (which used to re-lex per call).
//
// Honors the §12.2 storage gate: a usage inside any block may only match a
// module-scope Dim-kind declaration carrying the `Shared` modifier; at module
// level every root is visible. Procedure/type/enum/const roots are never
// gated.
Symbol const* declAt(ParseResult const& parse, std::vector<Token> const& tokens,
                     std::uint32_t off)
{
    Token const* tok = tokenAt(tokens, off);
    if (!tok)
    {
        return nullptr;
    }
    std::string const key = toLowerChars(tok->text());
    Symbol const* const siteScope = innermostScope(parse, off);
    for (Symbol const* cur = siteScope;; cur = cur ? parentOf(parse, cur) : nullptr)
    {
        std::vector<Symbol> const& cands = cur ? cur->children : parse.roots;
        for (auto const& c : cands)
        {
            if (c.key.empty() || c.key != key)
            {
                continue;
            }
            bool const gated = cur == nullptr && siteScope != nullptr &&
                               c.kind == SymbolKind::Dim && !c.shared;
            if (!gated)
            {
                return &c;
            }
        }
        if (!cur)
        {
            return nullptr;
        }
    }
}

// Fill `occurrences` on every Symbol of `parse` (file roots tagged
// moduleScope) with every usage that resolves to it, in source order.
// `const_cast` is safe here: the walk is read-only and the decl pointer is,
// by construction, into the tree we are filling.
void attachOccurrences(ParseResult& parse, std::vector<Token> const& tokens)
{
    for (Symbol& root : parse.roots)
    {
        root.moduleScope = true;
    }
    for (Token const& t : tokens)
    {
        if (t.kind != TokenKind::Identifier)
        {
            continue;
        }
        Symbol const* const decl = declAt(parse, tokens, t.beg);
        if (!decl || (t.beg == decl->selection.beg && t.end == decl->selection.end))
        {
            continue;
        }
        bool const siteModuleScope = innermostScope(parse, t.beg) == nullptr;
        const_cast<Symbol*>(decl)->occurrences.push_back({rangeOf(t), siteModuleScope});
    }
}

bool isDirectiveWordChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// A `#pragma once` Preprocessor line anywhere in the source. Independent of
// the include sweep (also over Preprocessor tokens; a single pass over the
// few directive lines keeps both walks cheap and independent).
bool detectPragmaOnce(std::string_view source, std::vector<Token> const& tokens)
{
    for (Token const& t : tokens)
    {
        if (t.kind != TokenKind::Preprocessor)
        {
            continue;
        }
        std::string_view const line = source.substr(t.beg, t.end - t.beg);  // starts at '#'
        std::size_t i = 1;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        {
            ++i;
        }
        std::size_t const wbeg = i;
        while (i < line.size() && isDirectiveWordChar(line[i]))
        {
            ++i;
        }
        if (toLowerChars(line.substr(wbeg, i - wbeg)) != "pragma")
        {
            continue;
        }
        std::size_t r = i;
        while (r < line.size() && (line[r] == ' ' || line[r] == '\t'))
        {
            ++r;
        }
        std::size_t const vbeg = r;
        while (r < line.size() && isDirectiveWordChar(line[r]))
        {
            ++r;
        }
        if (r > vbeg && toLowerChars(line.substr(vbeg, r - vbeg)) == "once")
        {
            return true;
        }
    }
    return false;
}

// Extract `#include [once] ["]literal["]` directives from the whole-line
// Preprocessor tokens. Ranges are byte offsets into `source`.
void collectIncludes(std::string_view source, std::vector<Token> const& tokens,
                     std::vector<IncludeDirective>* out)
{
    for (Token const& t : tokens)
    {
        if (t.kind != TokenKind::Preprocessor)
        {
            continue;
        }
        std::string_view const line = source.substr(t.beg, t.end - t.beg);  // starts at '#'
        std::size_t i = 1;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        {
            ++i;
        }
        std::size_t const wbeg = i;
        while (i < line.size() && isDirectiveWordChar(line[i]))
        {
            ++i;
        }
        if (toLowerChars(line.substr(wbeg, i - wbeg)) != "include")
        {
            continue;
        }
        IncludeDirective inc;
        inc.line.beg = t.beg;
        inc.line.end = t.end;
        std::size_t r = i;
        while (r < line.size() && (line[r] == ' ' || line[r] == '\t'))
        {
            ++r;
        }
        std::size_t const onceLen = 4;
        if (r + onceLen <= line.size() &&
            toLowerChars(line.substr(r, onceLen)) == "once" &&
            (r + onceLen == line.size() || line[r + onceLen] == ' ' || line[r + onceLen] == '\t'))
        {
            inc.once = true;
            r += onceLen;
            while (r < line.size() && (line[r] == ' ' || line[r] == '\t'))
            {
                ++r;
            }
        }
        if (r >= line.size())
        {
            out->push_back(std::move(inc));
            continue;
        }
        std::uint32_t const base = t.beg;
        if (line[r] == '"')
        {
            std::size_t q = r + 1;
            while (q < line.size() && line[q] != '"')
            {
                ++q;
            }
            inc.literal = std::string(line.substr(r + 1, q - r - 1));
            inc.target.beg = base + static_cast<std::uint32_t>(r + 1);
            inc.target.end = base + static_cast<std::uint32_t>(q);
        }
        else
        {
            std::size_t k = r;
            while (k < line.size() && line[k] != ' ' && line[k] != '\t')
            {
                ++k;
            }
            inc.literal = std::string(line.substr(r, k - r));
            inc.target.beg = base + static_cast<std::uint32_t>(r);
            inc.target.end = base + static_cast<std::uint32_t>(k);
        }
        out->push_back(std::move(inc));
    }
}

}  // namespace

Symbol const* innermostScope(ParseResult const& parse, std::uint32_t off)
{
    Symbol const* best = nullptr;
    for (auto const& root : parse.roots)
    {
        if (Symbol const* d = deepestNesting(root, off))
        {
            best = d;
        }
    }
    while (best && !isScopeKind(best->kind))
    {
        best = parentOf(parse, best);
    }
    return best;
}

AnalyzedDoc analyze(std::string_view source)
{
    AnalyzedDoc doc;
    doc.parse = parseDocument(source);
    Lexer lx(source);
    for (;;)
    {
        Token const t = lx.next();
        doc.tokens.push_back(t);
        if (t.kind == TokenKind::Eof)
        {
            break;
        }
    }
    attachOccurrences(doc.parse, doc.tokens);
    collectIncludes(source, doc.tokens, &doc.includes);
    doc.pragmaOnce = detectPragmaOnce(source, doc.tokens);
    return doc;
}

Symbol const* resolveAt(AnalyzedDoc const& doc, std::uint32_t off)
{
    return declAt(doc.parse, doc.tokens, off);
}

std::vector<Occurrence> occurrencesOf(AnalyzedDoc const& doc, Symbol const& decl)
{
    (void)doc;  // precondition: `decl` points into doc.parse's tree
    return decl.occurrences;
}

CrossDecl resolveAcross(AnalyzedDoc const& doc, std::string const& normalizedPath,
                        std::uint32_t off, WorkspaceIndex const& index)
{
    Token const* const tok = tokenAt(doc.tokens, off);
    if (!tok)
    {
        return {};
    }

    // Tier 1: in-file scopes, shadowing wins.
    if (Symbol const* const local = declAt(doc.parse, doc.tokens, off))
    {
        return CrossDecl{nullptr, local};
    }

    std::string const key = toLowerChars(tok->text());
    bool const insideBlock = innermostScope(doc.parse, off) != nullptr;
    auto gated = [insideBlock](Symbol const& root)
    {
        return insideBlock && root.kind == SymbolKind::Dim && !root.shared;
    };

    // Tier 2: module scope of each closure file, textual include pre-order,
    // first key match. The closure is treated as one textual module (FreeBASIC
    // .md §9): the same storage gate applies to its roots as to the requesting
    // file's own module level.
    for (std::string const& closurePath : index.transitiveIncludes(normalizedPath))
    {
        std::shared_ptr<IndexedFile const> const file = index.fileAt(closurePath);
        if (!file)
        {
            continue;
        }
        for (Symbol const& root : file->roots)
        {
            if (root.key == key && !gated(root))
            {
                return CrossDecl{file, &root};
            }
        }
    }

    // Tier 3: lenient `byKey` workspace fallback for still-unincluded headers
    // (recorded divergence, FreeBASIC.md §12). byKey indexes file roots only,
    // so a procedure-local name can never resolve here.
    for (KeyedDecl const& kd : index.byKey(key))
    {
        if (!gated(*kd.decl))
        {
            return CrossDecl{kd.file, kd.decl};
        }
    }
    return {};
}

std::vector<Symbol const*> visibleSymbols(AnalyzedDoc const& doc, std::uint32_t off)
{
    return visibleSymbols(doc.parse, off);
}

Symbol const* resolveAt(ParseResult const& parse, std::string_view src, std::uint32_t off)
{
    return declAt(parse, lexAll(src), off);
}

std::vector<SourceRange> occurrencesOf(ParseResult const& parse, std::string_view src,
                                       Symbol const& decl)
{
    std::vector<SourceRange> out;
    std::vector<Token> const tokens = lexAll(src);
    for (auto const& t : tokens)
    {
        if (t.kind != TokenKind::Identifier ||
            (t.beg == decl.selection.beg && t.end == decl.selection.end))
        {
            continue;
        }
        if (declAt(parse, tokens, t.beg) == &decl)
        {
            out.push_back(rangeOf(t));
        }
    }
    std::sort(out.begin(), out.end(),
              [](SourceRange a, SourceRange b) { return a.beg < b.beg; });
    return out;
}

std::vector<Symbol const*> visibleSymbols(ParseResult const& parse, std::uint32_t off)
{
    std::vector<Symbol const*> out;
    Symbol const* const siteScope = innermostScope(parse, off);
    for (Symbol const* cur = siteScope;; cur = cur ? parentOf(parse, cur) : nullptr)
    {
        std::vector<Symbol> const& cands = cur ? cur->children : parse.roots;
        for (auto const& c : cands)
        {
            if (c.key.empty())
            {
                continue;
            }
            // §12.2 gate: from inside a block, module-level Dim-kind names
            // require the Shared modifier; at module level everything shows.
            if (cur == nullptr && siteScope != nullptr && c.kind == SymbolKind::Dim &&
                !c.shared)
            {
                continue;
            }
            out.push_back(&c);
        }
        if (!cur)
        {
            break;
        }
    }
    return out;
}

}  // namespace fblang