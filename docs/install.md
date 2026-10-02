# Installing freebasicd

`freebasicd` is the FreeBASIC language server. It is a single executable that
speaks LSP 3.17 over stdin and stdout, so installing it means putting one
binary where your editor can start it.

- [What you need](#what-you-need)
- [Build from source](#build-from-source)
- [Check that it runs](#check-that-it-runs)
- [Install to a prefix](#install-to-a-prefix)
- [Make the binary findable](#make-the-binary-findable)
- [What lands in the prefix](#what-lands-in-the-prefix)
- [Translations](#translations)
- [Configure a project](#configure-a-project)
- [Next: your editor](#next-your-editor)
- [Uninstall](#uninstall)

## What you need

- CMake 3.16 or newer, and a C++17 compiler (GCC, Clang, or MSVC)
- Git, for the two vendored dependencies (`LspCpp` and `tomlplusplus`, as
  submodules under `third_party/`)
- GNU gettext tools, for the message catalogs. Without them the build says so
  and carries on; the server then runs with English messages only.

Linux, macOS, and Windows are all supported — CI builds and tests all three.

## Build from source

```sh
git clone --recursive https://github.com/mudhairless/freebasicd
cd freebasicd
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Cloned without `--recursive`? Restore the submodules first:

```sh
git submodule update --init --recursive
```

On Windows with Visual Studio, use the same commands from a Developer Power
Shell; the generator defaults to Visual Studio and produces
`build\Debug\freebasicd.exe` (or `Release` — pass
`-DCMAKE_BUILD_TYPE=Release` only for single-config generators, since
multi-config generators take it per build).

The build produces `build/freebasicd`. You can point an editor straight at it
and skip installing, which is the right choice while you are trying the server
out or working on it. The next step is for when you want it to stay.

## Check that it runs

```sh
./build/freebasicd
```

It prints one line to stderr and then waits:

```
[freebasicd] freebasicd 0.7.0 starting
```

**That is correct behavior.** It is a language server: it takes no command line
arguments and does nothing until an editor sends it an `initialize` request
over stdin. Ctrl-C to exit.

There is no `--version` or `--help` flag, and adding one would mean parsing
argv before the JSON-RPC stream starts — the version is in that startup line and
in the editor's server-info panel instead.

The real test is an editor: if a `.bas` file produces diagnostics, completion,
or hover, the server is running.

## Install to a prefix

Choose the prefix at configure time, so the compiled-in paths match where the
files actually land:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=~/.local
cmake --build build --parallel
cmake --install build
```

`~/.local` keeps the install inside your account and needs no `sudo`. If you
want a system-wide location, configure with
`-DCMAKE_INSTALL_PREFIX=/usr/local` and run the install with `sudo`.

On Windows, pick a prefix you can write to without elevation, for example:

```powershell
cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$env:LOCALAPPDATA\Programs\freebasicd"
cmake --build build --config Release --parallel
cmake --install build --config Release
```

## Make the binary findable

Editors usually start a server by name and let the system resolve it, so
`<prefix>/bin` has to be on `PATH`. Add it once:

| Shell | Line to add to `~/.profile`, `~/.bashrc`, `~/.zshrc`, … |
|---|---|
| POSIX | `export PATH="$HOME/.local/bin:$PATH"` |
| fish | `fish_add_path $HOME/.local/bin` |
| PowerShell (user) | `[Environment]::SetEnvironmentVariable('Path', $env:Path + ";$env:LOCALAPPDATA\Programs\freebasicd\bin", 'User')` |

Open a new terminal afterwards — an editor started before the change will not
see it.

If you would rather not touch `PATH`, give the editor the absolute path to the
binary instead. Every editor page in [editors/](editors/README.md) shows that
form, and it is what the examples use.

## What lands in the prefix

| Path | What |
|---|---|
| `bin/freebasicd` (`freebasicd.exe` on Windows) | the server |
| `share/freebasicd/grammar/freebasic.vim` | vim and Neovim syntax file |
| `share/freebasicd/grammar/freebasic.tmLanguage.json` | TextMate grammar |
| `share/locale/<lang>/LC_MESSAGES/freebasicd.mo` | one compiled catalog per translated language |
| `share/doc/freebasicd/LICENSE.md` | the GPL text, which has to travel with the binary |

The two grammar files are generated from the server's own keyword catalog, so
they match whatever this build knows. Copy one into your editor's syntax
directory; the [Vim](editors/vim.md) and [Neovim](editors/neovim.md) pages show
the exact commands. [Kate](editors/kate.md), [Emacs](editors/emacs.md), and
[Helix](editors/helix.md) need none of them.

## Translations

Diagnostics, hover text, and other messages come from gettext catalogs. The
server looks for them in this order: the `FBLANG_LOCALEDIR` environment
variable, then the build tree, then `<prefix>/share/locale` as compiled in at
configure time. Your locale comes from the environment.

One caveat, if you install somewhere other than the configured prefix — say you
configured with the default and then installed to `~/.local`:

```sh
export FBLANG_LOCALEDIR=~/.local/share/locale
```

Without it, messages fall back to English. Setting `CMAKE_INSTALL_PREFIX` at
configure time, as above, avoids the problem entirely. Making the install tree
fully relocatable is an open item in [`PLAN.md`](../PLAN.md).

## Configure a project

The server takes no client settings: it reads a `freebasicd.toml` from the
workspace root, which it finds the same way your editor finds the project root.
Drop one in to change the defaults:

```toml
includePaths = ["inc", "vendor/fbinc"]   # like fbc's -i, relative to this file
diagnosticsOn = true
semanticTokensOn = true
inlayHintsOn = true
codeLensOn = true
```

Every key is optional. See [Configuration](../README.md#configuration) for what
each one does.

## Next: your editor

- [Kate](editors/kate.md) — GUI setup, no files to copy
- [Neovim](editors/neovim.md) — one Lua file, the most features
- [Emacs](editors/emacs.md) — a few lines of `init.el`
- [Helix](editors/helix.md) — two tables in `languages.toml`
- [Vim](editors/vim.md) — highlighting only; Vim has no LSP client

[editors/README.md](editors/README.md) is the index, including which LSP features
each of those clients can display.

## Uninstall

Delete the prefix. Nothing is written outside it — the index is in memory, there
is no cache, no database, and no state file. The only file the server ever reads
of your own is the `freebasicd.toml` you wrote.