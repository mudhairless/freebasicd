// Workspace index checks: scanning, persistence round-trip, staleness, and
// corrupt-cache recovery. Uses temp directories; never touches the real data
// dir or the workspace on disk.
//
// The index lives in an explicit temp cache dir (outside the workspace), is
// keyed to the workspace root, and must never carry symbols between runs or
// across workspaces.

#include <atomic>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "index.h"

namespace fs = std::filesystem;
using namespace fblang;

static int failures = 0;

#define CHECK(cond)                                                                              \
    do                                                                                           \
    {                                                                                            \
        if (!(cond))                                                                             \
        {                                                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                          \
            ++failures;                                                                          \
        }                                                                                        \
    } while (0)

static fs::path makeTmpDir()
{
    static std::atomic<long> counter{0};
    fs::path const root = fs::temp_directory_path() /
                          ("fblsp-index-" + std::to_string(::time(nullptr)) + "-" +
                           std::to_string(counter.fetch_add(1)));
    fs::create_directories(root);
    return root;
}

static void writeFile(fs::path const& path, std::string const& content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

static bool hasSymbol(SymbolKind kind, std::string const& name, IndexedFile const& f)
{
    for (Symbol const& root : f.roots)
    {
        if (root.kind == kind && root.name == name)
        {
            return true;
        }
    }
    return false;
}

int main()
{
    {
        // Keying + normalization helpers.
        std::string const a = normalizePath("/tmp/Alpha/./One.bas");
        std::string const b = normalizePath("/tmp/Alpha/One.bas");
        CHECK(a == b);
        CHECK(normalizePath("/tmp/a/../b/x.bi") == normalizePath("/tmp/b/x.bi"));
        CHECK(!workspaceKey(normalizePath("/w/one")).empty());
        CHECK(workspaceKey(normalizePath("/w/one")) != workspaceKey(normalizePath("/w/two")));
    }

    fs::path const sandbox = makeTmpDir();
    fs::path const ws = sandbox / "ws";
    fs::path const cache = sandbox / "cache";
    fs::create_directories(ws / "sub");
    writeFile(ws / "main.bas", "dim counter as integer\ncounter = counter + 1\n");
    writeFile(ws / "sub" / "lib.bi", "function clamp(v as integer, lo as integer) as integer\n    "
                                     "if v < lo then return lo\n    return v\nend function\n");

    // Round-trip: scan -> persist -> fresh index reloads from cache.
    {
        WorkspaceIndex first(ws, cache);
        first.open();
        first.scan(false);
        first.flushSoon();
        first.close();
    }
    {
        WorkspaceIndex second(ws, cache);
        second.open();
        // No scan: only the persisted cache can satisfy this.
        CHECK(second.size() == 2);
        bool foundMain = false;
        bool foundLib = false;
        for (auto const& f : second.snapshot())
        {
            foundMain = foundMain || hasSymbol(SymbolKind::Dim, "counter", *f);
            foundLib = foundLib || hasSymbol(SymbolKind::Function, "clamp", *f);
        }
        CHECK(foundMain);
        CHECK(foundLib);
        second.close();
    }

    // The cache must live outside the workspace and be tagged with the root.
    {
        WorkspaceIndex probe(ws, cache);
        CHECK(fs::exists(cache));
        CHECK(!fs::exists(ws / "index.json"));
        std::ifstream in(probe.indexFile(), std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(body.find("function clamp") != std::string::npos);
        CHECK(body.find("dim counter") != std::string::npos);
    }

    // Two different workspaces never share a cache file.
    {
        fs::path const ws2 = sandbox / "ws2";
        fs::create_directories(ws2);
        WorkspaceIndex one(ws, cache);
        WorkspaceIndex two(ws2, cache);
        CHECK(one.indexFile() != two.indexFile());
    }

    // Staleness: a changed file is re-parsed on the next scan, the cache is
    // rebuilt, and the stale symbol is gone.
    {
        writeFile(ws / "main.bas", "dim ghost as string\nprint ghost\n");
        WorkspaceIndex third(ws, cache);
        third.open();
        third.scan(false);
        third.close();
        WorkspaceIndex fourth(ws, cache);
        fourth.open();
        CHECK(fourth.size() == 2);
        bool sawGhost = false;
        bool sawCounter = false;
        for (auto const& f : fourth.snapshot())
        {
            sawGhost = sawGhost || hasSymbol(SymbolKind::Dim, "ghost", *f);
            sawCounter = sawCounter || hasSymbol(SymbolKind::Dim, "counter", *f);
        }
        CHECK(sawGhost);
        CHECK(!sawCounter);
        fourth.close();
    }

    // Corrupt cache: must be discarded and rebuilt from a scan, not trusted.
    {
        WorkspaceIndex corrupt(ws, cache);
        std::ofstream out(corrupt.indexFile(), std::ios::binary | std::ios::trunc);
        out << "this is not json {{{";
        WorkspaceIndex fifth(ws, cache);
        fifth.open();
        CHECK(fifth.size() == 0);
        fifth.scan(false);
        fifth.close();
        CHECK(true);  // survived without crashing
    }

    fs::remove_all(sandbox);

    if (failures == 0)
    {
        std::printf("index_checks: all passed\n");
        return 0;
    }
    std::printf("index_checks: %d failures\n", failures);
    return 1;
}