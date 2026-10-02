# FreeBASIC editor grammars

Generated from the `src/language.cpp` catalog by `tools/gen_grammar`. **Do not
edit these files by hand** — edit the catalog and regenerate:

```
cmake --build build --target grammar   # or: ./build/gen_grammar docs/grammar
```

`grammar_checks` (ctest) regenerates in memory and byte-diffs the result
against the committed files, so a stale grammar fails CI.

## Files

- `freebasic.tmLanguage.json` — TextMate grammar (`source.freebasic`), for
  TextMate-compatible editors.
- `freebasic.vim` — vim and Neovim syntax (`b:current_syntax = "freebasic"`).

`cmake --install` copies both to `<prefix>/share/freebasicd/grammar/`.

## Installing them

One page per editor, GUI-first where the editor has a GUI, lives in
[`../editors/`](../editors/README.md). The short version for vim, which
detects FreeBASIC itself and only needs the syntax file plus a filetype
override:

```
mkdir -p ~/.vim/syntax ~/.vim/ftdetect
cp freebasic.vim ~/.vim/syntax/freebasic.vim
printf 'au BufRead,BufNewFile *.bas,*.bi set filetype=freebasic\n' \
  > ~/.vim/ftdetect/freebasic.vim
```
