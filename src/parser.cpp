#include "parser.h"

#include "language.h"
#include "lexer.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace fblang {
namespace {

using TokenKind = fblang::TokenKind;

std::string uppercase(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        if (c >= 'a' && c <= 'z')
        {
            c = static_cast<char>(c - 'a' + 'A');
        }
        out.push_back(c);
    }
    return out;
}

struct Block {
    BlockKind kind;
    std::string_view close;  // closer keyword for display, lowercase ("#endif" may lead with #)
    bool needsEnd = false;   // closer is "END <close>"
    Symbol* sym = nullptr;   // decl container (SUB, TYPE, ...), or null for control blocks
    uint32_t begOpen = 0;    // opener keyword range
    uint32_t endOpen = 0;
};

struct Container {
    Symbol* sym;                                // null for module scope
    std::unordered_set<std::string> keys;       // dedupe for names at this level
    explicit Container(Symbol* s)
        : sym(s)
    {
    }
};

class Parser {
public:
    explicit Parser(std::string_view src)
        : src_(src)
        , lex_(src)
    {
    }

    ParseResult run()
    {
        containers_.push_back(Container(nullptr));
        advance();

        for (;;)
        {
            if (cur_.kind == TokenKind::Eof)
            {
                break;
            }
            switch (cur_.kind)
            {
            case TokenKind::Newline:
                advance();
                continue;
            case TokenKind::Comment:
                checkMetaLang();
                resetDoc();
                advance();
                continue;
            case TokenKind::DocComment:
                checkMetaLang();
                collectDoc();
                if (!cur_.text().empty() && cur_.text()[0] == '/')
                {
                    addDiagnostic(cur_.beg, cur_.end, Severity::Information, "doc-slash",
                                  "/// is not a FreeBASIC comment; use '' for doc comments");
                }
                advance();
                continue;
            case TokenKind::Preprocessor:
                handlePreprocessor();
                advance();
                continue;
            case TokenKind::Meta:
                addDiagnostic(cur_.beg, cur_.end, Severity::Information, "meta-directive",
                              "bare '$' is not a valid metacommand; FreeBASIC "
                              "metacommands are written as comments ('$LANG: \"qb\"')");
                advance();
                continue;
            case TokenKind::Symbol:
                if (cur_.text() == ":")
                {
                    advance();
                    continue;
                }
                handleStatement();
                continue;
            default:
                handleStatement();
                continue;
            }
        }

        // Unterminated blocks, innermost first.
        for (auto it = blocks_.rbegin(); it != blocks_.rend(); ++it)
        {
            addDiagnostic(it->begOpen, it->endOpen, Severity::Error, "unterminated-block",
                          "Expected '" + displayFor(*it) + "'");
        }
        return std::move(out_);
    }

private:
    std::string_view src_;
    Lexer lex_;
    Token cur_;
    ParseResult out_;
    std::vector<Block> blocks_;
    std::vector<Container> containers_;
    std::string docPending_;
    bool langWarned_ = false;

    void advance()
    {
        if (cur_.kind == TokenKind::String && !cur_.terminated)
        {
            addDiagnostic(cur_.beg, cur_.end, Severity::Warning, "unterminated-string",
                          "unterminated string literal");
        }
        cur_ = lex_.next();
    }

    // `$`-metacommand dialect detection inside a comment body. Metacommands are
    // written as comments in FreeBASIC (`'$LANG: "qb"`, `rem $LANG: "qb"`).
    void checkMetaLang()
    {
        LangMode m;
        if (langFromMetaDirective(cur_.text(), &m))
        {
            applyLangDirective(m, cur_.beg, cur_.end);
        }
    }

    void applyLangDirective(LangMode m, uint32_t beg, uint32_t end)
    {
        out_.lang = langName(m);
        if (m != LangMode::Fb && !langWarned_)
        {
            langWarned_ = true;
            addDiagnostic(beg, end, Severity::Information, "lang-mode",
                          "dialect '" + std::string(langName(m)) +
                              "' is not supported yet; parsing in 'fb' mode");
        }
    }

