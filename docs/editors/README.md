# Editor setup

`freebasicd` speaks LSP 3.17 over stdio, so an editor that ships an LSP client
can drive it with no extension to install. Every page below is written for that
case: the editors here either have a GUI that configures a server, or need a
few lines pasted into a config file. Where an editor has a settings GUI, the
page leads with the GUI.

Do [install the server](../install.md) first. There is nothing to configure in the
client beyond the path to the binary — project settings live in a
`freebasicd.toml` at the project root, which the server reads from disk (see
[Configuration](../../README.md#configuration)).

## Pick your editor

| Editor | LSP client | How it is configured | Syntax file to install | Page |
|---|---|---|---|---|
| Kate | built in, as the **LSP Client** plugin | GUI (a JSON tab in the plugin's settings page) | none — Kate ships a FreeBASIC syntax | [kate.md](kate.md) |
| Neovim | built in, `vim.lsp` | one Lua file in your config directory | [`freebasic.vim`](../grammar/freebasic.vim) | [neovim.md](neovim.md) |
| Emacs | built in, `eglot` (Emacs 29 and newer) | a few lines in `init.el` | none — buffers open as plain text until you add a mode | [emacs.md](emacs.md) |
| Helix | built in | `~/.config/helix/languages.toml` | none — Helix has no FreeBASIC tree-sitter grammar, so highlighting stays plain | [helix.md](helix.md) |
| Vim | none — Vim ships no LSP client | — | [`freebasic.vim`](../grammar/freebasic.vim) | [vim.md](vim.md) |

The two grammar files are generated from the server's own keyword catalog, so
they cannot drift from it: see [`../grammar/`](../grammar/README.md). They are
also installed with the server, at
`<prefix>/share/freebasicd/grammar/`, so an installed server hands you the file
without a source checkout.

## What each client actually shows you

`freebasicd` implements every row; what differs is whether the editor has a way
to display it. "Opt-in" means the client has the feature switched off by
default and the page says how to turn it on.

| Feature | Kate | Neovim | Emacs | Helix |
|---|---|---|---|---|
| Diagnostics | yes | yes | yes | yes |
| Completion | yes | yes | yes | yes |
| Hover | yes | yes | yes | yes |
| Go to definition | yes | yes | yes | yes |
| Find references | yes, as a list | yes | yes | yes, `gr` |
| Rename | yes | yes | yes | yes |
| Document symbols | yes | yes | yes | yes |
| Workspace symbols | yes | yes | yes | yes |
| Quick fixes | yes | yes | yes | yes |
| Signature help | yes | yes | yes | yes |
| Semantic tokens | opt-in | yes | Emacs master only | no, not implemented by Helix |
| Inlay hints | opt-in | opt-in | opt-in | opt-in |
| Code lens | no, not implemented by Kate | yes | no, not implemented by Eglot | no, not implemented by Helix |
| Selection ranges | yes, **Expand Selection** in the LSP Client menu | 0.12 and newer | no | no |
| Call hierarchy | no | yes | Emacs master only | no |
| Type hierarchy | no | yes | Emacs master only | no |
| Type definition, implementation | no | yes | Emacs master only, type definition | no |

Where a cell says no, the client has no way to display the answer — the server
still answers the request, and the integration suite asserts it. A "no" here is
a limit of the editor, not of `freebasicd`; Neovim is the one client on this list
that can show nearly all of it, which is why it is worth the extra setup.

## Not listed here, and why

- **VS Code, Zed, Sublime Text, Notepad++, Geany, Brackets.** Each of these
  needs a per-editor extension or plugin before it will talk to any language
  server, and writing one is separate work from this server. When one ships,
  add a page here.
- **Emacs `lsp-mode`.** A stronger client than `eglot`, but it is a package
  from MELPA rather than part of Emacs, so it does not belong on a page about
  extensions you do not have to install.