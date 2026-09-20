# FreeBASIC editor grammars

Generated from the `src/language.cpp` catalog by `tools/gen_grammar`. **Do not
edit these files by hand** — edit the catalog and regenerate:

```
cmake --build build --target grammar     # or: ./build/gen_grammar editors
```

`grammar_checks` (ctest) regenerates in memory and byte-diffs the result
against the committed files, so a stale grammar fails CI.

## Files

- `freebasic.tmLanguage.json` — TextMate grammar (`source.freebasic`).
- `freebasic.vim` — vim/Neovim syntax (`b:current_syntax = "freebasic"`).

## Install

### VS Code / TextMate-compatible editors

Copy or symlink `freebasic.tmLanguage.json` into an extension's `syntaxes/`
directory and register it for the `bas`/`bi` file types.

### vim

```
mkdir -p ~/.vim/syntax ~/.vim/ftdetect
cp editors/freebasic.vim ~/.vim/syntax/freebasic.vim
printf 'au BufRead,BufNewFile *.bas,*.bi set filetype=freebasic\n' \
  > ~/.vim/ftdetect/freebasic.vim
```

### Neovim

Use the same files under `stdpath("config")/syntax` and
`stdpath("config")/ftdetect`, or drop them into a runtime path.