    static bool isDeclOpenerWord(const std::string& w)
    {
        return w == "sub" || w == "function" || w == "property" || w == "operator" ||
               w == "constructor" || w == "destructor" || w == "union" || w == "enum" ||
               w == "namespace";
    }

    static bool isControlOpenerWord(const std::string& w)
    {
        return w == "scope" || w == "select" || w == "with" || w == "extern" || w == "asm" ||
               w == "for" || w == "while" || w == "do";
    }

    static SymbolKind declKindFor(const std::string& w)
    {
        if (w == "sub") return SymbolKind::Sub;
        if (w == "function") return SymbolKind::Function;
        if (w == "property") return SymbolKind::Property;
        if (w == "operator") return SymbolKind::Operator;
        if (w == "constructor") return SymbolKind::Constructor;
        if (w == "destructor") return SymbolKind::Destructor;
        if (w == "union") return SymbolKind::Union;
        if (w == "enum") return SymbolKind::Enum;
        return SymbolKind::Namespace;
    }

    std::string displayFor(const Block& b)
    {
        BlockCloser c;
        c.kind = b.kind;
        c.closeWord = b.close;
        c.needsEnd = b.needsEnd;
        return closerDisplay(c);
    }

    std::string headerText(const Token& openTok)
    {
        uint32_t endOff = currentLineEnd();
        if (endOff < openTok.beg)
        {
            endOff = openTok.beg;
        }
        std::string s = std::string(src_.substr(openTok.beg, endOff - openTok.beg));
        size_t e = s.size();
        while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t'))
        {
            --e;
        }
        s.resize(e);
        return s;
    }

    uint32_t currentLineEnd()
    {
        for (int i = 0; i < 512; ++i)
        {
            Token t = lex_.peek(i);
            if (t.kind == TokenKind::Newline)
            {
                return t.beg;
            }
            if (t.kind == TokenKind::Eof)
            {
                return t.beg;
            }
        }
        return static_cast<uint32_t>(src_.size());
    }

    void addDiagnostic(uint32_t beg, uint32_t end, Severity sev, const char* code,
                       std::string msg)
    {
        Diagnostic d;
        d.range.beg = beg;
        d.range.end = end;
        d.severity = sev;
        d.code = code;
        d.message = std::move(msg);
        out_.diagnostics.push_back(std::move(d));
    }

    void collectDoc()
    {
        std::string_view t = cur_.text();
        size_t skip = 0;
        if (t.size() >= 2 && t[0] == '\'' && t[1] == '\'')
        {
            skip = 2;
        }
        else if (t.size() >= 3 && t[0] == '/' && t[1] == '/' && t[2] == '/')
        {
            skip = 3;
        }
        std::string_view rest = t.substr(skip);
        size_t e = rest.size();
        while (e > 0 && (rest[e - 1] == ' ' || rest[e - 1] == '\t'))
        {
            --e;
        }
        if (!docPending_.empty())
        {
            docPending_.push_back('\n');
        }
        docPending_.append(rest.substr(0, e));
    }

    void resetDoc()
    {
        docPending_.clear();
    }

    std::string takeDoc()
    {
        std::string d = docPending_;
        docPending_.clear();
        return d;
    }

    Symbol* addSymbol(Symbol&& s)
    {
        bool dedupe = s.kind == SymbolKind::Dim || s.kind == SymbolKind::Const ||
                      s.kind == SymbolKind::Variable || s.kind == SymbolKind::Label ||
                      s.kind == SymbolKind::Type || s.kind == SymbolKind::Union ||
                      s.kind == SymbolKind::Enum || s.kind == SymbolKind::Namespace;
        if (dedupe && !containers_.empty())
        {
            auto& set = containers_.back().keys;
            if (set.count(s.key) != 0)
            {
                addDiagnostic(s.selection.beg, s.selection.end, Severity::Warning,
                              "duplicate-definition",
                              "duplicate definition: '" + s.name + "'");
            }
            set.insert(s.key);
        }
        if (containers_.empty())
        {
            out_.roots.push_back(std::move(s));
            return &out_.roots.back();
        }
        Container& c = containers_.back();
        if (c.sym == nullptr)
        {
            out_.roots.push_back(std::move(s));
            return &out_.roots.back();
        }
        c.sym->children.push_back(std::move(s));
        return &c.sym->children.back();
    }

