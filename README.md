# freebasicd

[![CI](https://github.com/mudhairless/freebasicd/actions/workflows/ci.yml/badge.svg)](https://github.com/mudhairless/freebasicd/actions/workflows/ci.yml)

A FreeBASIC Language Server.

A [Language Server
Protocol](https://microsoft.github.io/language-server-protocol/)
implementation for [FreeBASIC](https://www.freebasic.net/), written in C++17 and
speaking LSP 3.17 over stdio. It gives FreeBASIC code the editor support you
would expect from a modern toolchain: cross-file navigation, completion,
hover, rename, folding, semantic highlighting, quick fixes, and a workspace
symbol index. No plugin is required in your editor, only an LSP client.

## Status: early, and a testbed

This project is **not released**. The source is public; the release is not.
There are no published binaries, no tagged releases, and no packaged
installers, and the `0.7.0` in `CMakeLists.txt` is a version the build stamps
on itself rather than a promise that a download exists. The project follows
[semantic versioning](https://semver.org/), and the number moves only when a
release ships.

Development is also a deliberate experiment. This server is being built in
public as a testbed for agentic coding: an AI coding agent works through the
roadmap in [`PLAN.md`](PLAN.md) milestone by milestone, and
[`CHANGELOG.md`](CHANGELOG.md) plus the commit history are the record. Expect
churn, expect occasional rewrites of yesterday's structure, and treat the API as
unstable until a version tag exists.

What that means in practice:

- The feature list below is what the code does today, not a roadmap.
- Nothing is stable or supported. Expect breaking changes without notice.
- The CI matrix builds and tests Linux (GCC and Clang), macOS (AppleClang), and
  Windows (MSVC) on every push, and a local green run is not a substitute for
  the other two — those legs exist only in CI.

## What it does

Implemented and advertised in the `initialize` reply:

| Feature | Notes |
|---|---|
| Diagnostics | Parse errors (block-structure and preprocessor ones carry fbc's own error numbers), unresolved `#include`; pushed by default, or pulled on request (see below) |
| Hover | Types and signatures; resolves member access through the base variable's declared type, including cross-file and `with`-implicit chains; `#define`/`#macro` definitions on their usages |
| Go to definition | Same-file and cross-file, gated on FreeBASIC's `Shared` visibility rules; a cursor on an `#include` filename literal opens its header |
| Find references | Workspace-wide, following includes |
| Document highlight | Read/write occurrences of the symbol under the cursor |
| Rename | `prepareRename` plus a workspace `rename` that respects scope |
| Completion | Keywords, intrinsics, locals, preprocessor defines/macros, and context-aware UDT and enum members after `.` or `->`; a declared symbol's doc comment is attached on demand by `completionItem/resolve` |
| Signature help | For calls and subs, with parameter labels |
| Document symbols | Symbols, subs, functions, types, enums, defines, and macros per file |
| Folding ranges | Blocks and multi-line constructs |
| Semantic tokens | Full, delta, and range requests; drives client-side highlighting |
| Inlay hints | Inferred types and block closers |
| Code actions | Quick fixes for a missing `#include` target, an unclosed block, a reserved word used as a member name, and an ignored keyword suffix |
| Selection ranges | Token → statement → enclosing blocks → file |
| Call hierarchy | Outgoing calls and incoming call sites, through the include closure |
| Type definition | The type a variable, a field, or a `Type.name` member belongs to |
| Implementation | The `declare sub` / module-level `sub Type.name` edge, across files |
| Type hierarchy | Supertypes and subtypes of a `type` / `union`, through `Extends` |
| Code lens | A reference count per declaration, whose click lists the sites |
| Document links | Each `#include` literal that resolves to a file links to that header (the same navigation `definition` gives on the literal, for clients that draw no link) |
| Formatting | Full-document `textDocument/formatting`; `textDocument/rangeFormatting` is advertised (defers to a full-document edit for now) |
| Pull diagnostics | `textDocument/diagnostic` and `workspace/diagnostic`, negotiated |
| Workspace symbols | Aggregated from every workspace root, one index per root |
| Workspace folders | Multi-root sessions, with watched files and per-root config |

The formatter (full-document `textDocument/formatting`, plus the `fbformat`
CLI) is documented separately in [`Formatting.md`](Formatting.md): what it
rewrites and preserves, the configuration options, and CLI usage.

Not implemented, and therefore not advertised: `typeHierarchy/resolve`
(prepare fills both lists instead).

The server advertises `textDocumentSync.willSave` and accepts
`textDocument/willSave`, but acts on neither: diagnostics are already reissued
by `didSave`, and the hook is reserved for format-on-save. The formatter
respects the nearest `.fbformat` (overriding `freebasicd.toml` when present),
with FBdocs as the default preset.

### Push or pull

Diagnostics are pushed with `textDocument/publishDiagnostics` unless the client
advertises the LSP 3.17 pull capability `textDocument.diagnostic`, in which case
the server advertises `diagnosticProvider` and switches to answering
`textDocument/diagnostic` and `workspace/diagnostic` for the rest of the
session — a client that supports both is never sent the same diagnostic twice.
A pull answer is `full` or `unchanged` (the latter when the client names the
`resultId` it already holds and nothing changed), and a report about one
document can carry the problems of the files it includes in
`relatedDocuments`. The server sends `workspace/diagnostic/refresh` when a
change invalidates a report the client has already pulled.

## Requirements

- CMake 3.16 or newer and a C++17 compiler (verified with GCC and Clang)
- Git, for the vendored submodules
- GNU gettext tools (`msgfmt`) and a gettext runtime library, for the message
  catalogs. The build skips catalog compilation with a status message when
  gettext is missing; the server still runs, untranslated.
- `fbc` 1.10.2 is handy but not required. The language facts in
  [`FreeBASIC.md`](FreeBASIC.md) were verified against it, as were the 51 `.bas`
  files in the test corpus.

## Building

```sh
git clone --recursive https://github.com/mudhairless/freebasicd
cd freebasicd
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

If you already cloned without `--recursive`:

```sh
git submodule update --init --recursive
```

The build produces `build/freebasicd`. It takes no command line flags and
speaks LSP over stdin and stdout, so running it directly in a terminal looks
like it hangs. That is correct behavior.

To lay down a runnable prefix instead:

```sh
cmake --install build --prefix /usr/local
```

That installs the binary to `<prefix>/bin`, the message catalogs to
`<prefix>/share/locale`, the editor grammars to
`<prefix>/share/freebasicd/grammar`, and `LICENSE.md` to
`<prefix>/share/doc/freebasicd`.

One caveat: the catalog directory is compiled in as an absolute path at
configure time, so `--prefix` at install time has to match the
`CMAKE_INSTALL_PREFIX` the build was configured with. Install anywhere else and
point the server at its catalogs with `FBLANG_LOCALEDIR=<prefix>/share/locale`.
Make the install tree relocatable is an open item in [`PLAN.md`](PLAN.md) §4.

The server has no packaging config (CPack, AppImage, MSI) yet.

## Running in an editor

Any LSP client that can launch a binary over stdio will work; no first-party
editor extension is needed or shipped. The shape of the config, using a generic
key-based LSP client:

```json
{
  "command": "/absolute/path/to/build/freebasicd",
  "filetypes": ["freebasic", "bas", "bi"],
  "rootMarkers": [".git", "freebasicd.toml"]
}
```

Ready-made steps for the editors that work with no extension are in
[`docs/editors/`](docs/editors/README.md) — Kate, Neovim, Emacs, Helix, Vim,
and micro — and installation is covered in
[`docs/install.md`](docs/install.md).

The editor grammars (a TextMate grammar, a vim/Neovim syntax file, and a micro
syntax file) are in [`docs/grammar/`](docs/grammar/README.md). They are
generated from the keyword catalog in `src/language.cpp`, so they cannot drift
from the server.

## Configuration

Drop a `freebasicd.toml` at a workspace root. The file's presence also marks
that directory as a workspace root, alongside a VCS marker or a
source/include layout.

```toml
includePaths = ["inc", "vendor/fbinc"]   # like fbc's -i, relative to this file
diagnosticsOn = true
semanticTokensOn = true
inlayHintsOn = true
codeLensOn = true
warningLevel = "none"                    # fbc's -w names; see below
suppress = ["bad-continuation"]          # drop diagnostics by routing key
```

Each key has a default, unknown keys are ignored, and a malformed file keeps
the defaults and logs an error rather than degrading the session. The file is
re-read on `workspace/didChangeConfiguration`; the notification payload is
ignored, the file is the truth.

`warningLevel` gates which fbc-numbered warnings the server reports. It takes
fbc's own `-w` option names — `all`, `none`, `param`, `escape`, `pedantic`,
`next`, `funcptr`, `constness`, `suffix`, `upcast` — and an absent, unknown, or
non-string value means `-w none`. The default is `none`, so warnings stay
silent until a root opts in; each named level otherwise keeps fbc's exact
threshold (`all`/`constness`/`funcptr` show every warning, the rest show the
level-1 set). fbc's `-w error` is not accepted: a mapped warning stays a
warning. These strings are English only and are never translated.

`suppress` is a list of diagnostic identifiers the server drops from its
output entirely. The identifier is the diagnostic's own routing key — the
stable name quick fixes use, such as `bad-continuation`, `unterminated-block`,
or `include-not-found`. That key is also the leading half of the `code` the
server reports: `<key>, fbc error: <n>` (`include-not-found, fbc error: 23`)
when the parser mapped the diagnostic onto an fbc message, or the key alone
(`bad-continuation`) when fbc has no number for it. The list is matched
case-insensitively and order does not matter; an entry copied from the wire
code in full (`include-not-found, fbc error: 23`) is reduced to its key, with a
warning that asks you to clean the file up. It is a root-wide setting; a
per-line source comment (`' ignore: <key>`) and a config allow-list
counterpart are planned.

## How it is put together

```
src/lexer.cpp        tokenizer over the FreeBASIC surface, byte offsets
src/parser.cpp       declarations, block matching, dialect detection
src/language.cpp     keyword catalog, block closers, 255 intrinsics
src/resolve.cpp      same-file and cross-file name and member resolution
src/index.cpp        in-memory workspace index, include resolution
src/session.cpp      LSP handlers, capabilities, diagnostics
src/code_actions.cpp quick fixes as pure functions of a diagnostic
src/selection.cpp    expand-selection chains
src/call_hierarchy.cpp  call hierarchy over a scanned body
src/code_lens.cpp    "N references" lens anchors and titles
src/settings.cpp     freebasicd.toml
src/i18n.cpp         gettext message catalogs
```

The language layer is LSP-agnostic and works in byte offsets. All protocol
interaction lives in `src/session.cpp`, which converts to and from UTF-16
positions at the boundary. Each workspace root owns one `WorkspaceIndex`, held
entirely in memory: nothing is ever written to disk.

- [`FreeBASIC.md`](FreeBASIC.md) is the language reference and the source of
  truth for language facts. It records where the implementation and the real
  `fbc` still disagree.
- [`PLAN.md`](PLAN.md) holds the roadmap, the architecture notes, and the
  remaining gaps — what is still to do.
- [`CHANGELOG.md`](CHANGELOG.md) records what has shipped, when, and what it
  cost. Nothing has been released yet, so it is all under `## [Unreleased]`.
- [`AGENTS.md`](AGENTS.md) is the working agreement for the agent that drives
  this repo.
- [`TIDY.md`](TIDY.md) explains every `clang-tidy` suppression, with a reason.

## Tests

Comprehensive CTest suites, no external test framework:

```sh
ctest --test-dir build --output-on-failure
```

They cover the lexer, parser, resolver, index, settings, semantic tokens, inlay
hints, code actions, formatting, grammar freshness, i18n, and a comprehensive
`LanguageSession` integration suite that drives the server over in-memory
streams. The corpus in `tests/corpus/` holds 51 `.bas` files whose expected
diagnostics (the matching `.diag` files) are checked against the parser.

## Localization

Log and diagnostic messages go through gettext. The message template lives in
`po/freebasicd.pot`, and 29 committed `po/<lang>.po` catalogs ship with the
source. The server picks the catalog from the client's `locale` or the
environment. FreeBASIC keywords and the string "FreeBASIC" are never
translated, and a test enforces it.

```sh
cmake --build build --target po-template   # re-extract the template
cmake --build build --target update-po     # merge it into the po files
```

## Dependencies

- [LspCpp](https://github.com/kuafuwang/LspCpp), vendored as a submodule from
  our fork [mudhairless/LspCpp](https://github.com/mudhairless/LspCpp), which
  tracks upstream plus eleven local commits (watched-files registration types,
  `SemanticTokensEdit` serialization, the `codeAction` and diagnostic-report
  result types, and `InitializeResult::serverInfo`).
- [tomlplusplus](https://github.com/marzer/tomlplusplus) v3.4.0, vendored as a
  submodule, for the config file.
- [argparse](https://github.com/p-ranav/argparse) v3.2, vendored as a submodule,
  for the `fbformat` CLI.
- RapidJSON, via LspCpp, for JSON.
- GNU gettext, from the system, for localization.

## License

GPL-3.0-or-later. See [`LICENSE.md`](LICENSE.md).
