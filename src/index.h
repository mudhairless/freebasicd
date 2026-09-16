#pragma once

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "symbols.h"

namespace fblang {

struct AnalyzedDoc;

// One `#include [once] "target"` edge of an indexed file. `target` is the
// resolved normalized absolute path; when empty the include was unresolved and
// the open-buffer publish path emits an `include-not-found` diagnostic from
// `literal` + `targetRange`.
struct IncludeEdge {
    std::string target;       // normalized absolute path; empty when unresolved
    std::string literal;      // filename as written, case preserved
    SourceRange targetRange;  // filename literal range in source (quotes excluded)
    bool once = false;        // `#include once`
};

// One workspace file's in-memory symbol tree plus the disk state it was parsed
// from. Immutable once published; updates replace the shared_ptr wholesale.
struct IndexedFile {
    std::string path;         // normalized absolute path
    std::uint64_t mtime = 0;  // std::filesystem last_write_time ticks
    std::uint64_t size = 0;   // content bytes
    std::string lang = "fb";
    std::vector<Symbol> roots;
    std::vector<IncludeEdge> includes;

    // The file carries a `#pragma once` line: a self-granted once-guard,
    // recorded as M6 metadata. Not enforced yet (FreeBASIC.md §12.6: guard
    // states are not evaluated); the per-path dedup in transitiveIncludes is
    // intentional and unaffected.
    bool pragmaOnce = false;

    // Whether this entry came from a scan of the on-disk source (true) or from
    // a live open buffer (false). Scan is disk truth, buffers are live truth:
    // a `fromDisk=false` entry must never satisfy scan's mtime/size cache-hit,
    // or an unsaved buffer would shadow the source that scan is about to read.
    bool fromDisk = true;
};

// A module-scope declaration reachable through the #include closure from some
// workspace file, as answered by WorkspaceIndex::byKey.
struct KeyedDecl {
    std::shared_ptr<IndexedFile const> file;  // owner of `decl` (kept alive)
    Symbol const* decl;
};

// Per-workspace symbol index, purely in memory: no symbols or index state are
// ever written to disk.
//
// Thread-safe for the LSP handler pool. A background scan thread parses the
// workspace; entries are replaced wholesale on every update and queries read a
// consistent shared_ptr snapshot.
class WorkspaceIndex
{
public:
    explicit WorkspaceIndex(std::filesystem::path const& root);
    ~WorkspaceIndex();

    WorkspaceIndex(WorkspaceIndex const&) = delete;
    WorkspaceIndex& operator=(WorkspaceIndex const&) = delete;

    // Start the background threads (the debounced watched-files rescan).
    void open();
    // Stop background threads; nothing is written (the index is in memory).
    void close();

    // Re-stat every workspace file; parse changed/new files, drop vanished
    // ones. Reuses entries whose (mtime, size) still match — open-buffer
    // entries (fromDisk=false) never satisfy this cache-hit, so scan always
    // replaces a lingering buffer parse with disk truth. When `async` the
    // scan runs on an internal thread (returns immediately).
    void scan(bool async = true);

    // A `workspace/didChangeWatchedFiles` event arrived. A debounced rescan
    // follows on an internal thread: bursts coalesce into one scan, and the
    // notification FIFO thread never blocks on a scan. Safe to call from any
    // thread.
    void watchedFilesChanged();

    // Feed a file parsed from a live buffer or scan.
    void upsert(IndexedFile entry);
    void remove(std::string const& path);

    // Consistent snapshot: shared_ptr copies only, no deep copies.
    std::vector<std::shared_ptr<IndexedFile const>> snapshot() const;
    std::size_t size() const;

    // Module-scope declarations whose key (lowercase, suffix char included)
    // equals `key`. Includes open-buffer entries. Answers cross-file
    // references through the #include closure.
    std::vector<KeyedDecl> byKey(std::string const& key) const;

    // Transitively included normalized paths of `normalizedPath` (itself
    // excluded), textual include pre-order, each file once even through a
    // diamond; cycles (a.bi <-> b.bi) terminate.
    std::vector<std::string> transitiveIncludes(std::string const& normalizedPath) const;

    // The indexed entry for a normalized path, or nullptr when the index has
    // no entry (never indexed / not scanned yet). Includes open-buffer
    // entries. The returned shared_ptr pins the immutable snapshot.
    std::shared_ptr<IndexedFile const> fileAt(std::string const& normalizedPath) const;

