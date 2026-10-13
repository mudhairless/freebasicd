# micro

micro is a terminal editor with syntax highlighting built in and **no bundled
LSP client** — its plugins are `autoclose`, `comment`, `diff`, `ftoptions`,
`linter`, `literate`, and `status`, and none of them speaks the protocol. So
this page covers the same half as the [Vim](vim.md) page: syntax highlighting.

micro ships syntax files for over 100 languages, but not FreeBASIC. Without
one, a `.bas` or `.bi` file gets micro's fallback highlighting, which knows
only about email addresses and URLs. The file this project generates fills
that gap.

## Install the syntax file

micro reads syntax files from the `syntax/` directory inside its config
directory, which is `~/.config/micro` on every platform unless `XDG_CONFIG_HOME`
or `MICRO_CONFIG_HOME` says otherwise:

```sh
mkdir -p ~/.config/micro/syntax
cp /usr/local/share/freebasicd/grammar/freebasic.yaml ~/.config/micro/syntax/freebasic.yaml
```

On Windows the same directory is `%USERPROFILE%\.config\micro\syntax`. From a
source checkout instead of an install, the file is
[docs/grammar/freebasic.yaml](../grammar/freebasic.yaml) — generated from the
server's keyword catalog, so it knows every FreeBASIC keyword and type this
build of `freebasicd` knows.

That is the whole setup. Unlike Vim and Neovim, micro needs no filetype
override, because how to recognize a FreeBASIC file — the `\.(bas|bi)$` filename
pattern — travels in the syntax file itself.

micro reads the syntax directory once at startup, so **restart it** after
copying the file. Open a `.bas` file; its comments, strings, numbers, and
keywords should be colored. If nothing changed, check that the file landed at
`~/.config/micro/syntax/freebasic.yaml`, and that no other installed syntax
file claims the same extensions first.

## What you get

Comments (`'`, `REM`, and the `/'...'/` block comment, with `TODO`/`FIXME`/`XXX`
inside a comment called out), strings with the doubled-quote escape, numbers
with the `%`, `&`, `!`, `#` suffixes, `#` preprocessor lines, keywords, and
operators. Colors come from your colorscheme, through micro's standard groups:
`comment`, `constant.string`, `constant.number`, `preproc`, `statement`,
`symbol.operator`, `identifier`, and `todo`.

One limit worth knowing: micro highlights a block comment only as deep as its
own region rule, so nested `/' ... /'` pairs inside a block comment are not
tracked as nesting.

## If you want the language server anyway

micro ships no LSP client, so driving `freebasicd` from it means installing a
third-party plugin that brings its own configuration. The server is the same
over stdio as it is for every other client — the generic snippet in the
[README](../../README.md#running-in-an-editor) is the shape such a plugin would
use.

For the full language server today, [Neovim](neovim.md) is the terminal-editor
answer, and it can read this same project's highlighting work only through its
own `freebasic.vim`.
