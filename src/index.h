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
// file outside the workspace persists parsed files, keyed to the workspace
// root so no symbol ever leaks across workspaces. The cache is a warm-start
// optimization only: entries are validated against file mtime/size on load and
// scan, and a corrupt or mismatched cache is discarded and rebuilt.
//
// Thread-safe for the LSP handler pool. A background scan thread parses the
// workspace; a debounced flusher persists changes atomically (temp + rename).
class WorkspaceIndex
{
public:
    // `cacheDir` empty selects the platform data dir; tests pass a temp dir.
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
    // ones. Reuses persisted entries whose (mtime, size) still match. When
    // `async` the scan runs on an internal thread (returns immediately).
    void scan(bool async = true);

    // Feed a file parsed from a live buffer or scan. `flush` schedules the
    // next debounced write.
    void upsert(IndexedFile entry);
    void remove(std::string const& path);

    // Consistent snapshot: shared_ptr copies only, no deep copies.
    std::vector<std::shared_ptr<IndexedFile const>> snapshot() const;
    std::size_t size() const;

    std::filesystem::path root() const;
    std::filesystem::path indexFile() const;

    // Persistence primitives (also exercised directly by tests).
    bool loadFromDisk();
    void flushSoon();

private:
    void flushNow();
    void flusherLoop();

    std::filesystem::path root_;
    std::filesystem::path cacheDir_;
    std::filesystem::path indexFile_;

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
std::string normalizePath(std::filesystem::path const& path);
std::string workspaceKey(std::string const& normalizedRoot);
std::filesystem::path defaultCacheDir();
bool statFile(std::filesystem::path const& path, std::uint64_t* mtime, std::uint64_t* size);

}  // namespace fblang