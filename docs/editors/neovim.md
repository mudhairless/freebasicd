# Neovim

Neovim's LSP client is built in, so there is nothing to install — a config file
and a syntax file are the whole setup. This page covers 0.11 and newer first,
since that is where the recommended API lives, then 0.10.

## 1. Filetype and syntax file

Neovim does detect `.bas` and `.bi`, but as **Bison** — its filetype table maps
both extensions to `bison`. That has to be overridden before anything is read,
so in your config (`init.lua`, or
`~/.config/nvim/lua/config/filetype.lua`):

```lua
vim.filetype.add({ extension = { bas = 'freebasic', bi = 'freebasic' } })
```

An `ftdetect` file does not work here, which is worth knowing rather than
rediscovering: Neovim runs its own extension table first and sources
`ftdetect` scripts only when nothing has set a filetype yet, so a
`au BufRead,BufNewFile *.bas setf freebasic` line in `ftdetect/freebasic.vim`
never runs for a file that is already `bison`. `vim.filetype.add` writes into the
same table the built-in table came from, so it wins.

Then install the syntax file itself:

```sh
# Linux / macOS
mkdir -p ~/.config/nvim/syntax
cp /usr/local/share/freebasicd/grammar/freebasic.vim ~/.config/nvim/syntax/freebasic.vim
```

On Windows the config directory is `%LOCALAPPDATA%\nvim-data`. From a source
checkout instead of an install, the file is
[docs/grammar/freebasic.vim](../grammar/freebasic.vim) — generated from the
server's keyword catalog. Check it worked with `:set syntax?`, which prints
`freebasic`.

## 2. The server

Neovim 0.11 and newer read server configs from `lsp/<name>.lua` in your config
directory. Create `~/.config/nvim/lsp/freebasicd.lua`:

```lua
return {
  cmd = { '/usr/local/bin/freebasicd' },
  filetypes = { 'freebasic' },
  root_markers = { { 'freebasicd.toml' }, '.git' },
}
```

Enable it once, from anywhere in your config (`init.lua`, or
`~/.config/nvim/lua/config/lsp.lua` if you split things up):

```lua
vim.lsp.enable('freebasicd')
```

That is the entire setup. `vim.lsp.enable` starts the server whenever a buffer
whose filetype is `freebasic` is opened, once one of `root_markers` is found
walking up from the file. Use a bare `freebasicd` instead of the absolute path
if it is on your `PATH`.

If you prefer to define it inline, the same config as two calls:

```lua
vim.lsp.config('freebasicd', {
  cmd = { 'freebasicd' },
  filetypes = { 'freebasic' },
  root_markers = { { 'freebasicd.toml' }, '.git' },
})
vim.lsp.enable('freebasicd')
```

### Neovim 0.10

0.10 has no `vim.lsp.config`, so start it by hand in `init.lua`:

```lua
vim.lsp.start({
  name = 'freebasicd',
  cmd = { '/usr/local/bin/freebasicd' },
  filetypes = { 'freebasic' },
  root_dir = function(fname)
    return vim.fs.root(0, { 'freebasicd.toml', '.git' })
  end,
})
```

## 3. Turn on the two optional displays

Both are off by default in current Neovim. Once, in your config:

```lua
vim.lsp.inlay_hint.enable(true)
vim.lsp.codelens.enable(true)
```

Inlay hints show inferred types and block closers. Code lens adds the "N
references" line above each declaration; clicking it lists the sites.

## What you get

Every feature this server implements. Only the rows marked *default* come with a
mapping out of the box; the rest are `vim.lsp.buf` functions you call from a key
or an `:lua` one-liner.

| Feature | How |
|---|---|
| Diagnostics | automatic; `:lua vim.diagnostic.open_float()` for details |
| Completion | automatic |
| Signature help | automatic, `<C-s>` cycles *default* |
| Hover | `K` *default* |
| Go to definition | `vim.lsp.buf.definition()` |
| Go to declaration | `vim.lsp.buf.declaration()` |
| Find references | `grr` *default* |
| Rename | `grn` *default* |
| Document highlight | `vim.lsp.buf.document_highlight()` |
| Document symbols | `gO` *default* |
| Workspace symbols | `vim.lsp.buf.workspace_symbol()` |
| Quick fixes | `gra` *default* |
| Implementation | `gri` *default* |
| Semantic tokens | automatic |
| Inlay hints | `vim.lsp.inlay_hint.enable(true)` |
| Code lens | `vim.lsp.codelens.enable(true)`, then click the lens |
| Call hierarchy | `vim.lsp.buf.incoming_calls()`, `outgoing_calls()` |
| Type hierarchy | `vim.lsp.buf.typehierarchy()` |
| Type definition | `vim.lsp.buf.type_definition()` |
| Selection ranges | `vim.lsp.buf.selection_range()` — 0.12 and newer |

Go to definition deserves a note, because it is the one everybody expects and
the one Neovim does not map: `gd` is Vim's own *include-file* lookup, not an LSP
binding. Map it over:

```lua
local function lspmap(lhs, method)
  vim.keymap.set('n', lhs, function() vim.lsp.buf[method]() end, { desc = method })
end
lspmap('gd', 'definition')
lspmap('gD', 'declaration')
lspmap('gr', 'references')
```

The command set grew across 0.11 and 0.12, so `:h vim.lsp.buf` is the authority
for the build you have.

## Verify

`:checkhealth vim.lsp` lists freebasicd under "Enabled Configurations". Open a
`.bas` file in a project directory and `:LspInfo` shows the attached client, its
root, and whether semantic tokens are in use.

Two things to know when something looks wrong:

- No server at all means the filetype did not resolve. `:set filetype?` should
  print `freebasic`; if it prints empty or `bison`, step 1 did not take effect.
- A server that attaches but answers nothing usually means no root was found,
  because neither `.git` nor `freebasicd.toml` was found above the file.
  `:LspInfo` prints the root it chose.

## Project settings

Do not add a `settings` block. `freebasicd` reads `freebasicd.toml` from the
project root and ignores client configuration — see
[Configuration](../../README.md#configuration).