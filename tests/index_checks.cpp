// Workspace index checks: SHA-256 keying, platform cache dir, per-source-file
// persistence, round-trip, staleness, and corrupt-cache recovery. Uses temp
// directories; never touches the real data dir or the workspace on disk.
//
// The cache lives in an explicit temp cache dir (outside the workspace). Each
// indexed source gets its own JSON file named by the SHA-256 of its normalized
// path, inside a per-workspace subdirectory keyed by the SHA-256 of the root;
// workspaces never share cache files.

#include <atomic>
#include <cstdio>
#include <cstdlib>
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

static bool isSha256Hex(std::string const& s)
{
    if (s.size() != 64)
    {
        return false;
    }
    return s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

static int countJsonFiles(fs::path const& dir)
{
    if (!fs::exists(dir))
    {
        return 0;
    }
    int n = 0;
    for (auto const& e : fs::directory_iterator(dir))
    {
        if (e.path().extension() == ".json")
        {
            ++n;
        }
    }
    return n;
}

int main()
{
    // SHA-256 known vectors (FIPS 180-2).
    CHECK(sha256Hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // Keying + normalization helpers.
    std::string const a = normalizePath("/tmp/Alpha/./One.bas");
    std::string const b = normalizePath("/tmp/Alpha/One.bas");
    CHECK(a == b);
    CHECK(normalizePath("/tmp/a/../b/x.bi") == normalizePath("/tmp/b/x.bi"));
    CHECK(isSha256Hex(workspaceKey(normalizePath("/w/one"))));
    CHECK(workspaceKey(normalizePath("/w/one")) != workspaceKey(normalizePath("/w/two")));

    // Platform index dir: <platform state>/freebasiclsp/index.
    {
        fs::path const dir = defaultCacheDir();
        CHECK(dir.filename() == fs::path("index"));
        CHECK(dir.parent_path().filename() == fs::path("freebasiclsp"));
#if !defined(_WIN32) && !defined(__APPLE__)
        if (std::getenv("XDG_STATE_HOME") == nullptr && std::getenv("HOME") != nullptr)
        {
            CHECK(dir == fs::path(std::getenv("HOME")) / ".local" / "state" / "freebasiclsp" /
                             "index");
        }
#endif
    }

    fs::path const sandbox = makeTmpDir();
    fs::path const ws = sandbox / "ws";
    fs::path const cache = sandbox / "cache";
    fs::create_directories(ws / "sub");
    writeFile(ws / "main.bas", "dim counter as integer\ncounter = counter + 1\n");
    writeFile(ws / "sub" / "lib.bi", "function clamp(v as integer, lo as integer) as integer\n    "
                                      "if v < lo then return lo\n    return v\nend function\n");
    fs::path const wsCache = cache / workspaceKey(normalizePath(ws));

    // Round-trip: scan -> persist -> fresh index reloads from per-file cache.
    {
        WorkspaceIndex first(ws, cache);
        first.open();
        first.scan(false);
        first.close();
        // One SHA-256-named JSON file per indexed source, inside the
        // workspace-keyed subdirectory, never in the workspace.
        CHECK(countJsonFiles(wsCache) == 2);
        CHECK(!fs::exists(ws / "index.json"));
        for (auto const& e : fs::directory_iterator(wsCache))
        {
            CHECK(e.path().extension() == ".json");
            CHECK(isSha256Hex(e.path().stem().string()));
        }
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

    // Cache location + per-file lookup: the filename is the SHA-256 of the
    // normalized source path, and the entry's own path is the real one.
    {
        WorkspaceIndex probe(ws, cache);
        std::string const mainNorm = normalizePath(ws / "main.bas");
        CHECK(probe.indexDir() == wsCache);
        CHECK(probe.cachePathFor(mainNorm).filename().stem() == sha256Hex(mainNorm));
        std::ifstream in(probe.cachePathFor(mainNorm), std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(body.find("dim counter") != std::string::npos);
    }

    // Two different workspaces never share a cache directory.
    {
        fs::path const ws2 = sandbox / "ws2";
        fs::create_directories(ws2);
        WorkspaceIndex one(ws, cache);
        WorkspaceIndex two(ws2, cache);
        CHECK(one.indexDir() != two.indexDir());
        CHECK(countJsonFiles(one.indexDir()) == 2);
        CHECK(countJsonFiles(two.indexDir()) == 0);
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

    // Corrupt cache: the single bad file is discarded, the rest load, and a
    // scan rebuilds the missing entry.
    {
        WorkspaceIndex corrupt(ws, cache);
        std::ofstream out(corrupt.cachePathFor(normalizePath(ws / "main.bas")),
                          std::ios::binary | std::ios::trunc);
        out << "this is not json {{{";
        WorkspaceIndex fifth(ws, cache);
        fifth.open();
        CHECK(fifth.size() == 1);  // lib.bi still trusted
        fifth.scan(false);
        fifth.close();
        WorkspaceIndex sixth(ws, cache);
        sixth.open();
        CHECK(sixth.size() == 2);  // rebuilt by scan
        sixth.close();
    }

    // remove() drops the entry and its cache file.
    {
        WorkspaceIndex probe(ws, cache);
        probe.open();
        std::string const libNorm = normalizePath(ws / "sub" / "lib.bi");
        CHECK(fs::exists(probe.cachePathFor(libNorm)));
        probe.remove(libNorm);
        CHECK(!fs::exists(probe.cachePathFor(libNorm)));
        CHECK(probe.size() == 1);
        probe.close();
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