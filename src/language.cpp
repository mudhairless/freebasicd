#include "language.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

namespace fblang {

namespace {

constexpr char const* kReserved[] = {
    "abs",
    "abstract",
    "access",
    "acos",
    "alias",
    "allocate",
    "and",
    "andalso",
    "any",
    "append",
    "as",
    "asc",
    "asin",
    "asm",
    "assert",
    "assertwarn",
    "atan2",
    "atn",
    "base",
    "beep",
    "bin",
    "binary",
    "bit",
    "bitreset",
    "bitset",
    "bload",
    "boolean",
    "bsave",
    "byref",
    "byte",
    "byval",
    "call",
    "callocate",
    "case",
    "cast",
    "cbool",
    "cbyte",
    "cdbl",
    "cdecl",
    "chain",
    "chdir",
    "chr",
    "cint",
    "circle",
    "class",
    "clear",
    "clng",
    "clngint",
    "close",
    "cls",
    "color",
    "command",
    "common",
    "condbroadcast",
    "condcreate",
    "conddestroy",
    "condsignal",
    "condwait",
    "const",
    "constructor",
    "continue",
    "cos",
    "cptr",
    "cshort",
    "csign",
    "csng",
    "csrlin",
    "cubyte",
    "cuint",
    "culng",
    "culngint",
    "cunsg",
    "curdir",
    "cushort",
    "cvd",
    "cvi",
    "cvl",
    "cvlongint",
    "cvs",
    "cvshort",
    "data",
    "date",
    "deallocate",
    "declare",
    "defbyte",
    "defdbl",
    "defined",
    "defint",
    "deflng",
    "deflongint",
    "defshort",
    "defsng",
    "defstr",
    "defubyte",
    "defuint",
    "defulongint",
    "defushort",
    "delete",
    "destructor",
    "dim",
    "dir",
    "do",
    "double",
    "draw",
    "dylibfree",
    "dylibload",
    "dylibsymbol",
    "else",
    "elseif",
    "encoding",
    "end",
    "endif",
    "enum",
    "environ",
    "eof",
    "eqv",
    "erase",
    "erfn",
    "erl",
    "ermn",
    "err",
    "error",
    "exec",
    "exepath",
    "exit",
    "exp",
    "export",
    "extends",
    "extern",
    "false",
    "field",
    "fix",
    "flip",
    "for",
    "frac",
    "fre",
    "freefile",
    "function",
    "get",
    "getjoystick",
    "getkey",
    "getmouse",
    "gosub",
    "goto",
    "hex",
    "hibyte",
    "hiword",
    "if",
    "iif",
    "imageconvertrow",
    "imagecreate",
    "imagedestroy",
    "imageinfo",
    "imp",
    "implements",
    "import",
    "inkey",
    "inp",
    "input",
    "instr",
    "instrrev",
    "int",
    "integer",
    "is",
    "kill",
    "lbound",
    "lcase",
    "left",
    "len",
    "let",
    "lib",
    "line",
    "lobyte",
    "loc",
    "local",
    "locate",
    "lock",
    "lof",
    "log",
    "long",
    "longint",
    "loop",
    "loword",
    "lpos",
    "lprint",
    "lset",
    "ltrim",
    "mid",
    "mkd",
    "mkdir",
    "mki",
    "mkl",
    "mklongint",
    "mks",
    "mkshort",
    "mod",
    "multikey",
    "mutexcreate",
    "mutexdestroy",
    "mutexlock",
    "mutexunlock",
    "name",
    "namespace",
    "new",
    "next",
    "not",
    "object",
    "oct",
    "offsetof",
    "open",
    "operator",
    "or",
    "orelse",
    "out",
    "output",
    "overload",
    "paint",
    "palette",
    "pascal",
    "pcopy",
    "peek",
    "pmap",
    "point",
    "pointcoord",
    "pointer",
    "poke",
    "pos",
    "preserve",
    "preset",
    "print",
    "private",
    "procptr",
    "property",
    "pset",
    "ptr",
    "public",
    "put",
    "random",
    "randomize",
    "read",
    "reallocate",
    "redim",
    "rem",
    "reset",
    "restore",
    "resume",
    "return",
    "rgb",
    "rgba",
    "right",
    "rmdir",
    "rnd",
    "rset",
    "rtrim",
    "run",
    "sadd",
    "scope",
    "screen",
    "screencontrol",
    "screencopy",
    "screenevent",
    "screenglproc",
    "screeninfo",
    "screenlist",
    "screenlock",
    "screenptr",
    "screenres",
    "screenset",
    "screensync",
    "screenunlock",
    "seek",
    "select",
    "setdate",
    "setenviron",
    "setmouse",
    "settime",
    "sgn",
    "shared",
    "shell",
    "shl",
    "short",
    "shr",
    "sin",
    "single",
    "sizeof",
    "sleep",
    "space",
    "spc",
    "sqr",
    "static",
    "stdcall",
    "step",
    "stop",
    "str",
    "string",
    "strptr",
    "sub",
    "swap",
    "system",
    "tab",
    "tan",
    "then",
    "threadcall",
    "threadcreate",
    "threadwait",
    "time",
    "timer",
    "to",
    "trim",
    "true",
    "type",
    "typeof",
    "ubound",
    "ubyte",
    "ucase",
    "uinteger",
    "ulong",
    "ulongint",
    "union",
    "unlock",
    "unsigned",
    "until",
    "ushort",
    "using",
    "val",
    "valint",
    "vallng",
    "valuint",
    "valulng",
    "var",
    "varptr",
    "view",
    "virtual",
    "wait",
    "wbin",
    "wchr",
    "wend",
    "whex",
    "while",
    "width",
    "window",
    "windowtitle",
    "winput",
    "with",
    "woct",
    "write",
    "wspace",
    "wstr",
    "wstring",
    "xor",
    "zstring",
};

constexpr char const* kBuiltinTypes[] = {
    "any", "boolean", "byte", "double", "integer", "long", "longint",
    "object", "pointer", "ptr", "short", "single", "string", "ubyte",
    "uinteger", "ulong", "ulongint", "ushort", "wstring", "zstring",
};

// Reserved words that are never legitimate identifiers start at this point;
// keep children under a cheap `int` table.
struct BlockRow {
    char const* opener;  // lowercase opening word
    BlockKind kind;
    char const* close;   // closer after "end" (needsEnd) or the bare closer
    bool needsEnd;
};

constexpr BlockRow kBlockOpeners[] = {
    {"sub", BlockKind::Sub, "sub", true},
    {"function", BlockKind::Function, "function", true},
    {"property", BlockKind::Property, "property", true},
    {"operator", BlockKind::Operator, "operator", true},
    {"constructor", BlockKind::Constructor, "constructor", true},
    {"destructor", BlockKind::Destructor, "destructor", true},
    {"type", BlockKind::Type, "type", true},
    {"union", BlockKind::Union, "union", true},
    {"enum", BlockKind::Enum, "enum", true},
    {"namespace", BlockKind::Namespace, "namespace", true},
    {"scope", BlockKind::Scope, "scope", true},
    {"if", BlockKind::If, "if", true},
    {"select", BlockKind::Select, "select", true},
    {"with", BlockKind::With, "with", true},
    {"extern", BlockKind::Extern, "extern", true},
    {"asm", BlockKind::Asm, "asm", true},
    {"for", BlockKind::For, "next", false},
    {"while", BlockKind::While, "wend", false},
    {"do", BlockKind::Do, "loop", false},
};

// Words that only ever appear as closers (never open a block).
constexpr BlockRow kCloserOnly[] = {
    {"next", BlockKind::For, "next", false},
    {"wend", BlockKind::While, "wend", false},
    {"loop", BlockKind::Do, "loop", false},
};

// FreeBASIC wiki page suffixes that do not match the naive `KeyPg<Word>` rule.
struct DocsPage
{
    char const* word;
    char const* page;
};
constexpr DocsPage kDocsPages[] = {
    {"and", "OpAnd"},             {"andalso", "OpAndAlso"},
    {"condbroadcast", "CondBroadcast"}, {"condcreate", "CondCreate"},
    {"conddestroy", "CondDestroy"},     {"condsignal", "CondSignal"},
    {"condwait", "CondWait"},           {"delete", "OpDelete"},
    {"eqv", "OpEqv"},                   {"get", "Getfileio"},
    {"if", "Ifthen"},                   {"imageconvertrow", "ImageConvertRow"},
    {"imagedestroy", "ImageDestroy"},   {"imageinfo", "ImageInfo"},
    {"imp", "OpImp"},                   {"line", "Linegraphics"},
    {"lobyte", "LoByte"},               {"loword", "LoWord"},
    {"mid", "Midfunction"},             {"mod", "OpModulus"},
    {"mutexcreate", "MutexCreate"},     {"mutexdestroy", "MutexDestroy"},
    {"mutexlock", "MutexLock"},         {"mutexunlock", "MutexUnlock"},
    {"new", "OpNew"},                   {"not", "OpNot"},
    {"or", "OpOr"},                     {"orelse", "OpOrElse"},
    {"pointcoord", "PointCoord"},       {"pointer", "Ptr"},
    {"procptr", "OpProcptr"},           {"put", "Putfileio"},
    {"screen", "Screengraphics"},       {"seek", "Seekset"},
    {"select", "Selectcase"},           {"shl", "OpShiftLeft"},
    {"shr", "OpShiftRight"},            {"strptr", "OpStrptr"},
    {"threadcall", "ThreadCall"},       {"threadcreate", "ThreadCreate"},
    {"threadwait", "ThreadWait"},       {"varptr", "OpVarptr"},
    {"view", "Viewgraphics"},           {"xor", "OpXor"},
};

}  // namespace

bool isReservedWord(std::string_view word)
{
    // Reserve suffixes never apply to keywords; look the bare word up.
    return std::binary_search(std::begin(kReserved), std::end(kReserved), std::string(word));
}

std::vector<std::string_view> reservedWords()
{
    std::vector<std::string_view> out;
    out.reserve(std::size(kReserved));
    for (char const* w : kReserved)
    {
        out.emplace_back(w);
    }
    return out;
}

std::string keywordDocsUrl(std::string_view word)
{
    std::string const lower = toLowerChars(word);
    if (!isReservedWord(lower))
    {
        return {};
    }
    auto it = std::partition_point(
        std::begin(kDocsPages), std::end(kDocsPages),
        [&](DocsPage const& d) { return std::string_view(d.word) < lower; });
    std::string page;
    if (it != std::end(kDocsPages) && it->word == lower)
    {
        page = it->page;
    }
    else
    {
        page = lower;
        page[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(lower[0])));
    }
    return "https://www.freebasic.net/wiki/KeyPg" + page;
}

