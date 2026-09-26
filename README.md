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
roadmap in [`PLAN.md`](PLAN.md) milestone by milestone, and the commit history
is the record. Expect churn, expect occasional rewrites of yesterday's
structure, and treat the API as unstable until a version tag exists.

What that means in practice:

- The feature list below is what the code does today, not a roadmap.
- Nothing is stable or supported. Expect breaking changes without notice.
- The build works on Linux with GCC or Clang. Other platforms are aspirational;
  CI is scaffolded but has never run against the hosted runners.

## What it does

Implemented and advertised in the `initialize` reply:

| Feature | Notes |
|---|---|
| Diagnostics | Parse errors, unterminated blocks, unresolved `#include` |
| Hover | Types and signatures; resolves member access through the base variable's declared type, including cross-file and `with`-implicit chains |
| Go to definition | Same-file and cross-file, gated on FreeBASIC's `Shared` visibility rules |
| Find references | Workspace-wide, following includes |
| Document highlight | Read/write occurrences of the symbol under the cursor |
| Rename | `prepareRename` plus a workspace `rename` that respects scope |
| Completion | Keywords, intrinsics, locals, and context-aware UDT and enum members after `.` or `->` |
| Signature help | For calls and subs, with parameter labels |
| Document symbols | Symbols, subs, functions, types, enums, and macros per file |
| Folding ranges | Blocks and multi-line constructs |
| Semantic tokens | Full, delta, and range requests; drives client-side highlighting |
| Inlay hints | Inferred types and block closers |
| Code actions | Quick fixes for a missing `#include` target and for an unclosed block |
| Workspace symbols | Aggregated from every workspace root, one index per root |
| Workspace folders | Multi-root sessions, with watched files and per-root config |

Not implemented, and therefore not advertised: `selectionRange`,
`callHierarchy`, `codeLens`, pull diagnostics, document links, and completion
item resolve.

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
`<prefix>/share/locale`, and `LICENSE.md` to `<prefix>/share/doc/freebasicd`.

One caveat: the catalog directory is compiled in as an absolute path at
configure time, so `--prefix` at install time has to match the
`CMAKE_INSTALL_PREFIX` the build was configured with. Install anywhere else and
point the server at its catalogs with `FBLANG_LOCALEDIR=<prefix>/share/locale`.
Make the install tree relocatable is an open item in [`PLAN.md`](PLAN.md) §4.

The server has no packaging config (CPack, AppImage, MSI) yet.

## Running in an editor

There is no first-party editor extension yet. Any LSP client that can launch a
binary over stdio will work. The shape of the config, using a generic
key-based LSP client:

```json
{
  "command": "/absolute/path/to/build/freebasicd",
  "filetypes": ["freebasic", "bas", "bi"],
  "rootMarkers": [".git", "freebasicd.toml"]
}
```

Editor grammars for FreeBASIC live in [`editors/`](editors/README.md): a
TextMate grammar for VS Code and friends, and a vim/Neovim syntax file. They
are generated from the keyword catalog in `src/language.cpp`, so they cannot
drift from the server.

## Configuration

Drop a `freebasicd.toml` at a workspace root. The file's presence also marks
that directory as a workspace root, alongside a VCS marker or a
source/include layout.

```toml
includePaths = ["inc", "vendor/fbinc"]   # like fbc's -i, relative to this file
diagnosticsOn = true
semanticTokensOn = true
inlayHintsOn = true
```

Each key has a default, unknown keys are ignored, and a malformed file keeps
the defaults and logs an error rather than degrading the session. The file is
re-read on `workspace/didChangeConfiguration`; the notification payload is
ignored, the file is the truth.

## How it is put together

```
src/lexer.cpp        tokenizer over the FreeBASIC surface, byte offsets
src/parser.cpp       declarations, block matching, dialect detection
src/language.cpp     keyword catalog, block closers, 247 intrinsics
src/resolve.cpp      same-file and cross-file name and member resolution
src/index.cpp        in-memory workspace index, include resolution
src/session.cpp      LSP handlers, capabilities, diagnostics
src/code_actions.cpp quick fixes as pure functions of a diagnostic
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
  remaining gaps.
- [`AGENTS.md`](AGENTS.md) is the working agreement for the agent that drives
  this repo.
- [`TIDY.md`](TIDY.md) explains every `clang-tidy` suppression, with a reason.

## Tests

Fifteen CTest suites, no external test framework:

```sh
ctest --test-dir build --output-on-failure
```

They cover the lexer, parser, resolver, index, settings, semantic tokens, inlay
hints, code actions, grammar freshness, i18n, and a 4,800-line `LanguageSession`
integration suite that drives the server over in-memory streams. The corpus in
`tests/corpus/` holds 51 `.bas` files whose expected diagnostics (the matching
`.diag` files) are checked against the parser.

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
  tracks upstream plus five local commits (watched-files registration types,
  `SemanticTokensEdit` serialization, and the `codeAction` result type).
- [tomlplusplus](https://github.com/marzer/tomlplusplus) v3.4.0, vendored as a
  submodule, for the config file.
- RapidJSON, via LspCpp, for JSON.
- GNU gettext, from the system, for localization.

## License

GPL-3.0-or-later. See [`LICENSE.md`](LICENSE.md).
