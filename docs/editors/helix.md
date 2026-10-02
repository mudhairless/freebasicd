# Helix

Helix has a built-in LSP client, so the whole setup is two tables in
`~/.config/helix/languages.toml`.

One honest warning first: **Helix has no FreeBASIC tree-sitter grammar**, and
Helix gets all of its syntax highlighting from tree-sitter. Nothing this project
ships changes that: the grammars in [`../grammar/`](../grammar/README.md) are a
Vim syntax script and a TextMate grammar, and Helix can read neither. Every LSP
feature works; the buffer stays uncolored. If coloring matters to you, use
[Vim](vim.md), [Neovim](neovim.md), [Emacs](emacs.md), or [Kate](kate.md).

## 1. Register the language

Create `~/.config/helix/languages.toml` if it does not exist and add:

```toml
[[language]]
name = "freebasic"
scope = "source.freebasic"
file-types = ["bas", "bi"]
comment-tokens = ["'"]
roots = [".git", "freebasicd.toml"]
language-servers = ["freebasicd"]

[language-server.freebasicd]
command = "/usr/local/bin/freebasicd"
```

What each line is for:

- `file-types` is how Helix recognizes the buffer. It matches the extension
  without the dot.
- `roots` is how Helix picks the directory it sends as the workspace root. It
  walks up from the file and takes the **topmost** directory containing a
  marker, so listing `freebasicd.toml` next to `.git` means the server's own
  settings file also marks the project root, in a directory with no VCS.
- `comment-tokens` gives Helix's `toggle_comments` something to work with, so
  `:` then `toggle_comments` comments a line the FreeBASIC way.
- `command` may be a bare name, which Helix looks up in `PATH`, or an absolute
  path, which does not depend on Helix's environment.

There is deliberately no `config` table: that is where Helix would put
initialization options, and `freebasicd` takes none.

Restart Helix — or reopen the file — and the language applies.

## 2. Turn on inlay hints

Off by default, in `~/.config/helix/config.toml`:

```toml
[editor]
lsp.display-inlay-hints = true
```

## What you get

| Feature | Key |
|---|---|
| Diagnostics | shown in the status line; `<space>d` lists them |
| Completion | `Ctrl-x`, then `<C-n>` / `<C-p>` |
| Hover | `K` |
| Go to definition | `gd` |
| Go to declaration | `gD` |
| Find references | `gr` |
| Rename | `R` |
| Quick fixes | `<space>a` |
| Signature help | automatic while completing |
| Document symbols | `<space>s` |
| Workspace symbols | `<space>S` |
| Inlay hints | the config above |

Helix does not implement **semantic tokens**, **code lens**, **selection
ranges**, or **call/type hierarchy**, so those never appear. The server sends
semantic tokens; there is nothing on this side to render them.

## Verify

Open a `.bas` file. The status line names the language, and if the server
attached, pressing `K` on a symbol eventually produces a hover popup — the
answer comes from the server, so the first successful reply is the proof that it
started. `:lsp-restart` restarts it, `:lsp-stop` stops it, and
`:lsp-workspace-command` lists the server's workspace commands.

If nothing responds, the usual cause is a root that Helix could not find.
`:log-open` shows the LSP status lines, including which command Helix ran and
what came back on it.

## Project settings

Do not add a `config` table for initialization options. `freebasicd` reads
`freebasicd.toml` from the project root and ignores client configuration — see
[Configuration](../../README.md#configuration).