# Kate

Kate's **LSP Client** plugin talks to `freebasicd` over stdio, and the plugin
has a settings page that edits the server configuration as JSON, so this is
almost entirely a GUI setup. Kate also ships a FreeBASIC syntax definition, so
highlighting works with nothing installed.

## 1. Enable the plugin

The plugin ships with Kate; whether it is already enabled depends on your
distribution. Open **Settings → Configure Kate… → Plugins**, tick **LSP
Client** if it is not already ticked, then **OK**. Kate's own documentation
notes that the LSP Client page only appears in the configuration dialog *after*
the plugin is enabled, which is why this step comes first.

## 2. Add the server

**Settings → Configure Kate… → LSP Client**, then the **Server Config** tab.
It shows the JSON Kate merges into its built-in server list, at
`~/.config/kate/lspclient/settings.json`. Add one entry to its `servers`
object:

```json
{
  "servers": {
    "FreeBASIC": {
      "command": ["/usr/local/bin/freebasicd"],
      "rootIndicationFileNames": [".git", "freebasicd.toml"]
    }
  }
}
```

- `FreeBASIC` is the language name Kate's own syntax definition uses, and that
  is how the plugin matches a buffer to a server. It is case-insensitive. If
  you ever rename your syntax definition, add
  `"highlightingModeRegex": "^FreeBASIC$"` to the entry instead.
- `command` is a list. Give an absolute path: the plugin looks the binary up in
  `PATH` and warns if it does not find it, and an absolute path also keeps the
  answer from depending on what Kate's environment happens to be.
- `rootIndicationFileNames` is how Kate picks the project root it sends as
  `rootUri`. `.git` covers a normal checkout; `freebasicd.toml` covers a
  directory that has no VCS and where the server's settings file marks the
  project anyway.

The page validates the JSON as you type. Save, and Kate re-reads the
configuration immediately.

## 3. First start

Open a `.bas` or `.bi` file. The status area reports a positive
*Started server …* message naming the command it launched. The first time, Kate
asks whether it may run that command — allow it, or every subsequent file will
ask again.

If it says *"Failed to find server binary"*, the path in `command` is wrong for
this user. If nothing starts at all, check that the file was detected as
FreeBASIC: the status bar shows the detected syntax, and it must not be
*Plain Text*.

## 4. Turn on the highlighting

Back in **Settings → Configure Kate… → LSP Client**, tick:

- **Semantic highlighting** — FreeBASIC identifiers, types, keywords, and
  strings recoloured by what the server resolved them to. Off by default in
  Kate, and it is the most visible thing `freebasicd` contributes.
- **Inlay hints** — inferred types, and the block closers.
- **Diagnostics** — the squiggles (on by default in Kate).

## What you get, and what Kate cannot show

Diagnostics, completion, hover, go to definition, find references, rename,
document and workspace symbols, quick fixes, signature help, folding, selection
ranges (**Expand Selection** and **Shrink Selection** in the LSP Client menu —
assign them a shortcut under *Settings → Configure Shortcuts*, since Kate ships
none), and the syntax highlighting above.

Four features have no Kate UI and need a plugin, whatever the server sends:
**code lens** (the "N references" line above a declaration), **call hierarchy**,
**type hierarchy**, and **implementation**.

## Project settings

Do not put settings in the JSON above. `freebasicd` takes no initialization
options and ignores the configuration payload: it reads `freebasicd.toml` from
the project root on disk, and re-reads it when the file changes. See
[Configuration](../../README.md#configuration).