# Dependencies

Read this file before changing a submodule, a dependency pin, dependency
CMake, or the LspCpp fork.

## Build requirements

- CMake 3.16 or later.
- A C++17 compiler.
- Git submodules restored with `git submodule update --init`.

## LspCpp

LspCpp provides LSP and JSON-RPC support. It is a submodule at
`third_party/LspCpp`, consumed with `add_subdirectory(third_party/LspCpp)`
and linked as the `lspcpp` target.

This project uses its fork, [mudhairless/LspCpp][fork], rather than upstream.
The submodule is pinned to commit `e4b177e` on the fork branch
`lsp-3.17-completions`. That branch starts from upstream
[kuafuwang/LspCpp][upstream] commit `19150d12`, dated 2026-08-21. Upstream has
no release tag.

The fork carries changes this server needs:

- LSP 3.17 watched-file registration types.
- The `CodeAction` arm and writer in the `codeAction` result.
- Diagnostic report types.
- The wire shape of `SemanticTokensEdit`.
- `InitializeResult::serverInfo`.
- The `WorkingFiles::OnOpen` version fix.
- CMake and MSVC build fixes.

The individual commits and their rationale are on the fork branch and in
`CHANGELOG.md`. Do not replace the fork with upstream until those changes are
available there.

LspCpp handles protocol JSON through its bundled RapidJSON. No Boost is
required. `LSPCPP_STANDALONE_ASIO` is the default.

Build it with:

- `LSPCPP_BUILD_WEBSOCKETS=OFF`
- `LSPCPP_BUILD_EXAMPLES=OFF`
- `LSPCPP_BUILD_TESTS=OFF`
- `USE_ZLIB=OFF`

The WebSockets setting still causes ixwebsocket to be added, and that project
can default to compression that a stdio server cannot use. The root CMake
configuration controls the values above.

### Working on the fork

Inside `third_party/LspCpp`:

- `origin` is `mudhairless/LspCpp`. Fetch uses HTTPS and push uses SSH.
- `upstream` is `kuafuwang/LspCpp`. Its push URL is disabled.
- The checked-out branch is `lsp-3.17-completions`.

To take in upstream changes:

```sh
git fetch upstream
git switch lsp-3.17-completions
git rebase upstream/master
git push --force-with-lease origin lsp-3.17-completions
```

Then update the submodule pin in this repository. The `.gitmodules` URL does
not change, so contributors can still restore the pin with
`git submodule update --init`.

Do not edit the checked-out submodule as a shortcut for a dependency change.
Commit the change on the fork branch, then bump the pin here.

### Windows CMake caution

Windows builds use the Visual Studio generator. Only that generator reaches
the Boost NuGet branch in LspCpp's CMake, and a non-Windows machine cannot
configure that path. A change to LspCpp's CMake must therefore be read
carefully for the Windows branch even when it builds locally.

## tomlplusplus

[tomlplusplus][tomlplusplus] is a submodule at `third_party/tomlplusplus`,
pinned to commit `30172438`, version 3.4.0. It is header-only and consumed as
the CMake interface target `tomlplusplus::tomlplusplus`.

It parses `freebasicd.toml` in `src/settings.h` and `src/settings.cpp`.

## argparse

[argparse][argparse] is a submodule at `third_party/argparse`, pinned to
commit `3eda91b`, version 3.2. It is header-only and consumed as the CMake
interface target `argparse::argparse`.

It parses the `fbformat` command line in `src/format/fbformat_main.cpp`.
`freebasicd` takes no command-line arguments today. If it gains any, use
argparse rather than adding a hand-written parser.

[fork]: https://github.com/mudhairless/LspCpp
[upstream]: https://github.com/kuafuwang/LspCpp
[tomlplusplus]: https://github.com/marzer/tomlplusplus
[argparse]: https://github.com/p-ranav/argparse