    void closeBlock(uint32_t end)
    {
        Block b = blocks_.back();
        blocks_.pop_back();
        out_.blockRanges.push_back({b.begOpen, end});
        if (b.sym)
        {
            b.sym->range.end = end;
            if (!containers_.empty() && containers_.back().sym == b.sym)
            {
                containers_.pop_back();
            }
        }
    }

    void skipStatement()
    {
        bool captureMember = false;
        SymbolKind mk = SymbolKind::Variable;
        if (!blocks_.empty())
        {
            BlockKind bk = blocks_.back().kind;
            if (bk == BlockKind::Type || bk == BlockKind::Union)
            {
                captureMember = true;
            }
            else if (bk == BlockKind::Enum)
            {
                captureMember = true;
                mk = SymbolKind::Const;
            }
        }
        for (;;)
        {
            TokenKind k = cur_.kind;
            if (k == TokenKind::Newline || k == TokenKind::Eof || k == TokenKind::Comment ||
                k == TokenKind::DocComment)
            {
                return;
            }
            if (k == TokenKind::Symbol && cur_.text() == ":")
            {
                return;
            }
            if (k == TokenKind::Symbol && cur_.text() == "_")
            {
                addDiagnostic(cur_.beg, cur_.end, Severity::Error, "bad-continuation",
                              "expected end of line after '_'");
            }
            if (captureMember && k == TokenKind::Identifier)
            {
                Symbol m;
                m.kind = mk;
                m.name = std::string(cur_.text());
                m.key = toLowerChars(m.name);
                m.selection.beg = m.range.beg = cur_.beg;
                m.selection.end = m.range.end = cur_.end;
                m.doc = takeDoc();
                addSymbol(std::move(m));
                captureMember = false;
            }
            advance();
        }
    }

    void handleStatement()
    {
        // Line label: Identifier directly followed by ':'.
        if (cur_.kind == TokenKind::Identifier)
        {
            Token nxt = lex_.peek(0);
            if (nxt.kind == TokenKind::Symbol && nxt.text() == ":")
            {
                Symbol s;
                s.kind = SymbolKind::Label;
                s.name = std::string(cur_.text());
                s.key = toLowerChars(s.name);
                s.selection.beg = s.range.beg = cur_.beg;
                s.selection.end = s.range.end = cur_.end;
                s.doc = takeDoc();
                addSymbol(std::move(s));
                advance();
                advance();
                return;
            }
        }
        if (cur_.kind != TokenKind::Keyword)
        {
            resetDoc();
            skipStatement();
            return;
        }

        std::string w = toLowerChars(cur_.text());

        if (w == "private" || w == "public" || w == "export" || w == "static")
        {
            Token nxt = lex_.peek(0);
            if (nxt.kind == TokenKind::Keyword &&
                (isDeclOpenerWord(toLowerChars(nxt.text())) ||
                 toLowerChars(nxt.text()) == "type"))
            {
                advance();
                handleStatement();
                return;
            }
            if (w == "static" && nxt.kind == TokenKind::Identifier)
            {
                handleVarDecls(SymbolKind::Dim);
                return;
            }
            resetDoc();
            skipStatement();
            return;
        }

        if (w == "end")
        {
            handleEnd();
            return;
        }
        if (w == "next" || w == "wend" || w == "loop")
        {
            BlockCloser c;
            blockForCloser(w, &c);
            handlePlainCloser(c);
            return;
        }
        if (isDeclOpenerWord(w))
        {
            handleDeclBlock(w, declKindFor(w));
            return;
        }
        if (w == "type")
        {
            handleType();
            return;
        }
        if (w == "declare")
        {
            handleDeclare();
            return;
        }
        if (w == "dim" || w == "redim" || w == "var" || w == "local" || w == "common" ||
            w == "const")
        {
            handleVarDecls(w == "const" ? SymbolKind::Const : SymbolKind::Dim);
            return;
        }
        if (w == "if")
        {
            handleIf();
            return;
        }
        if (w == "else" || w == "elseif")
        {
            if (blocks_.empty() || blocks_.back().kind != BlockKind::If)
            {
                addDiagnostic(cur_.beg, cur_.end, Severity::Error, "stray-closer",
                              "ELSE without IF");
            }
            resetDoc();
            skipStatement();
            return;
        }
        if (w == "case")
        {
            if (blocks_.empty() || blocks_.back().kind != BlockKind::Select)
            {
                addDiagnostic(cur_.beg, cur_.end, Severity::Error, "stray-closer",
                              "CASE without SELECT");
            }
            resetDoc();
            skipStatement();
            return;
        }
        if (w == "exit" || w == "continue")
        {
            resetDoc();
            skipStatement();
            return;
        }
        if (isControlOpenerWord(w))
        {
            pushBlockFromOpener(w);
            resetDoc();
            advance();
            skipStatement();
            return;
        }
        resetDoc();
        skipStatement();
    }