bool isBuiltinType(std::string_view wordLower)
{
    for (char const* t : kBuiltinTypes)
    {
        if (wordLower == t)
        {
            return true;
        }
    }
    return false;
}

namespace {

BlockCloser fromRow(const BlockRow& r)
{
    BlockCloser c;
    c.kind = r.kind;
    c.closeWord = r.close;
    c.needsEnd = r.needsEnd;
    return c;
}

}  // namespace

bool blockForOpener(std::string_view wordLower, BlockCloser* out)
{
    for (const auto& r : kBlockOpeners)
    {
        if (wordLower == r.opener)
        {
            if (out)
            {
                *out = fromRow(r);
            }
            return true;
        }
    }
    return false;
}

bool blockForCloser(std::string_view wordLower, BlockCloser* out)
{
    if (out)
    {
        *out = {};
    }
    for (const auto& r : kBlockOpeners)
    {
        if (r.needsEnd && wordLower == r.close)
        {
            if (out)
            {
                *out = fromRow(r);
            }
            return true;
        }
    }
    for (const auto& r : kCloserOnly)
    {
        if (wordLower == r.opener)
        {
            if (out)
            {
                *out = fromRow(r);
            }
            return true;
        }
    }
    return false;
}

std::string closerDisplay(const BlockCloser& closer)
{
    std::string s;
    if (closer.needsEnd)
    {
        s = "END ";
    }
    for (char c : closer.closeWord)
    {
        s.push_back(static_cast<char>(c - 'a' + 'A'));
    }
    return s;
}

