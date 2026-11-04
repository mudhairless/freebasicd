# Architecture

Read this file before changing the session boundary, workspace index,
configuration handling, concurrency model, or an LSP feature module. The
short rules in `AGENTS.md` remain binding when this file gives more detail.

## Layering

The language modules are independent of LSP:

- `lexer`, `parser`, and `language` work in byte offsets.
- `session` owns protocol interaction.
- Positions cross the boundary through LspCpp `WorkingFile` line offsets and
  the helpers in `utf16.h`.
- All LSP positions are UTF-16 code units.

Use `lsp::LanguageSession` and register handlers with `server.on(...)`.
Request types are `td_<feature>::request` and `td_<feature>::response`.
Notifications are `Notify_<Name>::notify`. Document buffers use
`WorkingFiles` and `WorkingFile`, including `OnOpen`, `OnChange`, `OnClose`,
`GetOffsetForPosition`, and `RebuildLineOffsets`. Push diagnostics use
`Notify_TextDocumentPublishDiagnostics::notify`.

Advertise only implemented capabilities. Advertise
`positionEncoding: "utf-16"`.

## Boundary discipline

`session` is the only system boundary. Parse and validate protocol
parameters, `freebasicd.toml`, paths, and other external input there. Pure
modules may then trust their inputs.

Report a contradiction at the boundary instead of silently resolving it. A
malformed value must not degrade the whole session.

## Concurrency and snapshots

LspCpp runs a parse pool, a handler pool, and a FIFO notification thread.
Requests can therefore run at the same time.

- Keep `max_workers=2`.
- Guard shared language state with a mutex.
- Take a lock only long enough to snapshot the state a handler needs.
- Build the response without holding that lock.
- Snapshot a `shared_ptr<WorkspaceIndex>` and hold it while using raw
  pointers into its entries.

The lifetime rules behind these points are in [Testing](testing.md),
including the rules for result types that return pointers into a snapshot.

## Workspace index

Each `WorkspaceIndex` is in memory only. The server never writes index data
to disk.

A background scan parses the workspace. Watched-file events trigger a
debounced rescan. Open-buffer entries have `fromDisk=false`, and a scan skips
those entries. Byte offsets in an index entry must describe the bytes from
which a reply will be measured.

The write side of the rule is the important half: `upsert` refuses to let a
`fromDisk` entry replace a `fromDisk=false` entry. Do not move that
protection to a caller-side check. Closed files remain disk truth. That is
what lets a watched-file event pick up an external edit after the buffer is
closed.

Since M11, the server owns one index per workspace root. The map is
`indexes_`, keyed by normalized root and guarded by `indexesMutex_`. It can
hold registered client folders that are workspace roots, detected roots, and
single-file roots. Session handlers use `indexFor` for one path and
`allIndexes()` for workspace-wide requests such as workspace symbols.

Watched-file events route to the index that owns the path and are
deduplicated by owner root. Adding or removing a folder rekeys the map.

## Workspace roots and settings

`chooseIndexRoot` uses this priority order:

0. The deepest registered marker root containing the file.
1. A client root that is itself a workspace root.
2. The nearest version-control marker.
3. The nearest `freebasicd.toml`.
4. The source and include layout found by `findSourceLayoutRoot`, walking no
   farther than the drive root or home directory.
5. Single-file mode when there is no client root.

A file outside every index root is served in resolution-only mode through
the session-root index and its on-demand include closure. Do not give it its
own index, because it would leak into workspace symbol results. Do not use
the single-file branch while a client root exists.

When a detected root replaces a client-provided root, log the replacement
and the signal that caused it to stderr. The unbounded walks end at the home
folder. `isHomeFolder` identifies it with `std::filesystem::equivalent`, not
a path comparison. Windows can spell the same path in long and 8.3-short
forms, and a symlinked home directory keeps the link.

### Settings lifecycle

`src/settings.h` and `src/settings.cpp` parse `freebasicd.toml`. The presence
of the file is a workspace-root marker. `Settings` keys have fixed defaults.

Each index receives a `Settings` snapshot when it is created through
`ensureWorkspaceIndex`:

```cpp
index->applySettings(settingsForDirLogged(root));
```

`workspace/didChangeConfiguration` ignores the payload and rereads each
root's file. The file is the truth, and rereading it is idempotent. Applying
a changed snapshot:

- Reindexes include edges without reparsing files.
- Reconciles open buffers under that root.
- Republishes diagnostics when diagnostics are enabled.
- Publishes one empty diagnostic set and then stays silent when diagnostics
  are disabled.
- Reresolves include edges for open buffers when `includePaths` changes.