    void pushBlockFromOpener(const std::string& w)
    {
        BlockCloser c;
        blockForOpener(w, &c);
        Block b;
        b.kind = c.kind;
        b.close = c.closeWord;
        b.needsEnd = c.needsEnd;
        b.begOpen = cur_.beg;
        b.endOpen = cur_.end;
        blocks_.push_back(b);
    }

    void handleDeclBlock(const std::string& openWord, SymbolKind k)
    {
        Token openTok = cur_;
        BlockCloser closer;
        blockForOpener(openWord, &closer);
        advance();

        Symbol s;
        s.kind = k;
        s.range.beg = openTok.beg;
        if (cur_.kind == TokenKind::Identifier || cur_.kind == TokenKind::Keyword ||
            (k == SymbolKind::Operator && cur_.kind == TokenKind::Symbol))
        {
            s.name = std::string(cur_.text());
            s.key = toLowerChars(s.name);
            s.selection.beg = cur_.beg;
            s.selection.end = cur_.end;
            advance();
        }
        s.signature = headerText(openTok);
        s.doc = takeDoc();

        if (cur_.kind == TokenKind::Symbol && cur_.text() == "(")
        {
            populateParams(s);
        }

        Symbol* psym = addSymbol(std::move(s));

        Block b;
        b.kind = closer.kind;
        b.close = closer.closeWord;
        b.needsEnd = closer.needsEnd;
        b.sym = psym;
        b.begOpen = openTok.beg;
        b.endOpen = openTok.end;
        blocks_.push_back(b);
        if (psym)
        {
            containers_.push_back(Container(psym));
        }
        skipStatement();
    }

    void populateParams(Symbol& s)
    {
        advance();  // '('
        std::vector<Token> entry;
        int depth = 1;
        for (;;)
        {
            TokenKind k = cur_.kind;
            if (k == TokenKind::Eof || k == TokenKind::Newline)
            {
                break;
            }
            if (k == TokenKind::Symbol)
            {
                std::string_view t = cur_.text();
                if (t == "(")
                {
                    entry.push_back(cur_);
                    ++depth;
                    advance();
                    continue;
                }
                if (t == ")")
                {
                    if (depth == 1)
                    {
                        addParam(s, entry);
                        advance();
                        return;
                    }
                    --depth;
                    advance();
                    continue;
                }
                if (depth == 1 && t == ",")
                {
                    addParam(s, entry);
                    entry.clear();
                    advance();
                    continue;
                }
            }
            entry.push_back(cur_);
            advance();
        }
        addParam(s, entry);
    }

