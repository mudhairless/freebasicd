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

// One workspace file's cached symbol tree plus the disk state it was parsed
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

    // Whether this entry may be written to the disk cache. False only for
    // open-buffer entries: an unsaved buffer must never be persisted as if it
    // were disk truth, and must never satisfy scan's mtime/size cache-hit.
    // Never serialized.
    bool persisted = true;
};

// A module-scope declaration reachable through the #include closure from some
// workspace file, as answered by WorkspaceIndex::byKey.
struct KeyedDecl {
    std::shared_ptr<IndexedFile const> file;  // owner of `decl` (kept alive)
    Symbol const* decl;
};

// Per-workspace symbol index. Live queries read an in-memory snapshot; a JSON
// file per indexed source persists each file's symbols, occurrences, and
// include edges outside the workspace so no symbol ever leaks into the
// codebase on disk. Every cache filename is the SHA-256 hex digest of the
// source path it caches (direct lookup), and each workspace owns a subdirectory
// keyed by the SHA-256 of its root, so workspaces never share entries. The
// cache is a warm-start optimization only: entries are validated against file
// mtime/size on load and scan, and a corrupt or mismatched cache file is
// discarded and rebuilt.
//
// Thread-safe for the LSP handler pool. A background scan thread parses the
// workspace; a debounced flusher persists changes atomically (temp + rename).
class WorkspaceIndex
{
public:
    // `cacheDir` empty selects the platform index dir (see defaultCacheDir);
    // tests pass a temp dir. The per-workspace subdirectory is derived from
    // `root`, never from `cacheDir`.
    explicit WorkspaceIndex(std::filesystem::path const& root,
                            std::filesystem::path const& cacheDir = std::filesystem::path{});
    ~WorkspaceIndex();

    WorkspaceIndex(WorkspaceIndex const&) = delete;
    WorkspaceIndex& operator=(WorkspaceIndex const&) = delete;

    // Load persisted entries and start the background flusher.
    void open();
    // Stop background threads and write any pending state.
    void close();

    // Re-stat every workspace file; parse changed/new files, drop vanished
    // ones (and their cache files). Reuses persisted entries whose
    // (mtime, size) still match. When `async` the scan runs on an internal
    // thread (returns immediately).
    void scan(bool async = true);

    // A `workspace/didChangeWatchedFiles` event arrived. A debounced rescan
    // follows on an internal thread: bursts coalesce into one scan, and the
    // notification FIFO thread never blocks on a scan. Safe to call from any
    // thread.
    void watchedFilesChanged();

    // Feed a file parsed from a live buffer or scan. `flush` schedules the
    // next debounced write.
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

    std::filesystem::path root() const;
    std::filesystem::path cacheDir() const;    // platform index dir (or test override)
    std::filesystem::path indexDir() const;    // this workspace's subdirectory
    std::filesystem::path cachePathFor(std::string const& normalizedPath) const;

    // Persistence primitives (also exercised directly by tests).
    bool loadFromDisk();
    void flushSoon();

private:
    void flushNow();
    void flusherLoop();
    void rescanLoop();
    void removeCacheFile(std::string const& normalizedPath);
    void addToProjections(std::shared_ptr<IndexedFile const> const& f);
    void subtractFromProjections(std::shared_ptr<IndexedFile const> const& f);

    std::filesystem::path root_;
    std::filesystem::path cacheDir_;
    std::filesystem::path indexDir_;

    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<IndexedFile const>> files_;
    std::map<std::string, std::vector<KeyedDecl>> byKey_;                 // key -> module-scope decls
    std::map<std::string, std::vector<IncludeEdge>> outInc_;              // path -> include edges

    std::atomic<bool> running_{false};
    std::thread flusher_;
    std::thread scanner_;
    std::mutex cvMu_;
    std::condition_variable cv_;
    bool dirty_ = false;

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

// Resolve `literal` as an include target: relative to `includingFile`'s
// directory first, then the workspace `root`; both `/` and `\` separators are
// accepted. Returns the normalized absolute path, or nullopt when it does not
// exist (kept as an unresolved edge for the M6 include diagnostics).
std::optional<std::string> resolveIncludeTarget(std::string_view literal,
                                                std::filesystem::path const& includingFile,
                                                std::filesystem::path const& workspaceRoot);

// An IndexedFile built from one shared analysis. `doc`, `mtime`, and `size`
// describe the file the buffer or scan produced; include targets are resolved
// against `workspaceRoot` from the including file's directory. `doc`'s symbol
// tree is moved into `roots` (a moved-from AnalyzedDoc must not be reused for
// indexing). `persisted=false` marks an open-buffer entry that must never be
// written to the disk cache.
IndexedFile indexedFileFromAnalysis(std::string const& normalizedPath, std::uint64_t mtime,
                                    std::uint64_t size, AnalyzedDoc&& doc,
                                    std::filesystem::path const& workspaceRoot,
                                    bool persisted);

// Normalization + keying helpers, exposed for tests.
std::string sha256Hex(std::string_view data);
std::string normalizePath(std::filesystem::path const& path);
std::string workspaceKey(std::string const& normalizedRoot);
std::filesystem::path defaultCacheDir();
// Per-source cache file: `dir` / (<sha256 of normalized path>.json).
std::filesystem::path cacheFileFor(std::filesystem::path const& dir, std::string const& normalizedPath);
bool statFile(std::filesystem::path const& path, std::uint64_t* mtime, std::uint64_t* size);

}  // namespace fblang