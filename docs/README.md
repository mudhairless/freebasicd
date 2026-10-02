# Documentation

- **[Installing the server](install.md)** — build it, install it to a prefix,
  get it on your `PATH`, and check that it runs. Start here.
- **[Editor setup](editors/README.md)** — one page per editor, GUI-first where
  the editor has a settings dialog: Kate, Neovim, Emacs, Helix, Vim. The index
  also lists which LSP features each of those clients can actually display.
- **[Editor grammars](grammar/README.md)** — the generated TextMate and
  vim/Neovim syntax files. Do not edit them by hand; they come from the server's
  keyword catalog.

For the language itself — what FreeBASIC's grammar, types, and dialect rules
are, and where the server's lexer diverges from `fbc` — see
[`FreeBASIC.md`](../FreeBASIC.md). That is the reference for *what the language
is*; these pages are about *running the server*.

For development — module layout, tests, the milestone plan — see
[`CONTRIBUTING.md`](../CONTRIBUTING.md) and [`PLAN.md`](../PLAN.md).