    void addParam(Symbol& s, const std::vector<Token>& entry)
    {
        for (const Token& t : entry)
        {
            if (t.kind == TokenKind::Identifier)
            {
                Symbol p;
                p.kind = SymbolKind::Parameter;
                p.name = std::string(t.text());
                p.key = toLowerChars(p.name);
                p.selection.beg = p.range.beg = t.beg;
                p.selection.end = p.range.end = t.end;
                s.children.push_back(std::move(p));
                break;
            }
        }
    }

    void handleType()
    {
        Token openTok = cur_;
        BlockCloser closer;
        blockForOpener("type", &closer);
        advance();

        Symbol s;
        s.kind = SymbolKind::Type;
        s.range.beg = openTok.beg;
        bool hasName = false;
        if (cur_.kind == TokenKind::Identifier || cur_.kind == TokenKind::Keyword)
        {
            hasName = true;
            s.name = std::string(cur_.text());
            s.key = toLowerChars(s.name);
            s.selection.beg = cur_.beg;
            s.selection.end = cur_.end;
            advance();
        }
        s.signature = headerText(openTok);
        s.doc = takeDoc();

        // Alias form: `TYPE name AS type` (rest of line, no ':' before 'as').
        bool alias = false;
        if (hasName)
        {
            if (cur_.kind == TokenKind::Newline || cur_.kind == TokenKind::Eof)
            {
                // name alone on the line -> UDT block
            }
            else if (cur_.kind == TokenKind::Keyword && toLowerChars(cur_.text()) == "as")
            {
                alias = true;
            }
            else if (cur_.kind == TokenKind::Symbol && cur_.text() == ":")
            {
                // one-line UDT `TYPE name : ... : END TYPE`
            }
            else
            {
                for (int i = 0; i < 512; ++i)
                {
                    Token t = lex_.peek(i);
                    if (t.kind == TokenKind::Newline || t.kind == TokenKind::Eof ||
                        t.kind == TokenKind::Comment)
                    {
                        break;
                    }
                    if (t.kind == TokenKind::Symbol && t.text() == ":")
                    {
                        break;
                    }
                    if (t.kind == TokenKind::Keyword && toLowerChars(t.text()) == "as")
                    {
                        alias = true;
                        break;
                    }
                }
            }
        }

        if (alias)
        {
            s.range.end = currentLineEnd();
            addSymbol(std::move(s));
            skipStatement();
            return;
        }

        Symbol* psym = hasName ? addSymbol(std::move(s)) : nullptr;
        Block b;
        b.kind = BlockKind::Type;
        b.close = closer.closeWord;
        b.needsEnd = closer.needsEnd;
        b.sym = psym;
        b.begOpen = openTok.beg;
        b.endOpen = openTok.end;
        blocks_.push_back(b);
        if (psym)
        {
            containers_.push_back(Container(psym));
        }
        skipStatement();
    }

    void handleDeclare()
    {
        Token openTok = cur_;
        advance();  // past DECLARE
        if (cur_.kind != TokenKind::Keyword)
        {
            resetDoc();
            skipStatement();
            return;
        }
        std::string w = toLowerChars(cur_.text());
        if (w != "sub" && w != "function" && w != "property")
        {
            resetDoc();
            skipStatement();
            return;
        }
        Symbol s;
        s.kind = w == "sub"   ? SymbolKind::Sub
                 : w == "function" ? SymbolKind::Function
                                   : SymbolKind::Property;
        advance();
        if (cur_.kind == TokenKind::Identifier || cur_.kind == TokenKind::Keyword)
        {
            s.name = std::string(cur_.text());
            s.key = toLowerChars(s.name);
            s.selection.beg = cur_.beg;
            s.selection.end = cur_.end;
            advance();
        }
        s.range.beg = openTok.beg;
        s.range.end = currentLineEnd();
        s.signature = headerText(openTok);
        s.doc = takeDoc();
        if (cur_.kind == TokenKind::Symbol && cur_.text() == "(")
        {
            populateParams(s);
        }
        addSymbol(std::move(s));
        skipStatement();
    }

