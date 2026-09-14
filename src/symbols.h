#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// FreeBASIC language model shared by the lexer, parser, index, and session.
// Everything here is LSP-agnostic and works in byte offsets; the session
// converts to/from LSP positions only at the protocol boundary.

namespace fblang {

struct SourceRange {
    uint32_t beg = 0;
    uint32_t end = 0;
};

enum class SymbolKind {
    Sub,
    Function,
    Property,
    Constructor,
    Destructor,
    Operator,
    Type,
    Union,
    Enum,
    Namespace,
    Scope,
    Const,
    Dim,
    Label,
    Parameter,
    Variable
};

// Per-document symbol. Ranges are byte offsets into the source buffer.
struct Symbol {
    std::string name;       // display name (original case + suffix char)
    std::string key;        // canonical lookup key = lowercase name incl. suffix
    SymbolKind kind = SymbolKind::Variable;
    SourceRange range;      // whole construct (SUB ... END SUB); decls: the statement
    SourceRange selection;  // name token
    std::string signature;  // readable declaration header (for hover/details)
    std::string doc;        // /// or '' doc-comment block directly above
    std::vector<Symbol> children;
};

enum class Severity {
    Error = 1,
    Warning = 2,
    Information = 3,
    Hint = 4
};

struct Diagnostic {
    SourceRange range;
    Severity severity = Severity::Error;
    std::string code;
    std::string message;
};

// Block kinds used for block matching and block-based folding.
enum class BlockKind {
    Sub,
    Function,
    Property,
    Operator,
    Constructor,
    Destructor,
    Type,
    Union,
    Enum,
    Namespace,
    Scope,
    If,
    Select,
    For,
    While,
    Do,
    With,
    Extern,
    Asm,
    PreprocIf,
    PreprocMacro
};

struct ParseResult {
    std::vector<Symbol> roots;
    std::vector<Diagnostic> diagnostics;
    std::vector<SourceRange> blockRanges;  // every nested block, useful for folding

    // Dialect the file declares via #LANG; "fb" when none or unsupported. Only
    // "fb" is implemented; other modes are parsed best-effort for now.
    std::string lang = "fb";
};

inline std::string toLowerChars(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
        out.push_back(c);
    }
    return out;
}

}  // namespace fblang