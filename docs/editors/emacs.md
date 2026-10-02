# Emacs

Eglot, Emacs's built-in LSP client, has shipped in the Emacs tree since 29.1,
so there is no package to install. Two things are needed: a major mode for
`.bas` / `.bi` files (Emacs ships none), and one entry in
`eglot-server-programs`.

## 1. A major mode for FreeBASIC

Emacs has no FreeBASIC mode, and Eglot attaches per major mode, so define one.
Put this in your init file:

```elisp
(define-derived-mode freebasic-mode prog-mode "FreeBASIC"
  "Major mode for FreeBASIC source files.")

(add-to-list 'auto-mode-alist '("\\.bas\\'" . freebasic-mode))
(add-to-list 'auto-mode-alist '("\\.bi\\'" . freebasic-mode))
```

That gives you a real mode (indentation, `isearch`, `outline`) with no
highlighting rules. Highlighting is not required for Eglot — it colors
identifiers, keywords, and types from the server's semantic tokens — but
Emacs 29 and 30 have no semantic token support, so in those versions the file
stays uncolored. See [What you get](#what-you-get).

## 2. Register the server

Still in your init file:

```elisp
(add-to-list 'eglot-server-programs
             '((freebasic-mode) . ("/usr/local/bin/freebasicd")))

(add-hook 'freebasic-mode-hook #'eglot-ensure)
```

`eglot-ensure` starts a session when a FreeBASIC buffer is opened and does
nothing if one is already running. Use the interactive `M-x eglot` instead if
you would rather start Eglot on demand.

The command is a list, so arguments would go after the binary. None are
needed, and Eglot figures the project root out itself from the buffer, which
is why there is no `:root-dir` here.

Eglot warns at startup if the binary is not on `PATH` or is not executable,
which is the first thing to check when a session does not come up.

## 3. Turn on inlay hints

In Eglot they are a minor mode, off by default:

```elisp
(add-hook 'freebasic-mode-hook #'eglot-inlay-hints-mode)
```

## What you get

| Feature | How |
|---|---|
| Diagnostics | flymake; `M-x flymake-diagnostics-buffer` for the list |
| Completion | `C-M-i` / `completion-at-point`, candidates from the server |
| Hover | eldoc, so it appears on its own as you move the cursor |
| Go to definition | `M-.`, back with `M-,` |
| Find references | `M-*` at the symbol |
| Rename | `M-x eglot-rename` |
| Document symbols | `M-x imenu` |
| Quick fixes | `M-x eglot-code-actions` |
| Signature help | eldoc shows it while you type |
| Inlay hints | the hook above |

There is no formatting: this server is not a formatter and does not advertise
the capability, so `M-x eglot-format-buffer` has nothing to call.

Not rendered by the released Emacs: **semantic tokens**
(`eglot-semantic-tokens-mode` exists in Emacs master but not in 29 or 30),
**code lens**, **selection ranges**, **call and type hierarchy**
(`eglot-show-call-hierarchy`, `eglot-show-type-hierarchy` and
`M-x eglot-find-typeDefinition` likewise exist only in Emacs master), and
**implementation**. The server answers all of them; the clients that can
display them are still catching up.

## Verify

`M-x eglot-current-server` shows the session for the current buffer, or an
error saying there is none. `M-x eglot-events-buffer` is the log: it records
every request and reply, and is the first place to look when a feature appears
inert. `M-x eglot-shutdown` ends a session.

## Project settings

Do not add `:initializationOptions`. `freebasicd` reads `freebasicd.toml` from
the project root and ignores client configuration — see
[Configuration](../../README.md#configuration).