    void handleVarDecls(SymbolKind k)
    {
        Token openTok = cur_;
        std::string doc = takeDoc();
        advance();
        bool atName = true;
        bool first = true;
        for (;;)
        {
            TokenKind tk = cur_.kind;
            if (tk == TokenKind::Newline || tk == TokenKind::Eof)
            {
                break;
            }
            if (tk == TokenKind::Symbol && cur_.text() == ":")
            {
                break;
            }
            if (tk == TokenKind::Symbol && cur_.text() == ",")
            {
                atName = true;
                advance();
                continue;
            }
            if (atName)
            {
                if (tk == TokenKind::Identifier)
                {
                    Symbol s;
                    s.kind = k;
                    s.name = std::string(cur_.text());
                    s.key = toLowerChars(s.name);
                    s.selection.beg = s.range.beg = cur_.beg;
                    s.selection.end = s.range.end = cur_.end;
                    s.signature = headerText(openTok);
                    if (first)
                    {
                        s.doc = doc;
                        first = false;
                    }
                    addSymbol(std::move(s));
                    atName = false;
                }
                else if (tk == TokenKind::Keyword && toLowerChars(cur_.text()) == "as")
                {
                    advance();
                    // Type-first form: `DIM AS <type> name`. Skip the type
                    // (builtin keyword or user-defined type) before the name.
                    if (cur_.kind == TokenKind::Keyword && isBuiltinType(toLowerChars(cur_.text())))
                    {
                        advance();
                    }
                    else if (cur_.kind == TokenKind::Identifier)
                    {
                        advance();
                    }
                    continue;
                }
                else
                {
                    advance();
                }
            }
            else
            {
                advance();
            }
        }
    }

    void handleIf()
    {
        Token ifTok = cur_;
        std::vector<Token> tail;
        tail.reserve(32);
        for (int i = 0; i < 512; ++i)
        {
            Token t = lex_.peek(i);
            if (t.kind == TokenKind::Newline || t.kind == TokenKind::Eof ||
                t.kind == TokenKind::Comment || t.kind == TokenKind::DocComment)
            {
                break;
            }
            tail.push_back(t);
        }

        int idxThen = -1;
        for (size_t i = 0; i < tail.size(); ++i)
        {
            if (tail[i].kind == TokenKind::Keyword && toLowerChars(tail[i].text()) == "then")
            {
                idxThen = static_cast<int>(i);
                break;
            }
        }

        bool singleLine = false;
        if (idxThen >= 0 && static_cast<size_t>(idxThen + 1) < tail.size())
        {
            Token firstAfter = tail[static_cast<size_t>(idxThen + 1)];
            bool colon = firstAfter.kind == TokenKind::Symbol && firstAfter.text() == ":";
            bool inlineEndIf = false;
            for (size_t i = static_cast<size_t>(idxThen + 1); i + 1 < tail.size(); ++i)
            {
                if (tail[i].kind == TokenKind::Keyword && tail[i + 1].kind == TokenKind::Keyword &&
                    toLowerChars(tail[i].text()) == "end" &&
                    toLowerChars(tail[i + 1].text()) == "if")
                {
                    inlineEndIf = true;
                    break;
                }
            }
            singleLine = !colon && !inlineEndIf;
        }

        if (singleLine)
        {
            resetDoc();
            advance();
            skipStatement();
            return;
        }

        Block b;
        b.kind = BlockKind::If;
        b.close = "if";
        b.needsEnd = true;
        b.begOpen = ifTok.beg;
        b.endOpen = ifTok.end;
        blocks_.push_back(b);
        resetDoc();
        advance();
        skipStatement();
    }

