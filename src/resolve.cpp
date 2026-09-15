#include "resolve.h"

#include <algorithm>

#include "lexer.h"

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

Symbol const* resolveAt(ParseResult const& parse, std::string_view src, std::uint32_t off)
{
    std::vector<Token> const tokens = lexAll(src);
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
        if (resolveAt(parse, src, t.beg) == &decl)
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