Diagnostics, semantic tokens, inlay hints, code lens, and code actions gate
on `settingsForDocument`. A disabled feature returns the appropriate empty
result.

## Quick fixes

`src/code_actions.h` and `src/code_actions.cpp` form the LSP-agnostic,
byte-offset quick-fix layer. Providers are registered in
`quickFixProviders()` and looked up with `quickFixProviderFor`.

A provider is a pure function of a diagnostic and a `QuickFixContext`. The
context is its only route to workspace state. It carries an index snapshot,
the document path, and a `resolveInclude` callback wired to
`resolveIncludeTarget`. Offer a fix only when one of those seams accepts the
candidate. If the context cannot prove the fix is right, offer nothing.

Adding a fix means adding one provider function and one table row. Do not
edit `session.cpp` or the capability registration for a new fix.

A fix ships as an LSP `CodeAction` with:

- `title`
- `kind: "quickfix"`
- The diagnostic it answers
- `edit.changes` keyed by the request document's URI

Never ship the edit as a `Command`. The client executes a command. An
empty-ID command with an edit in its arguments can appear in the menu and do
nothing when selected.

Filter `context.only` on the server. Build diagnostics used by a provider
with the same function used by the publish path. For unresolved includes,
that function is `unresolvedIncludeDiagnostics`. Separate construction paths
will eventually publish one range and fix another.

## Code lens

`src/code_lens.h` and `src/code_lens.cpp` form the LSP-agnostic, byte-offset
code-lens layer.

`carriesLens` decides which declarations receive a lens. It covers
procedure-like declarations and `Type`, `Union`, `Enum`, and `Namespace`,
with nesting flattened. The module owns the title and obtains counts through
one `ReferenceCounter` seam. Add a lens kind in this module only.

`LensAnchor` is pointer-free, so a lens does not pin an index snapshot.
`resolveProvider` is false, and lenses carry no `data` value. A lens range is
the declaration name token. Selecting it sends the same position to the
`freebasicd.showReferences` command, advertised through
`executeCommandProvider`.

Counts and reference lists use `referenceSites(..., scope)`:

- `Closure` means the request file and its transitive includes.
- `Workspace` also includes files that include the closure, using the
  reverse-reachability graph.

Match sites by `DeclIdentity`, never by `Symbol` pointer. Each candidate
file has its own parse, so one declaration is a different object in each
analysis.

A lens cannot request `includeDeclaration`. Its count excludes the
declaration's own name token, matching `textDocument/references` with that
option set to false.

## Pull diagnostics

Pull diagnostics are the LSP 3.17 alternative to push diagnostics. They are
negotiated and never assumed.

- If `capabilities.textDocument.diagnostic` is present, advertise
  `diagnosticProvider` and disable `publishDiagnostics` for the whole
  session.
- If it is absent, use the push path unchanged.

A client must never receive the same diagnostic through both paths.

### One payload

`documentDiagnostics(content, parse, entry)` is the single definition of a
document's diagnostics. It combines parse diagnostics with the document's
own unresolved include edges. The push path and both pull handlers call it.

### Result IDs

A pull result ID is `AnalysisCache::hashContent(content)` folded with:

- The document's include edges.
- The diagnostics gate.

Compute it fresh for each request. Equal IDs promise equal payloads. Fold in
nothing else. A request with a matching `previousResultId` can then receive
an `unchanged` answer without recomputing the report.

The diagnostics gate belongs in the ID. A configuration change can empty a
report, and a client that caches by ID must not keep the old report.

### Related documents and refresh

Related documents are the transitive include closure, one level deep. A
related report is never itself related. The feature is gated on
`relatedDocumentSupport`.

`workspace/diagnostic/refresh` is the server hint used where a push would
otherwise occur. Fire it after:

- An edit.
- A close.
- A configuration change.
- A completed watched-file rescan.

The rescan uses `WorkspaceIndex::onRescanCompleted`. Copy the callback out
under `rescanMu_`, then call it with no lock held.

## Message catalogs

Use `fblang::trf` for a user-visible message. Use
`fblang::trn(msgid, plural, n)` when a count can change its grammatical
form. Only the catalog plural rule can choose correctly in a language with
three or four forms.

Both helpers use `detail::substitutePercentS`. It substitutes `%s` only and
inserts each argument verbatim. Every argument is already a `std::string`.
Editor UI strings are messages too, including code-lens titles.

After adding or changing a message, regenerate `po/freebasicd.pot` with the
`po-template` target. `i18n_checks` fails when a literal is missing. Do not
regenerate `po/*.po` in the same change unless the task calls for it. Those
files deliberately lag the template.