    void handleEnd()
    {
        Token endTok = cur_;
        advance();  // past END

        if (cur_.kind != TokenKind::Keyword)
        {
            resetDoc();
            skipStatement();
            return;
        }
        std::string w = toLowerChars(cur_.text());

        if (w == "for" || w == "while")
        {
            std::string expected = w == "for" ? "NEXT" : "WEND";
            addDiagnostic(cur_.beg, cur_.end, Severity::Error, "invalid-end",
                          "Expected '" + expected + "'");
            resetDoc();
            skipStatement();
            return;
        }

        BlockCloser c;
        if (!blockForCloser(w, &c) || !c.needsEnd)
        {
            resetDoc();
            skipStatement();
            return;
        }

        if (blocks_.empty())
        {
            addDiagnostic(endTok.beg, cur_.end, Severity::Error, "stray-closer",
                          "END " + uppercase(w) + " without " + uppercase(w));
            resetDoc();
            skipStatement();
            return;
        }

        Block& top = blocks_.back();
        if (top.kind == c.kind && top.needsEnd)
        {
            closeBlock(cur_.end);
            advance();
            resetDoc();
            skipStatement();
            return;
        }

        addDiagnostic(endTok.beg, cur_.end, Severity::Error, "closer-mismatch",
                      "Expected '" + displayFor(top) + "'");
        resetDoc();
        skipStatement();
    }

    void handlePlainCloser(const BlockCloser& c)
    {
        Token closerTok = cur_;
        if (blocks_.empty())
        {
            const char* msg = c.kind == BlockKind::For   ? "NEXT without FOR"
                              : c.kind == BlockKind::While ? "WEND without WHILE"
                                                           : "LOOP without DO";
            addDiagnostic(closerTok.beg, closerTok.end, Severity::Error, "stray-closer", msg);
            resetDoc();
            skipStatement();
            return;
        }
        Block& top = blocks_.back();
        if (top.kind == c.kind && !top.needsEnd)
        {
            closeBlock(closerTok.end);
            advance();
            resetDoc();
            skipStatement();
            return;
        }
        addDiagnostic(closerTok.beg, closerTok.end, Severity::Error, "closer-mismatch",
                      "Expected '" + displayFor(top) + "'");
        resetDoc();
        skipStatement();
    }

    void handlePreprocessor()
    {
        std::string w = toLowerChars(preprocessorWord(cur_.text()));
        if (w == "if" || w == "ifdef" || w == "ifndef")
        {
            Block b;
            b.kind = BlockKind::PreprocIf;
            b.close = "#endif";
            b.needsEnd = false;
            b.begOpen = cur_.beg;
            b.endOpen = cur_.end;
            blocks_.push_back(b);
        }
        else if (w == "macro")
        {
            Block b;
            b.kind = BlockKind::PreprocMacro;
            b.close = "#endmacro";
            b.needsEnd = false;
            b.begOpen = cur_.beg;
            b.endOpen = cur_.end;
            blocks_.push_back(b);
        }
        else if (w == "endif")
        {
            if (!blocks_.empty() && blocks_.back().kind == BlockKind::PreprocIf)
            {
                closeBlock(cur_.end);
            }
            else
            {
                addDiagnostic(cur_.beg, cur_.end, Severity::Error, "stray-closer",
                              "#ENDIF without #IF");
            }
        }
        else if (w == "endmacro")
        {
            if (!blocks_.empty() && blocks_.back().kind == BlockKind::PreprocMacro)
            {
                closeBlock(cur_.end);
            }
            else
            {
                addDiagnostic(cur_.beg, cur_.end, Severity::Error, "stray-closer",
                              "#ENDMACRO without #MACRO");
            }
        }
        else if (w == "lang")
        {
            LangMode m;
            if (langFromDirective(cur_.text(), &m))
            {
                applyLangDirective(m, cur_.beg, cur_.end);
            }
        }
    }
};

}  // namespace

ParseResult parseDocument(std::string_view source)
{
    return Parser(source).run();
}

}  // namespace fblang