    std::filesystem::path root() const;

private:
    void rescanLoop();
    void addToProjections(std::shared_ptr<IndexedFile const> const& f);
    void subtractFromProjections(std::shared_ptr<IndexedFile const> const& f);

    // True when `normalizedPath` lies at or under this workspace's root
    // (compared in the normalized form used by normalizePath). The index is
    // strictly workspace-scoped: system headers and stray open buffers outside
    // the root must never be indexed.
    bool isInsideRoot(std::string const& normalizedPath) const;

    std::filesystem::path root_;
    std::string rootNorm_;  // normalizePath(root_) at construction

    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<IndexedFile const>> files_;
    std::map<std::string, std::vector<KeyedDecl>> byKey_;                 // key -> module-scope decls
    std::map<std::string, std::vector<IncludeEdge>> outInc_;              // path -> include edges

    std::atomic<bool> running_{false};
    std::thread scanner_;

    // Debounced watched-files rescan: events coalesce in `rescanQueued_`, the
    // dedicated `rescanLoop` waits out a quiet window (kRescanDebounce), then
    // scans on `scanner_`. Keeping the loop here (not in the session) preserves
    // the close() join ordering: `rescan_` is joined before `scanner_`, so
    // close() can never race the loop's scan(true) join-previous.
    std::thread rescan_;
    std::mutex rescanMu_;
    std::condition_variable rescanCv_;
    bool rescanQueued_ = false;
};

// Free functions, exposed for tests.

// System FreeBASIC headers ship next to the compiler install: Windows keeps
// them at `<exeDir>/inc`, POSIX at `<exeDir>/../include/freebasic`. Pure:
// derives the layout from an executable directory; existence is not checked.
std::filesystem::path systemIncludeDirForExecutable(std::filesystem::path const& fbcExeDir);

// Search the given PATH (split per platform) for an `fbc` executable and
// return its directory (canonicalized so a PATH shim tracks the real install),
// or nullopt when FreeBASIC is not installed. Does not check the headers dir.
std::optional<std::filesystem::path> findFbcExecutableDir(std::string const& pathEnv);

// The cached default system include dir: probes PATH for `fbc` once and
// verifies the derived folder exists on disk. Returns an empty path when fbc
// is not installed or was installed without its headers; thread-safe.
std::filesystem::path defaultSystemIncludeDir();

// Resolve `literal` as an include target, mirrors fbc's relative-path search
// order (its `-i` dirs join via an M11 settings option later):
//   1. relative to the including file's own directory;
//   2. relative to the workspace root;
//   3. relative to each immediate subdirectory of the workspace root
//      (projects keep shared headers in `inc` / `include` / `src` etc., so
//      `#include "folder/file.bi"` matches under such a child);
//   4. relative to the FreeBASIC installation's system header folder, resolved
//      from `fbc` on PATH (`systemIncludeDir`; pass an empty path to skip the
//      system search).
// Both `/` and `\` separators are accepted. Returns the normalized absolute
// path, or nullopt when the file exists nowhere (kept as an unresolved edge
// for the M6 include diagnostics).
std::optional<std::string> resolveIncludeTarget(
    std::string_view literal,
    std::filesystem::path const& includingFile,
    std::filesystem::path const& workspaceRoot,
    std::filesystem::path const& systemIncludeDir = defaultSystemIncludeDir());

// An IndexedFile built from one shared analysis. `doc`, `mtime`, and `size`
// describe the file the buffer or scan produced; include targets are resolved
// against `workspaceRoot` from the including file's directory. `doc`'s symbol
// tree is moved into `roots` (a moved-from AnalyzedDoc must not be reused for
// indexing). `fromDisk=false` marks an open-buffer entry that must never
// satisfy scan's mtime/size cache-hit.
IndexedFile indexedFileFromAnalysis(std::string const& normalizedPath, std::uint64_t mtime,
                                    std::uint64_t size, AnalyzedDoc&& doc,
                                    std::filesystem::path const& workspaceRoot,
                                    bool fromDisk);

// Normalization helpers, exposed for tests.
std::string normalizePath(std::filesystem::path const& path);
bool statFile(std::filesystem::path const& path, std::uint64_t* mtime, std::uint64_t* size);

}  // namespace fblang