/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "settings.h"
#include "symbols.h"

namespace fblang {

struct AnalyzedDoc;

// One `#include [once] "target"` edge of an indexed file. `target` is the
// resolved normalized absolute path; when empty the include was unresolved and
// the open-buffer publish path emits an `include-not-found` diagnostic from
// `literal` + `targetRange`.
struct IncludeEdge {
  std::string target;      // normalized absolute path; empty when unresolved
  std::string literal;     // filename as written, case preserved
  SourceRange targetRange; // filename literal range in source (quotes excluded)
  bool once = false;       // `#include once`
};

// One workspace file's in-memory symbol tree plus the disk state it was parsed
// from. Immutable once published; updates replace the shared_ptr wholesale.
struct IndexedFile {
  std::string path;        // normalized absolute path
  std::uint64_t mtime = 0; // std::filesystem last_write_time ticks
  std::uint64_t size = 0;  // content bytes
  std::string lang = "fb";
  std::vector<Symbol> roots;
  std::vector<IncludeEdge> includes;

  // The file carries a `#pragma once` line: a self-granted once-guard,
  // recorded as M6 metadata. Not enforced yet (FreeBASIC.md §12.6: guard
  // states are not evaluated); the per-path dedup in transitiveIncludes is
  // intentional and unaffected.
  bool pragmaOnce = false;

  // Whether this entry came from a scan of the on-disk source (true) or from
  // a live open buffer (false). Buffers outrank disk, and that is the whole
  // point of the flag: the parse here records byte offsets, and a reply
  // converts them with a line table built from a *different* copy whenever the
  // two disagree (an unsaved edit, or line endings the editor normalized on the
  // way in), which lands the range one column out per line they differ. So scan
  // skips a `fromDisk=false` path outright rather than treating it as a
  // cache-hit miss, and didChange is what refreshes it.
  bool fromDisk = true;
};

// A module-scope declaration reachable through the #include closure from some
// workspace file, as answered by WorkspaceIndex::byKey.
struct KeyedDecl {
  std::shared_ptr<IndexedFile const> file; // owner of `decl` (kept alive)
  Symbol const *decl;
};

// Builds the resolution entry for a normalized path on demand: reads the live
// open buffer when the client has one, else the file on disk, and parses it.
// Returns nullptr when the path cannot be served (deleted / unreadable). Used
// only by WorkspaceIndex::ensureClosure for documents outside the workspace
// root, whose closure a workspace scan never reaches.
using FileResolver =
    std::function<std::shared_ptr<IndexedFile const>(std::string const &)>;

// Per-workspace symbol index, purely in memory: no symbols or index state are
// ever written to disk.
//
// Thread-safe for the LSP handler pool. A background scan thread parses the
// workspace; entries are replaced wholesale on every update and queries read a
// consistent shared_ptr snapshot.
class WorkspaceIndex {
public:
  explicit WorkspaceIndex(std::filesystem::path const &root);
  ~WorkspaceIndex();

  WorkspaceIndex(WorkspaceIndex const &) = delete;
  WorkspaceIndex &operator=(WorkspaceIndex const &) = delete;

  // Start the background threads (the debounced watched-files rescan).
  void open();
  // Stop background threads; nothing is written (the index is in memory).
  void close();

  // Re-stat every workspace file; parse changed/new files, drop vanished
  // ones. Reuses entries whose (mtime, size) still match. Open-buffer entries
  // (fromDisk=false) are skipped entirely — the client owns those bytes until
  // didClose, and re-parsing the disk copy over them would leave the index
  // describing a different file than the one a reply is measured against. When
  // `async` the scan runs on an internal thread (returns immediately).
  void scan(bool async = true);

  // A `workspace/didChangeWatchedFiles` event arrived. A debounced rescan
  // follows on an internal thread: bursts coalesce into one scan, and the
  // notification FIFO thread never blocks on a scan. Safe to call from any
  // thread.
  void watchedFilesChanged();

  // Feed a file parsed from a live buffer or scan.
  void upsert(IndexedFile entry);
  void remove(std::string const &path);

  // Consistent snapshot: shared_ptr copies only, no deep copies.
  std::vector<std::shared_ptr<IndexedFile const>> snapshot() const;
  std::size_t size() const;

  // Module-scope declarations whose key (lowercase, suffix char included)
  // equals `key`. Includes open-buffer entries. Answers cross-file
  // references through the #include closure.
  std::vector<KeyedDecl> byKey(std::string const &key) const;

