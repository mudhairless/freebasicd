// Content-addressed analysis cache checks (M10): hit/miss discipline,
// freshness under content change, advisory-version semantics, borrow safety
// of the token stream, FIFO eviction of closed-file entries, and didClose
// removal. Byte-offset and LSP-agnostic.

#include <cstdio>
#include <string>

#include "analysis_cache.h"
#include "lexer.h"

using namespace fblang;

static int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

#define CHECK_MSG(cond, msg)                                                   \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #cond, msg);    \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static void TestIdenticalContentHits() {
  AnalysisCache cache;
  std::string const content = "dim total as integer\ntotal = 1\n";
  std::shared_ptr<AnalysisCache::Entry const> const a =
      cache.get("/a.bas", content, 1, /*fromBuffer=*/true, /*insert=*/true);
  std::shared_ptr<AnalysisCache::Entry const> const b =
      cache.get("/a.bas", content, 1, /*fromBuffer=*/true, /*insert=*/false);
  CHECK_MSG(a.get() == b.get(),
            "identical content must reuse the one analysis (pointer identity)");
  AnalysisCache::Stats const st = cache.stats();
  CHECK(st.misses == 1);
  CHECK(st.hits == 1);
  CHECK(cache.size() == 1);
}

static void TestContentChangeInvalidates() {
  AnalysisCache cache;
  std::shared_ptr<AnalysisCache::Entry const> const a =
      cache.get("/a.bas", "dim x as integer\n", 1, true, true);
  std::shared_ptr<AnalysisCache::Entry const> const b =
      cache.get("/a.bas", "dim y as integer\n", 2, true, true);
  CHECK_MSG(a.get() != b.get(), "a content change must analyze afresh");
  CHECK_MSG(b->analysis.parse.roots.size() == 1 &&
                b->analysis.parse.roots[0].key == "y",
            "the new entry must analyze the new bytes");
  AnalysisCache::Stats const st = cache.stats();
  CHECK(st.misses == 2);
}

static void TestPathDistinctness() {
  AnalysisCache cache;
  std::string const content = "dim x as integer\n";
  std::shared_ptr<AnalysisCache::Entry const> const a =
      cache.get("/a.bas", content, 1, true, true);
  std::shared_ptr<AnalysisCache::Entry const> const b =
      cache.get("/b.bas", content, 1, true, true);
  CHECK_MSG(a.get() != b.get(), "the same bytes under another path are a miss");
  CHECK(a->path == "/a.bas");
  CHECK(b->path == "/b.bas");
  CHECK(cache.size() == 2);
}

static void TestVersionIsAdvisoryOnly() {
  AnalysisCache cache;
  std::string const content = "dim x as integer\n";
  std::shared_ptr<AnalysisCache::Entry const> const v1 =
      cache.get("/a.bas", content, 1, true, true);
  // A version advance with byte-identical content is still the same content:
  // the entry is served (advisory version must never reject a content-valid
  // hit) and its recorded version is whatever filled it.
  std::shared_ptr<AnalysisCache::Entry const> const v2 =
      cache.get("/a.bas", content, 7, true, false);
  CHECK_MSG(v1.get() == v2.get(),
            "version is advisory; byte identity decides the hit");
  AnalysisCache::Stats const st = cache.stats();
  CHECK(st.hits == 1);
  CHECK(st.misses == 1);
}

static void TestOpenBufferMissDoesNotInsert() {
  AnalysisCache cache;
  std::string const content = "dim x as integer\n";
  std::shared_ptr<AnalysisCache::Entry const> const miss =
      cache.get("/a.bas", content, 1, /*fromBuffer=*/true, /*insert=*/false);
  CHECK_MSG(
      cache.size() == 0,
      "a request thread's open-buffer miss must not insert: the didChange "
      "fill owns open-buffer inserts");
  CHECK(miss->content == content);
  CHECK(miss->analysis.parse.roots.size() == 1);
  // The fill lands; the next read of the same bytes hits.
  std::shared_ptr<AnalysisCache::Entry const> const fill =
      cache.get("/a.bas", content, 1, true, true);
  CHECK(cache.size() == 1);
  std::shared_ptr<AnalysisCache::Entry const> const hit =
      cache.get("/a.bas", content, 1, false, false);
  CHECK_MSG(hit.get() == fill.get(),
            "a closed-file read of the identical bytes hits the fill");
}

