#include "lexer.h"

#include "language.h"

#include <cctype>

namespace fblang {

namespace {

bool isIdentStart(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool isIdentChar(char c)
{
    return isIdentStart(c) || (c >= '0' && c <= '9');
}

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

bool isWhitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

bool isRadixPrefix(char c)
{
    return c == 'h' || c == 'H' || c == 'o' || c == 'O' || c == 'b' || c == 'B';
}

}  // namespace

Lexer::Lexer(std::string_view source)
    : src_(source)
{
    p_ = src_.data();
    end_ = src_.data() + src_.size();
}

char Lexer::peekChar(size_t ahead) const
{
    const char* q = p_ + ahead;
    return q < end_ ? *q : '\0';
}

char Lexer::advance()
{
    char c = *p_;
    if (p_ < end_)
    {
        ++p_;
    }
    return c;
}

bool Lexer::atLineStart() const
{
    const char* q = p_;
    while (q > src_.data() && *(q - 1) != '\n' && *(q - 1) != '\r')
    {
        --q;
    }
    while (q < p_ && isWhitespace(*q))
    {
        ++q;
    }
    return q == p_;
}

void Lexer::skipHorizontalWs()
{
    while (p_ < end_ && isWhitespace(*p_))
    {
        ++p_;
    }
}

bool Lexer::consumeNewline()
{
    if (p_ >= end_)
    {
        return false;
    }
    if (*p_ == '\r')
    {
        ++p_;
        if (p_ < end_ && *p_ == '\n')
        {
            ++p_;
        }
        return true;
    }
    if (*p_ == '\n')
    {
        ++p_;
        return true;
    }
    return false;
}

Token Lexer::next()
{
    if (!lookahead_.empty())
    {
        Token t = lookahead_.front();
        lookahead_.erase(lookahead_.begin());
        return t;
    }
    return lexNext();
}

Token Lexer::peek(size_t ahead)
{
    while (lookahead_.size() <= ahead)
    {
        lookahead_.push_back(lexNext());
    }
    return lookahead_[ahead];
}

Token Lexer::lexNext()
{
    for (;;)
    {
        skipHorizontalWs();
        if (p_ >= end_)
        {
            Token t;
            t.kind = TokenKind::Eof;
            t.beg = static_cast<uint32_t>(end_ - src_.data());
            t.end = t.beg;
            t.data = src_.data() + t.beg;
            return t;
        }
        if (consumeNewline())
        {
            Token t;
            t.kind = TokenKind::Newline;
            // The newline is one or two bytes (\r, \n, \r\n); widen to cover it.
            t.beg = static_cast<uint32_t>(p_ - src_.data());
            while (t.beg > 0 &&
                   (src_.data()[t.beg - 1] == '\n' || src_.data()[t.beg - 1] == '\r'))
            {
                --t.beg;
            }
            t.end = static_cast<uint32_t>(p_ - src_.data());
            t.data = src_.data() + t.beg;
            return t;
        }

        char c = *p_;

        // Line-leading '#' is a preprocessor directive (swallows the line).
        if (c == '#' && atLineStart())
        {
            uint32_t beg = static_cast<uint32_t>(p_ - src_.data());
            while (p_ < end_ && *p_ != '\n' && *p_ != '\r')
            {
                ++p_;
            }
            Token t;
            t.kind = TokenKind::Preprocessor;
            t.beg = beg;
            t.end = static_cast<uint32_t>(p_ - src_.data());
            t.data = src_.data() + beg;
            return t;
        }

        // Line-leading REM is a comment (swallows the line). It must be the
        // first token and followed by whitespace, a quote, or end of line.
        if (atLineStart() && p_ + 3 <= end_ &&
            (c == 'r' || c == 'R') && (peekChar(1) == 'e' || peekChar(1) == 'E') &&
            (peekChar(2) == 'm' || peekChar(2) == 'M') &&
            (p_ + 3 == end_ || isWhitespace(peekChar(3)) || peekChar(3) == '\''))
        {
            uint32_t beg = static_cast<uint32_t>(p_ - src_.data());
            while (p_ < end_ && *p_ != '\n' && *p_ != '\r')
            {
                ++p_;
            }
            Token t;
            t.kind = TokenKind::Comment;
            t.beg = beg;
            t.end = static_cast<uint32_t>(p_ - src_.data());
            t.data = src_.data() + beg;
            return t;
        }

        // Line-leading '$' is a legacy meta-command (swallows the line).
        if (c == '$' && atLineStart())
        {
            uint32_t beg = static_cast<uint32_t>(p_ - src_.data());
            while (p_ < end_ && *p_ != '\n' && *p_ != '\r')
            {
                ++p_;
            }
            Token t;
            t.kind = TokenKind::Meta;
            t.beg = beg;
            t.end = static_cast<uint32_t>(p_ - src_.data());
            t.data = src_.data() + beg;
            return t;
        }

        // Line-leading '///' is a doc comment (swallows the line).
        if (c == '/' && atLineStart() && peekChar(1) == '/' && peekChar(2) == '/')
        {
            uint32_t beg = static_cast<uint32_t>(p_ - src_.data());
            while (p_ < end_ && *p_ != '\n' && *p_ != '\r')
            {
                ++p_;
            }
            Token t;
            t.kind = TokenKind::DocComment;
            t.beg = beg;
            t.end = static_cast<uint32_t>(p_ - src_.data());
            t.data = src_.data() + beg;
            return t;
        }

        if (c == '\'')
        {
            return lexComment();
        }
        if (c == '"')
        {
            return lexString();
        }
        if (isIdentStart(c))
        {
            return lexIdentifier();
        }
        if (isDigit(c) || (c == '&' && isRadixPrefix(peekChar(1))) ||
            (c == '.' && peekChar(1) >= '0' && peekChar(1) <= '9'))
        {
            return lexNumber();
        }
        return lexSymbol();
    }
}

Token Lexer::lexComment()
{
    uint32_t beg = static_cast<uint32_t>(p_ - src_.data());
    // Only a line-leading '' turns a quote-comment into a doc comment; mid-line
    // lone '' (e.g. after ':' on a statement line) stays an ordinary comment.
    bool doc = p_ + 1 < end_ && *(p_ + 1) == '\'' && atLineStart();
    while (p_ < end_ && *p_ != '\n' && *p_ != '\r')
    {
        ++p_;
    }
    Token t;
    t.kind = doc ? TokenKind::DocComment : TokenKind::Comment;
    t.beg = beg;
    t.end = static_cast<uint32_t>(p_ - src_.data());
    t.data = src_.data() + beg;
    return t;
}

Token Lexer::lexString()
{
    uint32_t beg = static_cast<uint32_t>(p_ - src_.data());
    advance();  // opening quote
    bool terminated = false;
    while (p_ < end_ && *p_ != '\n' && *p_ != '\r')
    {
        if (*p_ == '"')
        {
            if (p_ + 1 < end_ && *(p_ + 1) == '"')
            {
                p_ += 2;  // doubled quote inside a string
                continue;
            }
            ++p_;
            terminated = true;
            break;
        }
        ++p_;
    }
    Token t;
    t.kind = TokenKind::String;
    t.beg = beg;
    t.end = static_cast<uint32_t>(p_ - src_.data());
    t.terminated = terminated;
    t.data = src_.data() + beg;
    return t;
}

Token Lexer::lexIdentifier()
{
    uint32_t beg = static_cast<uint32_t>(p_ - src_.data());
    while (p_ < end_ && isIdentChar(*p_))
    {
        ++p_;
    }
    const char* baseStart = src_.data() + beg;
    const char* baseEnd = p_;

    bool isBareUnderscore = (baseEnd - baseStart == 1 && *baseStart == '_');

    // Trailing '_' as the last thing on a logical line is a line continuation:
    // drop it and the newline so callers see one continuous logical line.
    if (isBareUnderscore)
    {
        const char* q = p_;
        while (q < end_ && isWhitespace(*q))
        {
            ++q;
        }
        if (q < end_ && (*q == '\n' || *q == '\r'))
        {
            p_ = q;
            consumeNewline();
            return lexNext();
        }
        Token t;
        t.kind = TokenKind::Symbol;
        t.beg = beg;
        t.end = beg + 1;
        t.data = baseStart;
        return t;
    }

    std::string_view base(baseStart, static_cast<size_t>(baseEnd - baseStart));
    bool isKeywordBase = isReservedWord(base);

    // A directly-attached suffix char belongs to the identifier — unless the
    // base is a reserved word (PRINT#1 is PRINT + "#1" channel, not a suffix).
    if (!isKeywordBase && p_ < end_ && isSuffixChar(*p_))
    {
        ++p_;
    }

    uint32_t end = static_cast<uint32_t>(p_ - src_.data());
    // Combined assignment keywords lex as one token only when the '=' is
    // directly attached (AND= works; "and =" stays two tokens).
    if (isKeywordBase && p_ < end_ && *p_ == '=')
    {
        if (base == "and" || base == "or" || base == "xor" || base == "eqv" ||
            base == "imp" || base == "mod" || base == "shl" || base == "shr")
        {
            ++p_;
            end = static_cast<uint32_t>(p_ - src_.data());
        }
    }

    Token t;
    t.kind = isKeywordBase ? TokenKind::Keyword : TokenKind::Identifier;
    t.beg = beg;
    t.end = end;
    t.data = src_.data() + beg;
    return t;
}

Token Lexer::lexNumber()
{
    uint32_t beg = static_cast<uint32_t>(p_ - src_.data());
    auto radixDigit = [](char c, int radix) {
        if (c >= '0' && c <= '9')
        {
            return c - '0' < radix;
        }
        return radix == 16 && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
    };

    if (*p_ == '&')
    {
        char r = static_cast<char>(std::tolower(static_cast<unsigned char>(peekChar(1))));
        int radix = r == 'h' ? 16 : r == 'o' ? 8 : r == 'b' ? 2 : 0;
        if (radix != 0)
        {
            advance();
            advance();
            while (p_ < end_ && radixDigit(*p_, radix))
            {
                ++p_;
            }
            Token t;
            t.kind = TokenKind::Number;
            t.beg = beg;
            t.end = static_cast<uint32_t>(p_ - src_.data());
            t.data = src_.data() + beg;
            return t;
        }
    }

    if (*p_ == '.')
    {
        advance();
        while (p_ < end_ && isDigit(*p_))
        {
            ++p_;
        }
    }
    else
    {
        while (p_ < end_ && isDigit(*p_))
        {
            ++p_;
        }
        if (p_ < end_ && *p_ == '.' && p_ + 1 < end_ && isDigit(*(p_ + 1)))
        {
            ++p_;
            while (p_ < end_ && isDigit(*p_))
            {
                ++p_;
            }
        }
        if (p_ < end_ && (*p_ == 'e' || *p_ == 'E'))
        {
            const char* q = p_ + 1;
            if (q < end_ && (*q == '+' || *q == '-'))
            {
                ++q;
            }
            if (q < end_ && isDigit(*q))
            {
                p_ = q;
                while (p_ < end_ && isDigit(*p_))
                {
                    ++p_;
                }
            }
        }
    }
    if (p_ < end_ && isSuffixChar(*p_))
    {
        ++p_;
    }
    Token t;
    t.kind = TokenKind::Number;
    t.beg = beg;
    t.end = static_cast<uint32_t>(p_ - src_.data());
    t.data = src_.data() + beg;
    return t;
}

Token Lexer::lexSymbol()
{
    uint32_t beg = static_cast<uint32_t>(p_ - src_.data());
    char c = *p_;
    auto finish = [&](uint32_t end, std::string_view text) -> Token {
        Token t;
        t.kind = TokenKind::Symbol;
        t.beg = beg;
        t.end = end;
        t.data = text.data();
        return t;
    };

    switch (c)
    {
    case '.':
        if (peekChar(1) == '.' && peekChar(2) == '.')
        {
            p_ += 3;
            return finish(beg + 3, "...");
        }
        ++p_;
        return finish(beg + 1, ".");
    case '-':
        if (peekChar(1) == '>')
        {
            p_ += 2;
            return finish(beg + 2, "->");
        }
        if (peekChar(1) == '=')
        {
            p_ += 2;
            return finish(beg + 2, "-=");
        }
        ++p_;
        return finish(beg + 1, "-");
    case '+':
        if (peekChar(1) == '=')
        {
            p_ += 2;
            return finish(beg + 2, "+=");
        }
        ++p_;
        return finish(beg + 1, "+");
    case '*':
        if (peekChar(1) == '=')
        {
            p_ += 2;
            return finish(beg + 2, "*=");
        }
        ++p_;
        return finish(beg + 1, "*");
    case '/':
        if (peekChar(1) == '=')
        {
            p_ += 2;
            return finish(beg + 2, "/=");
        }
        ++p_;
        return finish(beg + 1, "/");
    case '\\':
        if (peekChar(1) == '=')
        {
            p_ += 2;
            return finish(beg + 2, "\\=");
        }
        ++p_;
        return finish(beg + 1, "\\");
    case '&':
        if (peekChar(1) == '=')
        {
            p_ += 2;
            return finish(beg + 2, "&=");
        }
        ++p_;
        return finish(beg + 1, "&");
    case '<':
        if (peekChar(1) == '=')
        {
            p_ += 2;
            return finish(beg + 2, "<=");
        }
        if (peekChar(1) == '>')
        {
            p_ += 2;
            return finish(beg + 2, "<>");
        }
        ++p_;
        return finish(beg + 1, "<");
    case '>':
        if (peekChar(1) == '=')
        {
            p_ += 2;
            return finish(beg + 2, ">=");
        }
        ++p_;
        return finish(beg + 1, ">");
    default:
    {
        ++p_;
        return finish(beg + 1, std::string_view(p_ - 1, 1));
    }
    }
}

}  // namespace fblang