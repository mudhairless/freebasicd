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
Symbol const* declAt(ParseResult const& parse, std::vector<Token> const& tokens,
                     std::uint32_t off)
{
    Token const* tok = tokenAt(tokens, off);
    if (!tok)
    {
        return nullptr;
    }
    std::string const key = toLowerChars(tok->text());

    for (Symbol const* cur = innermostScope(parse, off);; cur = cur ? parentOf(parse, cur) : nullptr)
    {
        std::vector<Symbol> const& cands = cur ? cur->children : parse.roots;
        for (auto const& c : cands)
        {
            if (!c.key.empty() && c.key == key)
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
    for (Symbol const* cur = innermostScope(parse, off);; cur = cur ? parentOf(parse, cur) : nullptr)
    {
        std::vector<Symbol> const& cands = cur ? cur->children : parse.roots;
        for (auto const& c : cands)
        {
            if (!c.key.empty())
            {
                out.push_back(&c);
            }
        }
        if (!cur)
        {
            break;
        }
    }
    return out;
}

}  // namespace fblang