static void TestEntryOwnsItsContent() {
  AnalysisCache cache;
  std::shared_ptr<AnalysisCache::Entry const> entry;
  {
    std::string const temp = "dim borrowed as integer\nborrowed = 2\n";
    entry = cache.get("/a.bas", temp, 1, true, true);
    // `temp` dies here; the entry must own its bytes.
  }
  bool sawBorrowed = false;
  for (Token const &t : entry->analysis.tokens) {
    if (t.kind == TokenKind::Identifier && t.text() == "borrowed") {
      sawBorrowed = true;
    }
  }
  CHECK_MSG(sawBorrowed,
            "the token stream must stay valid after the source buffer dies");
  CHECK(entry->content.find("borrowed") != std::string::npos);
}

static void TestFifoEvictionOfClosedEntries() {
  AnalysisCache cache;
  // 130 closed-file entries past the 128 cap: every insert is a miss, the
  // oldest two are evicted.
  for (int i = 0; i < 130; ++i) {
    std::string const path = "/d" + std::to_string(i) + ".bas";
    std::string const content = "dim var" + std::to_string(i) +
                                " as integer\nvar" + std::to_string(i) +
                                " = 1\n";
    (void)cache.get(path, content, 0, /*fromBuffer=*/false, /*insert=*/true);
  }
  AnalysisCache::Stats const st = cache.stats();
  CHECK_MSG(st.evictions == 2, "the two oldest closed entries are evicted");
  CHECK_MSG(st.misses == 130, "every closed-file insert is a miss");
  CHECK(cache.size() == 128);
  // A re-read of an evicted path analyzes again (miss) and self-warms.
  std::shared_ptr<AnalysisCache::Entry const> const again =
      cache.get("/d0.bas", "dim var0 as integer\nvar0 = 1\n", 0, false, true);
  CHECK(again->analysis.parse.roots.size() == 1);
  CHECK(cache.stats().misses == 131);
}

static void TestOpenBufferEntriesNeverFifoEvicted() {
  AnalysisCache cache;
  // 128 closed entries fill the cap, then an open-buffer entry inserts; it
  // must survive by policy (only closed entries count against the cap).
  for (int i = 0; i < 128; ++i) {
    std::string const path = "/e" + std::to_string(i) + ".bas";
    std::string const content = "dim v" + std::to_string(i) + " as integer\n";
    (void)cache.get(path, content, 0, false, true);
  }
  std::string const keep = "dim live as integer\n";
  std::shared_ptr<AnalysisCache::Entry const> const live =
      cache.get("/live.bas", keep, 3, true, true);
  std::shared_ptr<AnalysisCache::Entry const> const probe =
      cache.get("/live.bas", keep, 3, true, false);
  CHECK_MSG(live.get() == probe.get(), "the open-buffer entry is still served");
  CHECK(cache.size() == 129);
}

static void TestRemovePath() {
  AnalysisCache cache;
  std::string const content = "dim x as integer\n";
  (void)cache.get("/a.bas", content, 1, true, true);
  (void)cache.get("/b.bas", content, 1, false, true);
  CHECK(cache.size() == 2);
  cache.removePath("/a.bas");
  CHECK_MSG(cache.size() == 1,
            "didClose must drop every entry of the closed path");
  // A later open of the same path starts fresh (miss, new entry).
  std::shared_ptr<AnalysisCache::Entry const> const reopened =
      cache.get("/a.bas", content, 2, true, true);
  CHECK(reopened->version == 2);
  CHECK(cache.size() == 2);
  AnalysisCache::Stats const st = cache.stats();
  CHECK(st.misses == 3);
}

static void TestStatsAreExactUnderRepeatReads() {
  AnalysisCache cache;
  std::string const content = "dim x as integer\nx = x + 1\n";
  (void)cache.get("/a.bas", content, 1, true, true);
  for (int i = 0; i < 5; ++i) {
    (void)cache.get("/a.bas", content, 1, true, false);
  }
  AnalysisCache::Stats const st = cache.stats();
  CHECK(st.misses == 1);
  CHECK(st.hits == 5);
}

int main() {
  TestIdenticalContentHits();
  TestContentChangeInvalidates();
  TestPathDistinctness();
  TestVersionIsAdvisoryOnly();
  TestOpenBufferMissDoesNotInsert();
  TestEntryOwnsItsContent();
  TestFifoEvictionOfClosedEntries();
  TestOpenBufferEntriesNeverFifoEvicted();
  TestRemovePath();
  TestStatsAreExactUnderRepeatReads();

  if (failures != 0) {
    std::printf("analysis_cache_checks: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("analysis_cache_checks: all checks passed\n");
  return 0;
}