  // Transitively included normalized paths of `normalizedPath` (itself
  // excluded), textual include pre-order, each file once even through a
  // diamond; cycles (a.bi <-> b.bi) terminate. Answers from the workspace
  // scan/open-buffer entries first, and additionally from the on-demand
  // closure store (see ensureClosure) when the requesting document lives
  // outside the workspace root.
  std::vector<std::string>
  transitiveIncludes(std::string const &normalizedPath) const;

  // The indexed entry for a normalized path, or nullptr when the index has
  // no entry (never indexed / not scanned yet). Includes open-buffer entries
  // and on-demand closure entries (see ensureClosure). The returned
  // shared_ptr pins the immutable snapshot.
  std::shared_ptr<IndexedFile const>
  fileAt(std::string const &normalizedPath) const;

  // Resolve the transitive include closure of `normalizedPath` (a requesting
  // open document) on demand: every reachable file — the requesting file plus
  // each resolved include, transitively — is fetched through `resolver` and
  // recorded in the resolution maps only (`fileAt`/`transitiveIncludes`),
  // never in `snapshot()`/`byKey()` so workspace/symbol stays strictly
  // workspace-scoped. In-root files the index owns are left untouched. The
  // requesting file itself is always re-resolved (it is a live buffer);
  // closed files reuse the stored entry while their (mtime, size) is current
  // and an open-buffer closure file is always re-resolved. Safe to call from
  // the handler pool; concurrent walks for the same document share one
  // expansion.
  void ensureClosure(std::string const &normalizedPath,
                     FileResolver const &resolver);

  std::filesystem::path root() const;

  // Adopt a (possibly changed) configuration for this root: store a copy of
  // `s` (from `fblang::settingsForDir(root())`) and re-resolve every indexed
  // entry's include edges against the new include dirs. Cached entries reuse
  // their (mtime, size)-matching parse, so the include edges are re-resolved
  // explicitly here instead of left to a plain rescan (which would keep the
  // old edge targets forever). No file is re-read; the parse cache is
  // untouched. Safe to call from the notification thread while `scanner_` may
  // be running: it takes `mu_` and never starts a second scan.
  void applySettings(Settings const &s);

  // The settings snapshot currently governing this root (defaults until the
  // session applies them).
  Settings settings() const;

  // The resolved absolute include dirs (config includePaths relative to the
  // root), snapshot.
  std::vector<std::filesystem::path> includeDirs() const;

private:
  void rescanLoop();
  void addToProjections(std::shared_ptr<IndexedFile const> const &f);
  void subtractFromProjections(std::shared_ptr<IndexedFile const> const &f);

  // Rebuild every indexed entry with its IncludeEdge targets re-resolved from
  // the stored `literal` via resolveIncludeTarget, using `includeDirs_` as the
  // config-dir step. Replaces each entry wholesale and recomputes its
  // projections (`outInc_`, `byKey_` re-point at the new shared_ptr). No
  // re-parse. Caller holds `mu_` (applySettings calls it directly, and never
  // spawns a scan, so scan's single-scanner invariant is respected).
  void reindexIncludeEdges();

  // True when `normalizedPath` lies at or under this workspace's root
  // (compared in the normalized form used by normalizePath). The index is
  // strictly workspace-scoped: system headers and stray open buffers outside
  // the root must never be indexed.
  bool isInsideRoot(std::string const &normalizedPath) const;

  std::filesystem::path root_;
  std::string rootNorm_; // normalizePath(root_) at construction

  mutable std::mutex mu_;
  std::map<std::string, std::shared_ptr<IndexedFile const>> files_;
  std::map<std::string, std::vector<KeyedDecl>>
      byKey_; // key -> module-scope decls
  std::map<std::string, std::vector<IncludeEdge>>
      outInc_; // path -> include edges

  // The configuration governing this root (freebasicd.toml), applied by the
  // session after construction and re-applied on didChangeConfiguration.
  // Guarded by mu_ like the maps above. `includeDirs_` caches
  // settings_.includePaths resolved to absolute paths against root_.
  Settings settings_;
  std::vector<std::filesystem::path> includeDirs_;

  // Resolution-only closures for documents outside the workspace root (see
  // ensureClosure): the entry plus the disk state it was built from. Never
  // consulted by snapshot()/byKey()/scan — workspace/symbol stays strictly
  // workspace-scoped — but fileAt()/transitiveIncludes() answer from here.
  // Guarded by mu_ like the maps above.
  struct ClosureEntry {
    std::shared_ptr<IndexedFile const> entry;
    std::uint64_t mtime = 0;
    std::uint64_t size = 0;
  };
  std::map<std::string, ClosureEntry> closureFiles_;

  // Serializes on-demand closure walks so concurrent handler threads share
  // one expansion (and the resolver, which does file I/O, never runs inside
  // mu_).
  mutable std::mutex closureMu_;