std::string_view preprocessorWord(std::string_view line)
{
    // line starts at '#', skip the '#', any whitespace, then take the word.
    size_t i = 1;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
    {
        ++i;
    }
    size_t beg = i;
    while (i < line.size() &&
           ((line[i] >= 'a' && line[i] <= 'z') || (line[i] >= 'A' && line[i] <= 'Z') ||
            line[i] == '_'))
    {
        ++i;
    }
    if (i == beg)
    {
        return {};
    }
    // Directive names are compared case-insensitively downstream; return raw
    // and let callers lowercase.
    return line.substr(beg, i - beg);
}

bool isSuffixChar(char c)
{
    return c == '$' || c == '%' || c == '&' || c == '!' || c == '#';
}

bool langFromDirective(std::string_view line, LangMode* out)
{
    // Line starts at '#'. Expect `#LANG "name"`.
    size_t i = 1;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
    {
        ++i;
    }
    size_t wbeg = i;
    while (i < line.size() && ((line[i] >= 'a' && line[i] <= 'z') ||
                               (line[i] >= 'A' && line[i] <= 'Z')))
    {
        ++i;
    }
    std::string_view word = line.substr(wbeg, i - wbeg);
    if (word.size() != 4 ||
        (word[0] != 'l' && word[0] != 'L') || (word[1] != 'a' && word[1] != 'A') ||
        (word[2] != 'n' && word[2] != 'N') || (word[3] != 'g' && word[3] != 'G'))
    {
        return false;
    }
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
    {
        ++i;
    }
    if (i >= line.size() || line[i] != '"')
    {
        return false;
    }
    ++i;
    size_t sbeg = i;
    while (i < line.size() && line[i] != '"')
    {
        ++i;
    }
    if (i >= line.size())
    {
        return false;
    }
    std::string_view name = line.substr(sbeg, i - sbeg);

    LangMode mode;
    if (name == "fb")
    {
        mode = LangMode::Fb;
    }
    else if (name == "fblite")
    {
        mode = LangMode::FbLite;
    }
    else if (name == "qb")
    {
        mode = LangMode::Qb;
    }
    else if (name == "deprecated")
    {
        mode = LangMode::Deprecated;
    }
    else
    {
        return false;
    }
    if (out)
    {
        *out = mode;
    }
    return true;
}

