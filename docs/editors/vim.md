# Vim

Vim has no LSP client. There is no `vim.lsp` in Vim — that is Neovim's, and
the two are separate programs since Neovim forked (see `:h vim-lsp` in Neovim,
which does not exist here). Nothing in Vim will talk to `freebasicd` on its
own, and this page does not document a plugin for it, because a plugin is
exactly what this directory avoids.

So this page is the half of Vim that is worth having: syntax highlighting.
Vim ships a FreeBASIC syntax definition, so installing ours means replacing it.

For the language server, use [Neovim](neovim.md) — same syntax file, same
config style, and `vim.lsp` built in. On an existing Vim install, Vim 9 already
detects FreeBASIC files correctly (see step 2), so the transition is only about
which binary you launch.

## 1. Install the syntax file

```sh
mkdir -p ~/.vim/syntax
cp /usr/local/share/freebasicd/grammar/freebasic.vim ~/.vim/syntax/freebasic.vim
```

From a source checkout, the same file is at
[docs/grammar/freebasic.vim](../grammar/freebasic.vim). It is generated from
the server's keyword catalog, so it knows every FreeBASIC keyword and type this
build of `freebasicd` knows.

Verify with `:set syntax?`, which prints `freebasic`, and `:syntax list` for the
list of groups it defines.

## 2. Make the filetype stick

Vim's own detection handles most FreeBASIC files already: its `*.bas` and `*.bi`
rule reads the first 100 lines and sets `filetype=freebasic` when it sees
FreeBASIC-specific keywords such as `extern`, `constructor`, `namespace`, or
`property`, a `#` preprocessor line, `option static`, or a `/'` block comment. A
short file that matches none of those — `PRINT "hi"` on its own — falls back to
`filetype=basic`, and then no FreeBASIC highlighting is loaded.

One line fixes that. Create `~/.vim/ftdetect/freebasic.vim`:

```vim
au BufRead,BufNewFile *.bas,*.bi setf freebasic
```

Vim sources `ftdetect` files **after** its own detection, so `setf` (rather than
`setfiletype`) overrides the guess. Restart Vim afterwards, as documented in
`:help ftdetect`.

Neovim behaves the opposite way and needs
[`vim.filetype.add`](neovim.md#1-filetype-and-syntax-file) instead; the same
trick does not transfer between the two.

Then `:set filetype?` in a FreeBASIC file must print `freebasic`. If it does
not, the syntax file is installed but unreachable — check `:set runtimepath?`.

## What you get

Highlighting only: comments (`'`, `REM`, and the `/'...'/` block comment),
strings with the doubled-quote escape, numbers with the `%`, `&`, `!`, `#`
suffixes, preprocessor and `$`-metacommand lines, keywords, and operators.

Colors come from your colorscheme, since the syntax file only links groups to
standard ones (`Comment`, `String`, `Number`, `PreProc`, `Keyword`,
`Operator`, `Identifier`, `Todo`).

## If you want the language server anyway

Any Vim LSP plugin needs its own maintenance and its own configuration, which is
why this directory has no page for one. The two usual choices are `coc.nvim`
(a full LSP client with its own `freebasicd` setup) and `vim-lsp` (older, and
its FreeBASIC filetype handling is thinner). Whichever you pick, it will speak
the protocol this server implements — nothing here changes.

[Neovim](neovim.md) remains the answer that needs no plugin at all.