  std::atomic<bool> running_{false};
  std::thread scanner_;
  // Serializes the scanner_ handoff in scan(true); see the comment there. The
  // rescan loop below is the other async caller, so "only the loop starts
  // scans" is not true.
  std::mutex scannerMu_;

  // Debounced watched-files rescan: events coalesce in `rescanQueued_`, the
  // dedicated `rescanLoop` waits out a quiet window (kRescanDebounce), then
  // scans on `scanner_`. Keeping the loop here (not in the session) preserves
  // the close() join ordering: `rescan_` is joined before `scanner_`, so
  // close() can never race the loop's scan(true) join-previous.
  //
  // close() clears `running_` under `rescanMu_`, never outside it: the loop
  // blocks in wait() on that mutex, and a store+notify that slips in between
  // its predicate check and its block is a lost wakeup, which parks the loop
  // forever and hangs close()'s join.
  std::thread rescan_;
  std::mutex rescanMu_;
  std::condition_variable rescanCv_;
  bool rescanQueued_ = false;
};

// Free functions, exposed for tests.

// System FreeBASIC headers ship next to the compiler install: Windows keeps
// them at `<exeDir>/inc`, POSIX at `<exeDir>/../include/freebasic`. Pure:
// derives the layout from an executable directory; existence is not checked.
std::filesystem::path
systemIncludeDirForExecutable(std::filesystem::path const &fbcExeDir);

// Search the given PATH (split per platform) for an `fbc` executable and
// return its directory (canonicalized so a PATH shim tracks the real install),
// or nullopt when FreeBASIC is not installed. Does not check the headers dir.
std::optional<std::filesystem::path>
findFbcExecutableDir(std::string const &pathEnv);

// The cached default system include dir: probes PATH for `fbc` once and
// verifies the derived folder exists on disk. Returns an empty path when fbc
// is not installed or was installed without its headers; thread-safe.
std::filesystem::path defaultSystemIncludeDir();

// Resolve `literal` as an include target, mirrors fbc's relative-path search
// order:
//   1. relative to the including file's own directory;
//   2. relative to each of the config-file include dirs in config order — the
//      resolved absolute dirs from Settings.includePaths (`-i` dirs; see
//      WorkspaceIndex::includeDirs), which land before the workspace-root
//      search so a configured dir can shadow the root layout;
//   3. relative to the workspace root;
//   4. relative to the including file's own FreeBASIC project directory — the
//      nearest ancestor with an `inc`/`include` child of the source tree —
//      and each immediate subdirectory of it, but only when the including
//      file lies outside the workspace root: an editor may open a document
//      from a sibling project, whose headers (fbc's `-i inc` layout) the
//      workspace-root search can never see;
//   5. relative to each immediate subdirectory of the workspace root
//      (projects keep shared headers in `inc` / `include` / `src` etc., so
//      `#include "folder/file.bi"` matches under such a child);
//   6. relative to the FreeBASIC installation's system header folder, resolved
//      from `fbc` on PATH (`systemIncludeDir`; pass an empty path to skip the
//      system search).
// Both `/` and `\` separators are accepted. Returns the normalized absolute
// path, or nullopt when the file exists nowhere (kept as an unresolved edge
// for the M6 include diagnostics).
std::optional<std::string> resolveIncludeTarget(
    std::string_view literal, std::filesystem::path const &includingFile,
    std::filesystem::path const &workspaceRoot,
    std::filesystem::path const &systemIncludeDir = defaultSystemIncludeDir(),
    std::vector<std::filesystem::path> const &includeDirs = {});

// An IndexedFile built from one shared analysis. `doc`, `mtime`, and `size`
// describe the file the buffer or scan produced; include targets are resolved
// against `workspaceRoot` from the including file's directory, with the
// root's config include dirs joining the search as step ② (see
// resolveIncludeTarget). `doc`'s symbol tree is copied into `roots` (the
// caller keeps `doc` alive). `fromDisk=false` marks an open-buffer entry,
// which scan skips rather than re-parsing from disk.
IndexedFile indexedFileFromAnalysis(
    std::string const &normalizedPath, std::uint64_t mtime, std::uint64_t size,
    AnalyzedDoc const &doc, std::filesystem::path const &workspaceRoot,
    bool fromDisk, std::vector<std::filesystem::path> const &includeDirs = {});

// Normalization helpers, exposed for tests.
std::string normalizePath(std::filesystem::path const &path);
bool statFile(std::filesystem::path const &path, std::uint64_t *mtime,
              std::uint64_t *size);

} // namespace fblang
