#include "analysis_cache.h"

#include <algorithm>
#include <mutex>

#include "lexer.h"
#include "symbols.h"

namespace fblang {

namespace {
// Closed-file entries (fromBuffer=false) are FIFO-evicted past this cap. Open
// buffers are excluded: their entries are owned by the notification thread and
// live until didClose.
constexpr std::size_t kMaxClosedEntries = 128;
// FNV-1a 64-bit offset basis and prime, and the number of leading content
// bytes kept for the (size, head) hit-verify fingerprint.
constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::size_t kHeadBytes = 8;
constexpr std::size_t kBitsPerByte = 8;
} // namespace

std::uint64_t AnalysisCache::hashContent(std::string_view content) {
  // FNV-1a 64: a cheap content identity probe. Not cryptographic; the (size,
  // head) hit-verify catches the residual collision case by re-analyzing.
  std::uint64_t h = kFnvOffsetBasis;
  for (unsigned char const c : content) {
    h ^= c;
    h *= kFnvPrime;
  }
  return h;
}

std::shared_ptr<AnalysisCache::Entry const>
AnalysisCache::get(std::string const &path, std::string_view content,
                   std::uint64_t version, bool fromBuffer, bool insert) {
  std::uint64_t const hash = hashContent(content);
  std::uint64_t const size = content.size();
  std::uint64_t head = 0;
  for (std::size_t i = 0; i < kHeadBytes && i < content.size(); ++i) {
    head = (head << kBitsPerByte) | static_cast<unsigned char>(content[i]);
  }

  {
    std::lock_guard<std::mutex> const lock(mu_);
    auto const it = entries_.find(Key{path, hash});
    if (it != entries_.end()) {
      std::shared_ptr<Entry const> const &e = it->second;
      // Hit-verify beyond the hash: the entry's size and head must match the
      // bytes this call actually holds, so a residual hash collision (or a
      // mutated buffer read concurrently) re-analyzes instead of serving a
      // wrong analysis. Version is advisory only: byte identity decides.
      if (e->size == size && e->head == head) {
        ++stats_.hits;
        return e;
      }
    }
    // Count the miss while the lock is held: `stats_` is plain counters read
    // by stats(), so every mutation must be serialized.
    ++stats_.misses;
  }

  auto entry = std::make_shared<Entry>();
  entry->path = path;
  entry->fromBuffer = fromBuffer;
  entry->version = version;
  entry->size = size;
  entry->head = head;
  // `analyze`'s token stream borrows the buffer (lexer.h): copy the bytes into
  // the stable entry first so the returned shared_ptr pins valid `Token::data`
  // pointers for its whole lifetime.
  entry->content.assign(content);
  entry->analysis = analyze(entry->content);

  std::lock_guard<std::mutex> const lock(mu_);
  if (!insert) {
    // A request-path miss must not store: for an open buffer the didChange
    // fill owns the insert and lands microseconds later, so this analysis is
    // served once and dropped (self-healing, never authoritative here).
    return entry;
  }
  // Wholesale replace: every prior entry of this path (any content version) is
  // dead — the caller just established the path's current content. This bounds
  // open-buffer entries to one per path, since they are never FIFO-capped, and
  // keeps the closed-file FIFO queue free of superseded keys.
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->first.path == path) {
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
  closedOrder_.erase(
      std::remove_if(closedOrder_.begin(), closedOrder_.end(),
                     [&path](Key const &k) { return k.path == path; }),
      closedOrder_.end());
  entries_[Key{path, hash}] = entry;
  if (fromBuffer) {
    return entry;
  }
  closedOrder_.push_back(Key{path, hash});
  while (closedOrder_.size() > kMaxClosedEntries) {
    Key const oldest = closedOrder_.front();
    closedOrder_.pop_front();
    auto const it = entries_.find(oldest);
    // Guard: the slot may since have been re-published as an open-buffer entry
    // (fromBuffer=true), which is exempt from FIFO eviction.
    if (it != entries_.end() && !it->second->fromBuffer) {
      entries_.erase(it);
      ++stats_.evictions;
    }
  }
  return entry;
}

void AnalysisCache::removePath(std::string const &path) {
  std::lock_guard<std::mutex> const lock(mu_);
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->first.path != path) {
      ++it;
      continue;
    }
    it = entries_.erase(it);
  }
  closedOrder_.erase(
      std::remove_if(closedOrder_.begin(), closedOrder_.end(),
                     [&path](Key const &k) { return k.path == path; }),
      closedOrder_.end());
}

AnalysisCache::Stats AnalysisCache::stats() const {
  std::lock_guard<std::mutex> const lock(mu_);
  return stats_;
}

std::size_t AnalysisCache::size() const {
  std::lock_guard<std::mutex> const lock(mu_);
  return entries_.size();
}

} // namespace fblang