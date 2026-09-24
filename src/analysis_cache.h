/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "resolve.h"

namespace fblang {

// Session-owned memo of per-file analysis keyed by content identity, so the
// request path (14 handlers) serves the one analysis per (path, content)
// instead of re-lexing/re-parsing the same buffer per request, and the
// cross-file path (references/rename closures) reuses the analysis a file was
// already read for.
//
// The key's freshness half is the FNV-1a-64 content hash plus a (size, head)
// hit-verify. A request racing a didChange can therefore only ever address the
// entry whose bytes it actually holds: the content-addressed key makes
// staleness structurally impossible rather than scheduled away. The
// client-supplied WorkingFiles version (which does not advance on versionless
// changes) is advisory only, recorded per entry for inspection; it never gates
// a hit, because byte identity already decides.
//
// Every entry owns its content, so `analysis`'s token stream — `Token::data`
// borrows the analyzed buffer (lexer.h) — stays valid for the shared_ptr's
// lifetime. Entries are immutable once published; updates replace the
// shared_ptr wholesale. Responses build from a pinned snapshot, lock-free.
//
// Writers: `reparseAndPublish` (the notification FIFO thread) inserts
// open-buffer entries (fromBuffer=true, never FIFO-evicted); request threads
// may insert closed-file entries on first read (fromBuffer=false, FIFO-evicted
// past kMaxClosedEntries). A request that misses on an open buffer analyzes
// locally without inserting — the didChange fill lands microseconds later.
class AnalysisCache {
public:
  struct Entry : DocumentContent {
    std::string path;        // normalized absolute path
    bool fromBuffer = false; // open-buffer live truth; never FIFO-evicted
    std::uint64_t version = 0;
    std::uint64_t size = 0; // content bytes, hit-verify beyond the hash
    std::uint64_t head = 0; // first 8 content bytes, hit-verify
  };

  // Content-addressed analysis of `content` for `path`. `content` is copied
  // into the returned immutable entry; on a miss `analyze` runs outside the
  // lock. `fromBuffer` selects the eviction class. `insert` is false on the
  // request path for open buffers (the didChange fill owns those inserts);
  // closed-file reads and reparseAndPublish pass true.
  std::shared_ptr<Entry const> get(std::string const &path,
                                   std::string_view content,
                                   std::uint64_t version, bool fromBuffer,
                                   bool insert);

  // Drop every entry of `path` (didClose): the buffer no longer holds the
  // file, and a future open should start fresh.
  void removePath(std::string const &path);

  struct Stats {
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t evictions = 0;
  };
  Stats stats() const;
  std::size_t size() const;

private:
  struct Key {
    std::string path;
    std::uint64_t hash = 0;
    bool operator<(Key const &other) const {
      return path < other.path || (path == other.path && hash < other.hash);
    }
  };

  static std::uint64_t hashContent(std::string_view content);

  mutable std::mutex mu_;
  std::map<Key, std::shared_ptr<Entry const>> entries_;
  std::deque<Key> closedOrder_; // FIFO order of fromBuffer=false entries
  Stats stats_;
};

} // namespace fblang