const char* langName(LangMode mode)
{
    switch (mode)
    {
    case LangMode::Fb: return "fb";
    case LangMode::FbLite: return "fblite";
    case LangMode::Qb: return "qb";
    case LangMode::Deprecated: return "deprecated";
    }
    return "fb";
}

bool langFromMetaDirective(std::string_view text, LangMode* out)
{
    // `$`-metacommands live inside comments: `'$LANG: "qb"` or `rem $LANG:"qb"`.
    // The comment body is passed in; scan for `$lang` (case-insensitive)
    // followed by whitespace, an optional ':', and a quoted dialect name.
    auto ci = [](char c, char want) { return (c | 0x20) == want; };
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t n = text.size();
    for (size_t i = 0; i + 4 < n; ++i)
    {
        if (text[i] != '$' || !ci(text[i + 1], 'l') || !ci(text[i + 2], 'a') ||
            !ci(text[i + 3], 'n') || !ci(text[i + 4], 'g'))
        {
            continue;
        }
        size_t j = i + 5;
        while (j < n && ws(text[j]))
        {
            ++j;
        }
        if (j < n && text[j] == ':')
        {
            ++j;
            while (j < n && ws(text[j]))
            {
                ++j;
            }
        }
        if (j >= n || text[j] != '"')
        {
            continue;
        }
        ++j;
        size_t sbeg = j;
        while (j < n && text[j] != '"')
        {
            ++j;
        }
        if (j >= n)
        {
            return false;
        }
        std::string_view name = text.substr(sbeg, j - sbeg);
        LangMode mode;
        if (name == "fb")
        {
            mode = LangMode::Fb;
        }
        else if (name == "fblite")
        {
            mode = LangMode::FbLite;
        }
        else if (name == "qb")
        {
            mode = LangMode::Qb;
        }
        else if (name == "deprecated")
        {
            mode = LangMode::Deprecated;
        }
        else
        {
            continue;
        }
        if (out)
        {
            *out = mode;
        }
        return true;
    }
    return false;
}

}  // namespace fblang
