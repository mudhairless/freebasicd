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
#include "lexer.h"
#include "resolve.h"

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

#define CHECK_MSG(cond, msg)                                                                     \
    do                                                                                           \
    {                                                                                            \
        if (!(cond))                                                                             \
        {                                                                                        \
            std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #cond, msg);                \
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

static std::string readFileContent(fs::path const& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::shared_ptr<IndexedFile const> findFile(WorkspaceIndex const& idx, std::string const& norm)
{
    for (auto const& f : idx.snapshot())
    {
        if (f->path == norm)
        {
            return f;
        }
    }
    return nullptr;
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

    // Occurrence projection + moduleScope round-trip through the disk cache.
    {
        writeFile(ws / "occ.bas", "dim counter as integer\ncounter = counter + 1\n");
        {
            WorkspaceIndex occ(ws, cache);
            occ.open();
            occ.scan(false);
            occ.close();
        }
        WorkspaceIndex occ2(ws, cache);
        occ2.open();
        std::string const occNorm = normalizePath(ws / "occ.bas");
        auto const file = findFile(occ2, occNorm);
        CHECK(file != nullptr);
        bool sawDecl = false;
        bool sawSite = false;
        for (Symbol const& root : file->roots)
        {
            if (root.key != "counter")
            {
                continue;
            }
            sawDecl = root.moduleScope;
            sawSite = root.occurrences.size() == 2 &&
                      root.occurrences[0].moduleScope && root.occurrences[1].moduleScope;
        }
        CHECK_MSG(sawDecl, "a module-level dim round-trips with its moduleScope flag");
        CHECK_MSG(sawSite, "its module-level usages round-trip as occurrences");
        occ2.close();
    }

    // byKey + transitiveIncludes: diamond closure, cycle termination, and
    // round-trip of the projections through the disk cache.
    {
        writeFile(ws / "root2.bas",
                  "#include \"a.bi\"\n"
                  "#include \"b.bi\"\n"
                  "dim rootVal as integer\n");
        writeFile(ws / "a.bi", "#include \"c.bi\"\n");
        writeFile(ws / "b.bi", "#include \"c.bi\"\n");
        writeFile(ws / "c.bi", "dim sharedX as integer\n");
        writeFile(ws / "cyc1.bi", "#include \"cyc2.bi\"\n");
        writeFile(ws / "cyc2.bi", "#include \"cyc1.bi\"\n");
        {
            WorkspaceIndex closure(ws, cache);
            closure.open();
            closure.scan(false);

            std::string const root2Norm = normalizePath(ws / "root2.bas");
            std::string const aNorm = normalizePath(ws / "a.bi");
            std::string const bNorm = normalizePath(ws / "b.bi");
            std::string const cNorm = normalizePath(ws / "c.bi");
            std::string const cyc1Norm = normalizePath(ws / "cyc1.bi");
            std::string const cyc2Norm = normalizePath(ws / "cyc2.bi");

            // Diamond: root2 -> a -> c, root2 -> b -> c; c appears once.
            std::vector<std::string> const incs = closure.transitiveIncludes(root2Norm);
            CHECK(incs.size() == 3);
            CHECK(incs[0] == aNorm);
            CHECK(incs[1] == cNorm);
            CHECK(incs[2] == bNorm);

            // Inclusive cycle: a.bi <-> b.bi terminates with each side seen once.
            std::vector<std::string> const cyc = closure.transitiveIncludes(cyc1Norm);
            CHECK(cyc.size() == 1 && cyc[0] == cyc2Norm);
            CHECK(closure.transitiveIncludes(cyc2Norm).size() == 1 &&
                  closure.transitiveIncludes(cyc2Norm)[0] == cyc1Norm);

            // The shared declaration is reachable through the closure.
            std::vector<KeyedDecl> const decls = closure.byKey("sharedx");
            CHECK(decls.size() == 1);
            CHECK(decls[0].decl->name == "sharedX");
            CHECK(decls[0].decl->moduleScope);
            CHECK(decls[0].file->path == cNorm);
            CHECK_MSG(decls[0].file->includes.empty(), "c.bi includes nothing");

            // Include edges resolved to normalized absolute paths.
            auto const aFile = findFile(closure, aNorm);
            CHECK(aFile != nullptr && aFile->includes.size() == 1);
            CHECK(aFile->includes[0].literal == "c.bi");
            CHECK(aFile->includes[0].target == cNorm);
            CHECK(!aFile->includes[0].once);
            CHECK(closure.byKey("rootval").size() == 1);
            closure.close();
        }
        {
            // Projections survive a cold reload from the per-file cache.
            WorkspaceIndex reload(ws, cache);
            reload.open();
            std::string const root2Norm = normalizePath(ws / "root2.bas");
            std::string const cyc1Norm = normalizePath(ws / "cyc1.bi");
            CHECK(reload.byKey("sharedx").size() == 1);
            CHECK(reload.byKey("rootval").size() == 1);
            CHECK(reload.transitiveIncludes(root2Norm).size() == 3);
            CHECK(reload.transitiveIncludes(cyc1Norm).size() == 1);
            reload.close();
        }
    }

    // Open-buffer entries (persisted=false) never reach the disk cache and
    // never satisfy scan's mtime/size cache-hit.
    {
        writeFile(ws / "buf.bas", "dim counter as integer\ncounter = counter + 1\n");
        std::string const bufNorm = normalizePath(ws / "buf.bas");
        std::uint64_t mt = 0;
        std::uint64_t sz = 0;
        statFile(ws / "buf.bas", &mt, &sz);
        WorkspaceIndex live(ws, cache);
        live.open();
        live.scan(false);

        // A buffer diverges from disk; the upserted entry must not be written.
        AnalyzedDoc doc = analyze("dim ghost as string\nprint ghost\n");
        live.upsert(indexedFileFromAnalysis(bufNorm, mt, sz, std::move(doc), ws,
                                            /*persisted=*/false));
        live.close();
        {
            // The cache file still holds the on-disk scan truth (counter, not ghost).
            std::string const json = readFileContent(live.cachePathFor(bufNorm));
            CHECK(json.find("counter") != std::string::npos);
            CHECK(json.find("ghost") == std::string::npos);
            CHECK_MSG(json.find("\"version\":3") != std::string::npos, "cache is at v3");
        }
        {
            // After a cold reload the buffer entry is gone entirely.
            WorkspaceIndex cold(ws, cache);
            cold.open();
            auto const f = findFile(cold, bufNorm);
            CHECK(f != nullptr);
            bool sawCounter = false;
            for (Symbol const& root : f->roots)
            {
                sawCounter = sawCounter || root.key == "counter";
            }
            CHECK(sawCounter);
            cold.close();
        }
        {
            // In-memory persisted=false entry never shadows the scan
            // cache-hit: scan re-reads disk and replaces the buffer parse.
            WorkspaceIndex live2(ws, cache);
            live2.open();
            AnalyzedDoc doc2 = analyze("dim ghost as string\nprint ghost\n");
            live2.upsert(indexedFileFromAnalysis(bufNorm, mt, sz, std::move(doc2), ws,
                                                 /*persisted=*/false));
            live2.scan(false);
            auto const f = findFile(live2, bufNorm);
            CHECK(f != nullptr);
            bool sawGhost = false;
            bool sawCounter = false;
            for (Symbol const& root : f->roots)
            {
                sawGhost = sawGhost || root.key == "ghost";
                sawCounter = sawCounter || root.key == "counter";
            }
            CHECK(!sawGhost);
            CHECK(sawCounter);
            live2.close();
        }
    }

    // Pre-v3 (version 2) cache files are invalid: ignored on load, rebuilt by
    // scan, and the projections are back after the rebuild.
    {
        fs::path const wsV = sandbox / "verws";
        fs::create_directories(wsV);
        writeFile(wsV / "m.bas", "dim vv as integer\n");
        std::string const mNorm = normalizePath(wsV / "m.bas");
        WorkspaceIndex vBad(wsV, cache);
        std::ofstream out(vBad.cachePathFor(mNorm), std::ios::binary | std::ios::trunc);
        out << "{\"version\":2,\"path\":\"" << mNorm
            << "\",\"mtime\":0,\"size\":0,\"lang\":\"fb\",\"symbols\":[]}";
        WorkspaceIndex vGood(wsV, cache);
        vGood.open();
        CHECK_MSG(vGood.size() == 0, "a v2 cache file is not loaded");
        vGood.scan(false);
        vGood.close();
        WorkspaceIndex vRe(wsV, cache);
        vRe.open();
        CHECK(vRe.size() == 1);
        CHECK_MSG(vRe.byKey("vv").size() == 1, "the rebuilt v3 cache carries projections");
        vRe.close();
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