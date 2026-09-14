# FreeBASIC LSP Server — Implementation Plan

## 1. Context and current state

- Repository: a git working tree, initialized with default branch `main`; root at `/home/mud/Projects/freebasic/lsp`.
- AGENTS.md: implement an LSP server for FreeBASIC in C++17, cross-platform. LspCpp is the LSP/JSON-RPC dependency; simdjson and `langservercpp` are **not used** (see §2, §5).
- Toolchain verified: `g++`, `clang++`, `cmake` (no ninja), `fbc` 1.10.2.

## 2. Library decision: adopt LspCpp (kuafuwang/LspCpp)

**Decision: replace `langservercpp` with [LspCpp](https://github.com/kuafuwang/LspCpp)**, which provides everything we would otherwise hand-roll (framing, JSON-RPC dispatch, typed LSP message structs, position handling), so our code stays focused on FreeBASIC itself.

### Analysis summary

What LspCpp gives us (MIT, commit `19150d12`, no release tag yet, C++17, CMake 3.16+):
- **Transport + framing**: `LanguageSession.startStdio()` reads/writes Content-Length–framed LSP messages; TCP/WebSocket also available (stdio is all we need).
- **Typed LSP 3.17 message set**: `td_<feature>::request/response` and `Notify_<Name>::notify` structs generated via `MAKE_REFLECT_STRUCT` + RapidJSON reflection. Every feature we need exists — `td_initialize`, `td_documentSymbol`, `td_completion`, `td_hover`, `td_definition`, `td_references`, `td_documentHighlight`, `td_foldingRange`, `td_signatureHelp`, `td_rename`, `Notify_TextDocumentPublishDiagnostics`, plus `SemanticTokens.h`/`inlayHint.h` for later.
- **`WorkingFiles`**: open-document registry, incremental updates, and precomputed line offsets with `GetOffsetForPosition(lsPosition)` → byte offset. This directly replaces the `Document` class in the previous plan.
- **Ordering/cancellation**: notifications run FIFO so `didChange` order is preserved; requests run after prior notifications (gate); `$/cancelRequest` is threaded through via `CancelMonitor` for long ops.
- **Error responses**: `lsp::ResponseOrError<T>`, `lsp::RequestError`, `makeRequestCancelledError()` helpers.
- **Capabilities**: `initialize` response capabilities structs are present.

Tradeoffs / things to accept:
- **JSON engine is RapidJSON (bundled).** simdjson is dropped (user decision, §5); LspCpp handles all protocol JSON, so none of our code parses protocol JSON.
- **Dependency weight**: bundles Asio, RapidJSON, utfcpp, IXWebSocket (+ zlib for WebSocket). We configure `LSPCPP_BUILD_WEBSOCKETS=OFF` to skip the WebSocket stack; library needs no Boost (standalone Asio is the default).
- **Multithreaded by design**: parse pool + FIFO notification thread + handler pool (`max_workers`, default 2). Multiple request handlers can run concurrently → our shared language state needs locking (see §7).
- **Young API**: register early with a pinned commit; treat API churn as a patch risk, not a blocker.
- **CMake floor becomes 3.16** (repo currently declares 3.14).

### Vendoring method

The repo is a git working tree, so LspCpp is added as a **git submodule** pinned to commit `19150d12c4ae26239d75258ed598ba8ea3587cb7` (master head, 2026-08-21; the only project tag is `boost_version` — there is **no `v1.0.3` release tag yet**). Consumption is `add_subdirectory(third_party/LspCpp)` linking the `lspcpp` target (their supported in-tree mode). A submodule records the pinned SHA in `.gitmodules` + the git index, and anyone can restore it with `git submodule update --init`. If the submodule cannot be fetched (offline), fall back to FetchContent with a pinned URL.

## 3. Functional scope (unchanged in substance)

### Milestone 1 — Bring-up on LspCpp (M1)
- Vendor LspCpp, build the `lspcpp` target.
- `LanguageSession`: register `initialize`/`shutdown`/`exit`, `didOpen`/`didChange`/`didSave`/`didClose`.
- Wire `WorkingFiles` for buffer state; reparse on `didChange`; push diagnostics after each change.
- Capabilities: `textDocumentSync` (openClose, change=Incremental), `positionEncoding: "utf-16"` (LspCpp default).

### Milestone 2 — FreeBASIC lexer + parser (M2)
Pure language layer, no LSP coupling:
- Tokenizer over FreeBASIC's full syntax surface.
- Block recognizer: `SUB/FUNCTION/PROPERTY/OPERATOR/CONSTRUCTOR/DESTRUCTOR ... END *`, `TYPE/UNION/ENUM ... END *`, `IF ... END IF`, `SELECT`, `FOR`, `WHILE`, `DO`, `WITH`, `NAMESPACE/MODULE/SCOPE`, `#IF..#ENDIF`.
- Declaration extractor producing a per-document symbol tree.
- Diagnostics: unterminated block, stray/unmatched `END`, unclosed string, bad continuation, duplicate declaration (warning).

### Milestone 3 — LSP features (M3)
Backed by M1 + M2, implemented as typed LspCpp handlers:
- `textDocument/publishDiagnostics` (push).
- `textDocument/documentSymbol` (hierarchical outline).
- `textDocument/hover` (kind, signature, `///`/`''` doc comment).
- `textDocument/definition`, `references`, `documentHighlight`.
- `textDocument/completion` (keywords, `END`-block snippets, in-scope symbols).
- `textDocument/foldingRange` (block-based).
- `textDocument/signatureHelp` (best-effort from parameter lists).

### Milestone 4 — Cross-file indexing + polish (M4, stretch)
- Directory scan for `.bas`/`.bi`; workspace symbol index `key -> (defs, refs)`.
- `textDocument/rename` (workspace) using the index; `workspace/symbol`.
- `#include once` resolution reading referenced `.bi` files.
- `SemanticTokens` / inlay hints only if time permits.

## 4. Module map and key types

```
src/main.cpp          entry point; constructs session, startStdio(), waits on exit Condition
src/session.h/cpp     lsp::LanguageSession wiring; registers all handlers; owns language state;
                      converts parser output → LspCpp typed structs
src/symbols.h         shared model: Symbol, SymbolKind, Occurrence, BlockKind (used by parser + index)
src/lexer.h/cpp       FreeBASIC tokenizer
src/parser.h/cpp      block matcher + declaration extraction + diagnostics (byte-offset ranges)
src/language.h/cpp    FreeBASIC facts: keyword set, completion snippets, hover breadcrumbs
src/index.h/cpp       parse cache per document; (M4) workspace symbol index
src/utf16.h/cpp       byte ↔ UTF-16 position adapter for WorkingFile line offsets
tests/                ctest drivers: lexer_checks, parser_checks, utf16_checks, session_integration
```

### session.h (sketch)
```cpp
class FreeBasicServer {
public:
    explicit FreeBasicServer(lsp::LanguageSession& session);
    void registerHandlers();          // server.on(...) for every advertised method

private:
    // language state shared by request handlers (guarded, see §7)
    struct State;
    std::shared_ptr<State> state_;

    // handlers
    lsp::td_initialize::response   onInitialize(const lsp::td_initialize::request&);
    void onDidChange(const lsp::Notify_TextDocumentDidChange::notify&);   // reparse + publish
    void publishDiagnostics(const lsp::AbsolutePath&, int version, const std::vector<lsp::Diagnostic>&);
    // ... per-feature handlers
};
```

### symbols.h (shared model)
```cpp
enum class SymbolKind { Sub, Function, Property, Constructor, Destructor, Operator,
                        Type, Union, Enum, Namespace, Module, Const, Dim, Label,
                        Parameter, Variable };
enum class BlockKind   { Sub, Function, Property, Operator, Constructor, Destructor,
                        Type, Union, Enum, Namespace, Module, Scope, If, Select,
                        For, While, Do, With, PreprocIf };

struct Symbol {
    std::string name;       // display name (original case + suffix char)
    std::string key;        // normalized index key = lowercase(name incl. suffix)
    SymbolKind  kind;
    lsp::Range  range;      // whole construct (SUB → END SUB)
    lsp::Range  selection;  // name token
    std::string signature;  // readable decl for hover/details
    std::string doc;        // /// or '' doc-comment body above declaration
    std::vector<Symbol> children;
};

struct Occurrence {         // definition or reference site, byte-offset based
    std::string key;
    uint32_t    beg, end;   // byte offsets in document
    bool        isDefinition;
    std::string parentKey;
};
```

### parser.h (sketch)
```cpp
struct ParseResult {
    std::vector<Symbol>      roots;
    std::vector<Occurrence>  occurrences;  // per-document defs/refs
    std::vector<Diagnostic>  diagnostics;  // byte-offset ranges, converted later
};

ParseResult parseDocument(std::string_view text);
```

### lexer.h (sketch, unchanged from prior plan)
```cpp
enum class TokenKind { Identifier, Suffix, Keyword, Number, String, Comment,
                       DocComment, Preprocessor, Symbol, Newline, Eof };
struct Token { TokenKind kind; const char* start; const char* end; uint32_t beg; uint32_t len; };
class Lexer { public: explicit Lexer(std::string_view text);
              Token next(); Token peek(size_t ahead = 0); };
```

### utf16.h (LspCpp gap-filler)
LspCpp gives `WorkingFile::GetOffsetForPosition(lsPosition)` (UTF-16 → byte). We need the reverse to turn parser byte ranges into `lsRange`. Small adapter:
```cpp
lsPosition utf16_position(const WorkingFile& f, uint32_t byteOffset);
lsRange     utf16_range(const WorkingFile& f, uint32_t beg, uint32_t end);
```
Implementation walks `WorkingFile` line offsets backwards (fast path: ASCII line).

## 5. Dependencies (final)

| Dependency | Status |
|-----------|--------|
| LspCpp | **in tree** as a git submodule at `third_party/LspCpp`, pinned commit `19150d12` |
| `langservercpp` | **not used** — the original `CMakeLists.txt` referenced a missing package; LspCpp replaces it |
| simdjson | **dropped** — LspCpp's bundled RapidJSON handles all protocol JSON |
| Boost | not required (standalone Asio) |

`langservercpp` and simdjson must not be re-added for protocol work. If, later, non-protocol JSON parsing is ever needed (e.g. `workspace/didChangeConfiguration`), simdjson could be re-added then.

## 6. RPC handlers → LspCpp types

| Method (advertised) | LspCpp type | Behavior |
|---------------------|-------------|----------|
| initialize / shutdown / exit | `td_initialize` / `td_shutdown` / `Notify_Exit` | standard lifecycle; capabilities per §3 M1 |
| textDocument/didOpen | `Notify_TextDocumentDidOpen::notify` | `WorkingFiles.OnOpen`, parse, push diags |
| textDocument/didChange | `Notify_TextDocumentDidChange::notify` | `WorkingFiles.OnChange`, reparse, push diags |
| textDocument/didSave / didClose | `Notify_TextDocumentDidSave` / `...DidClose` | reparse on save; evict `WorkingFile` on close |
| textDocument/documentSymbol | `td_documentSymbol` | Symbol tree → `lsDocumentSymbol[]` |
| textDocument/foldingRange | `td_foldingRange` | Block ranges → `lsFoldingRange[]` |
| textDocument/hover | `td_hover` | occurrence at pos → `lsHover` (markdown) |
| textDocument/definition | `td_definition` | occurrence → origin selection range |
| textDocument/references | `td_references` | occurrences across index (same-doc first) |
| textDocument/documentHighlight | `td_documentHighlight` | same-doc occurrences |
| textDocument/completion | `td_completion` | keywords + `end <block>` snippets + symbols |
| textDocument/signatureHelp | `td_signatureHelp` | enclosing sub/function parameter list |
| textDocument/rename (M4) | `td_rename` | index-driven workspace edit |
| workspace/symbol (M4) | `ws_symbol` / workspace scan | symbol index over `.bas`/`.bi` |
| `$/cancelRequest` | built-in (CancelMonitor) | checked at loop boundaries of long scans; others fast enough to ignore |

## 7. Concurrency (plan change: was single-threaded)

LspCpp is multithreaded; our previous single-threaded-loop assumption is gone. New model:
- **Inbound ordering is guaranteed**: notifications are FIFO on a dedicated thread; requests execute only after prior notifications complete (LspCpp gate). So buffer state applied by `didChange` is always visible to request handlers.
- **Requests may run concurrently with each other** (handler pool, `max_workers=2` by default). Keep it at 2 — our CPU work is a per-document reparse, cheap and independent.
- **Shared state**: the language state (map of `WorkingFile` → parse cache, and M4 workspace index) is a snapshot-able structure guarded by a single `std::mutex`; each handler takes the lock only to fetch a parse snapshot, then does response construction lock-free. No locks on the LspCpp hot path (their docs explicitly forbid mutating registration maps at runtime).
- **Cancellation**: `CancelMonitor` threaded into M4 scans; per-edit reparses are sub-millisecond so cancellation is irrelevant there.

## 8. Cross-platform + UTF-16 position handling

- LspCpp hides stdin/stdout binary-mode and framing differences (Windows-safe `stream.h`); we add no own shell/exec/POSIX usage. `std::filesystem` only in M4.
- Position math:
  - All LSP positions are UTF-16 code units; `WorkingFile::RebuildLineOffsets` is called on open/change (or re-derive lazily against `GetContentNoLock()`).
  - Parser operates on byte offsets; `utf16.h` converts at the LSP boundary only.
  - FreeBASIC source is normally ASCII → fast paths, correctness first via `utf16_position`/`GetOffsetForPosition` round-trip (covered by `utf16_checks` tests).
- CRLF: LspCpp line-offset rebuild handles `\r`; our lexer treats `\r\n` and `\n` as EOL identically.

## 9. FreeBASIC lexical rules (encoded in lexer.cpp/language.cpp — unchanged contract)

- Case-insensitive identifiers; canonical key = lowercase name **including** type-suffix char.
- Suffix chars: `$` STRING, `%` SHORT, `&` LONG, `!` SINGLE, `#` DOUBLE, `@` LONG.
- Line continuation `_` (whitespace-tolerant) at EOL; statements separated by `:`.
- Comments: `'` to EOL and line-leading `REM`; `'` inside a string is not a comment.
- Doc comments: `///` and `''` lines directly above declarations feed `Symbol::doc`.
- Numbers: decimal, `&H`/`&O`/`&B` radix, floats `1.5`/`1e-5`, optional suffix.
- Strings: `"..."` with doubled `""` as escaped quote.
- Preprocessor lines start with `#` (`#include once`, `#define`, `#if..#endif`, `#print`).
- `.` member access and `->` are operators, never identifier parts.

## 10. Testing plan

- **Unit (ctest, no external framework)**:
  - `lexer_checks`: strings with `""`, `&H`/`&B` numbers, continuation `_`, `REM`/`'` comments, suffix identifiers, `:` separators.
  - `parser_checks`: block nesting, unterminated-block diagnostics, nested outline, doc-comment capture.
  - `utf16_checks`: byte↔UTF-16 round-trip on ASCII and multi-byte (`漢字`) buffers.
- **Integration**: drive `LanguageSession` with LspCpp's in-memory test streams (`FeedableIStream`, `StringOStream`, `MakeLspFrame` from their `tests/test_helpers.h`) instead of spawning a subprocess: script initialize → didOpen → documentSymbol → hover → definition → shutdown; assert response JSON. Portable, no Boost needed.
- **Ground truth**: compile ambiguous snippets with the installed `fbc` (10.2) and record agreement in `tests/corpus/`.

## 11. CMake changes

```cmake
cmake_minimum_required(VERSION 3.16)                 # LspCpp floor
project(freebasiclsp CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(third_party/LspCpp)                 # vendored, pinned commit 19150d12
set_target_properties(lspcpp PROPERTIES ... )        # if we must force Feature flags

add_executable(freebasiclsp
    src/main.cpp src/session.cpp src/lexer.cpp src/parser.cpp
    src/language.cpp src/index.cpp src/utf16.cpp)
target_link_libraries(freebasiclsp PRIVATE lspcpp)   # simdjson dropped
enable_testing()
```
LspCpp is configured with `-DLSPCPP_BUILD_WEBSOCKETS=OFF -DLSPCPP_BUILD_EXAMPLES=OFF -DLSPCPP_BUILD_TESTS=OFF -DLSPCPP_BUILD_MINIMAL_EXAMPLE=OFF` to minimize its compile surface and avoid the Boost-requiring example/test targets.

## 12. Milestone acceptance

1. **M1**: `cmake ..` + `cmake --build .` clean with vendored LspCpp; scripted session shows `initialize`→capabilities→`didOpen`→diagnostics with no protocol errors.
2. **M2**: lexer/parser pass corpus unit tests; diagnostics agree with `fbc` on unterminated/mismatched blocks where checkable.
3. **M3**: typed handlers return valid JSON for all advertised methods in the in-memory integration test; diagnostics push on every edit.
4. **M4** (stretch): workspace scan, cross-file references, rename.

## 13. Immediate next steps (when implementation starts)

1. `git submodule add <url> third_party/LspCpp`, then check out the pinned commit `19150d12c4ae26239d75258ed598ba8ea3587cb7`; commit `.gitmodules` + gitlink with it.
2. Rewrite `CMakeLists.txt` per §11 (drop the `langservercpp` `find_package`; link the `lspcpp` target).
3. Scaffold headers from this sketch with empty bodies; implement bottom-up: `symbols` → `lexer` → `parser` → `utf16` → `language` → `index` → `session` → `main`, compiling after each.
4. Commit after each milestone (M1–M4) against the §12 acceptance criteria; keep build/ artifacts out of the index (`.gitignore`).
