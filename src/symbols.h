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

// A `#include [once] "target"` directive as written in the source. Byte-offset
// ranges; `target` spans the filename literal (quotes excluded). Resolution to
// an absolute path happens at the index boundary (resolveIncludeTarget).
struct IncludeDirective {
    SourceRange line;      // whole `#include ...` line
    SourceRange target;    // filename literal range (quotes excluded)
    std::string literal;   // filename as written, case preserved
    bool once = false;     // `#include once`; `#pragma once` is recorded at the
                           // document/file level, not on the edge
};

// One usage of a symbol. `moduleScope` = true when the usage sits at module
// level (inside no block), i.e. the site is exposed to the #include closure
// for cross-file resolution.
struct Occurrence {
    SourceRange range;
    bool moduleScope = true;
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

    // M5 occurrence projection. `moduleScope` (true for file roots) marks
    // declarations whose key is cross-file reachable through the #include
    // closure; `occurrences` holds every usage site that resolved to this
    // symbol at analyze time, sorted by `range.beg` with the name token
    // itself excluded. Open buffers populate both; the disk cache stores them.
    bool moduleScope = false;
    std::vector<Occurrence> occurrences;
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