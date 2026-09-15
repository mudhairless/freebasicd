#pragma once

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "symbols.h"

namespace fblang {

// One workspace file's cached symbol tree plus the disk state it was parsed
// from. Immutable once published; updates replace the shared_ptr wholesale.
struct IndexedFile {
    std::string path;         // normalized absolute path
    std::uint64_t mtime = 0;  // std::filesystem last_write_time ticks
    std::uint64_t size = 0;   // content bytes
    std::string lang = "fb";
    std::vector<Symbol> roots;
};

// Per-workspace symbol index. Live queries read an in-memory snapshot; a JSON
// file per indexed source persists each file's symbols outside the workspace
// so no symbol ever leaks into the codebase on disk. Every cache filename is
// the SHA-256 hex digest of the source path it caches (direct lookup), and
// each workspace owns a subdirectory keyed by the SHA-256 of its root, so
// workspaces never share entries. The cache is a warm-start optimization only:
// entries are validated against file mtime/size on load and scan, and a
// corrupt or mismatched cache file is discarded and rebuilt.
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

    // Feed a file parsed from a live buffer or scan. `flush` schedules the
    // next debounced write.
    void upsert(IndexedFile entry);
    void remove(std::string const& path);

    // Consistent snapshot: shared_ptr copies only, no deep copies.
    std::vector<std::shared_ptr<IndexedFile const>> snapshot() const;
    std::size_t size() const;

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
    void removeCacheFile(std::string const& normalizedPath);

    std::filesystem::path root_;
    std::filesystem::path cacheDir_;
    std::filesystem::path indexDir_;

    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<IndexedFile const>> files_;

    std::atomic<bool> running_{false};
    std::thread flusher_;
    std::thread scanner_;
    std::mutex cvMu_;
    std::condition_variable cv_;
    bool dirty_ = false;
};

// Normalization + keying helpers, exposed for tests.
std::string sha256Hex(std::string_view data);
std::string normalizePath(std::filesystem::path const& path);
std::string workspaceKey(std::string const& normalizedRoot);
std::filesystem::path defaultCacheDir();
// Per-source cache file: `dir` / (<sha256 of normalized path>.json).
std::filesystem::path cacheFileFor(std::filesystem::path const& dir, std::string const& normalizedPath);
bool statFile(std::filesystem::path const& path, std::uint64_t* mtime, std::uint64_t* size);

}  // namespace fblang
