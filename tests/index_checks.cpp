// Workspace index checks: in-memory scan/upsert/remove, projections (byKey +
// transitiveIncludes), open-buffer isolation, workspace scoping, and the
// legacy-disk-index cleanup. Uses temp directories; never touches the real
// data dir or the workspace on disk.
//
// The index is purely in memory — no symbols or index state are ever written
// to disk, and the only disk the legacy cleanup touches is the platform index
// dir an older build left behind.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

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
    // Normalization helpers.
    std::string const a = normalizePath("/tmp/Alpha/./One.bas");
    std::string const b = normalizePath("/tmp/Alpha/One.bas");
    CHECK(a == b);
    CHECK(normalizePath("/tmp/a/../b/x.bi") == normalizePath("/tmp/b/x.bi"));

    // cleanupLegacyDiskIndex removes the platform index dir the old disk cache
    // lived in. The env var the platform dir is derived from is pointed at a
    // temp root so the real home/state is never touched.
    {
        fs::path const legacyRoot = fs::temp_directory_path() /
                                    ("fblsp-legacy-" + std::to_string(::time(nullptr)));
        auto const redirectOnce = [&](char const* env, fs::path const& base,
                                      fs::path const& legacy) {
            std::string const saved = std::getenv(env) ? std::getenv(env) : "";
            fs::create_directories(legacy / "somews");
            writeFile(legacy / "somews" / "stale.json", "{}");
#ifdef _WIN32
            _putenv_s(env, base.string().c_str());
#else
            setenv(env, base.string().c_str(), 1);
#endif
            cleanupLegacyDiskIndex();
#ifdef _WIN32
            if (saved.empty())
            {
                _putenv_s(env, "");
            }
            else
            {
                _putenv_s(env, saved.c_str());
            }
#else
            if (saved.empty())
            {
                unsetenv(env);
            }
            else
            {
                setenv(env, saved.c_str(), 1);
            }
#endif
            CHECK_MSG(!fs::exists(legacy),
                      "cleanupLegacyDiskIndex must remove the legacy index dir");
        };
#ifdef _WIN32
        redirectOnce("LOCALAPPDATA", legacyRoot, legacyRoot / "freebasiclsp" / "index");
#elif defined(__APPLE__)
        redirectOnce("HOME", legacyRoot,
                     legacyRoot / "Library" / "Application Support" / "freebasiclsp" / "index");
#else
        redirectOnce("XDG_STATE_HOME", legacyRoot, legacyRoot / "freebasiclsp" / "index");
#endif
        fs::remove_all(legacyRoot);
    }

    fs::path const sandbox = makeTmpDir();
    fs::path const ws = sandbox / "ws";
    fs::create_directories(ws / "sub");
    writeFile(ws / "main.bas", "dim counter as integer\ncounter = counter + 1\n");
    writeFile(ws / "sub" / "lib.bi", "function clamp(v as integer, lo as integer) as integer\n    "
                                      "if v < lo then return lo\n    return v\nend function\n");

    // A scan indexes every workspace source and answers queries from memory.
    {
        WorkspaceIndex index(ws);
        index.open();
        index.scan(false);
        CHECK(index.size() == 2);
        bool foundMain = false;
        bool foundLib = false;
        for (auto const& f : index.snapshot())
        {
            foundMain = foundMain || hasSymbol(SymbolKind::Dim, "counter", *f);
            foundLib = foundLib || hasSymbol(SymbolKind::Function, "clamp", *f);
        }
        CHECK(foundMain);
        CHECK(foundLib);
        CHECK(index.byKey("counter").size() == 1);
        index.close();
    }

    // Staleness: a changed file is re-parsed on the next scan, and the stale
    // symbol is gone.
    {
        writeFile(ws / "main.bas", "dim ghost as string\nprint ghost\n");
        WorkspaceIndex third(ws);
        third.open();
        third.scan(false);
        bool sawGhost = false;
        bool sawCounter = false;
        for (auto const& f : third.snapshot())
        {
            sawGhost = sawGhost || hasSymbol(SymbolKind::Dim, "ghost", *f);
            sawCounter = sawCounter || hasSymbol(SymbolKind::Dim, "counter", *f);
        }
        CHECK(sawGhost);
        CHECK(!sawCounter);
        third.close();
    }

    // remove() drops the entry from the index.
    {
        WorkspaceIndex probe(ws);
        probe.open();
        probe.scan(false);
        std::string const libNorm = normalizePath(ws / "sub" / "lib.bi");
        CHECK(probe.fileAt(libNorm) != nullptr);
        probe.remove(libNorm);
        CHECK(probe.size() == 1);
        CHECK(probe.fileAt(libNorm) == nullptr);
        probe.close();
    }

    // Occurrence projection + moduleScope flags live in memory.
    {
        writeFile(ws / "occ.bas", "dim counter as integer\ncounter = counter + 1\n");
        WorkspaceIndex occ(ws);
        occ.open();
        occ.scan(false);
        std::string const occNorm = normalizePath(ws / "occ.bas");
        auto const file = findFile(occ, occNorm);
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
        CHECK_MSG(sawDecl, "a module-level dim carries its moduleScope flag");
        CHECK_MSG(sawSite, "its module-level usages are projected as occurrences");
        occ.close();
    }

    // The §12.2 storage tag: `dim shared` and plain `dim` carry distinct
    // `shared` flags, so cross-file resolution can gate the plain one from
    // inside a block.
    {
        writeFile(ws / "shared.bi", "dim shared sharedFlag as integer\ndim plainP as integer\n");
        WorkspaceIndex sf(ws);
        sf.open();
        sf.scan(false);
        std::string const sharedNorm = normalizePath(ws / "shared.bi");
        auto const file = findFile(sf, sharedNorm);
        CHECK_MSG(file != nullptr, "the shared/plain fixture is indexed");
        bool sawShared = false;
        bool sawPlain = false;
        for (Symbol const& root : file->roots)
        {
            if (root.key == "sharedflag")
            {
                sawShared = root.shared;
            }
            if (root.key == "plainp")
            {
                sawPlain = !root.shared;
            }
        }
        CHECK_MSG(sawShared, "dim shared must carry shared=true");
        CHECK_MSG(sawPlain, "plain dim must carry shared=false");
        sf.close();
    }

    // byKey + transitiveIncludes: diamond closure, cycle termination, and the
    // projections stay correct across upserts.
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
        WorkspaceIndex closure(ws);
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

        // An upserted file replaces its own projections, leaving the others.
        writeFile(ws / "a.bi", "#include \"c.bi\"\nsub changedA()\nend sub\n");
        closure.scan(false);
        CHECK(closure.byKey("changeda").size() == 1);
        CHECK(closure.byKey("sharedx").size() == 1);
        closure.close();
    }

    // Open-buffer entries (fromDisk=false) never satisfy scan's mtime/size
    // cache-hit: scan re-reads disk and replaces the buffer parse.
    {
        writeFile(ws / "buf.bas", "dim counter as integer\ncounter = counter + 1\n");
        std::string const bufNorm = normalizePath(ws / "buf.bas");
        std::uint64_t mt = 0;
        std::uint64_t sz = 0;
        statFile(ws / "buf.bas", &mt, &sz);

        WorkspaceIndex live(ws);
        live.open();
        live.scan(false);

        // A buffer diverges from disk; the upserted entry must not shadow what
        // scan sees.
        AnalyzedDoc doc = analyze("dim ghost as string\nprint ghost\n");
        live.upsert(indexedFileFromAnalysis(bufNorm, mt, sz, std::move(doc), ws,
                                            /*fromDisk=*/false));
        {
            auto const f = live.fileAt(bufNorm);
            bool sawGhost = false;
            for (Symbol const& root : f->roots)
            {
                sawGhost = sawGhost || root.key == "ghost";
            }
            CHECK_MSG(sawGhost, "the live buffer parse is served to queries");
        }
        // The fromDisk=false entry never satisfies the in-memory cache-hit:
        // scan re-reads disk and replaces the buffer parse.
        live.scan(false);
        auto const f = live.fileAt(bufNorm);
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
        live.close();
    }

    // watchedFilesChanged() converges an external edit without any session
    // involvement: the debounced rescan picks up a rewritten header.
    {
        fs::path const wsW = sandbox / "watched";
        fs::create_directories(wsW);
        writeFile(wsW / "lib.bi", "sub greet()\nend sub\n");
        WorkspaceIndex w(wsW);
        w.open();
        w.scan(true);
        {
            int until = 200;
            while (until-- > 0 && w.byKey("greet").size() != 1)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            CHECK_MSG(w.byKey("greet").size() == 1, "the initial async scan must index lib.bi");
        }
        writeFile(wsW / "lib.bi", "sub greet()\nend sub\nsub farewell()\nend sub\n");
        w.watchedFilesChanged();
        {
            int until = 200;
            while (until-- > 0 && w.byKey("farewell").size() != 1)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            CHECK_MSG(w.byKey("farewell").size() == 1,
                      "a watched-files event must converge the new symbol via rescan");
            CHECK(w.byKey("greet").size() == 1);
        }
        w.close();
    }

    // Workspace scoping: the index only ever holds files under its root. A
    // scan indexes only the workspace, and an open-buffer upsert of an outside
    // file is dropped entirely.
    {
        fs::path const wsS = sandbox / "scopedws";
        fs::path const outDir = sandbox / "scoped-outside";
        fs::create_directories(wsS);
        fs::create_directories(outDir);
        writeFile(wsS / "in.bas", "dim inside as integer\n");
        writeFile(outDir / "rogue.bi", "dim rogue as integer\n");

        std::string const inNorm = normalizePath(wsS / "in.bas");
        std::string const rogueNorm = normalizePath(outDir / "rogue.bi");

        WorkspaceIndex a(wsS);
        a.open();
        a.scan(false);
        CHECK_MSG(findFile(a, inNorm) != nullptr, "a workspace file is indexed");
        CHECK_MSG(a.size() == 1, "the scan indexes exactly the workspace file");

        // An open-buffer upsert of an outside file is dropped entirely.
        std::uint64_t mt = 0;
        std::uint64_t sz = 0;
        statFile(outDir / "rogue.bi", &mt, &sz);
        AnalyzedDoc doc = analyze("dim rogue as integer\n");
        a.upsert(indexedFileFromAnalysis(rogueNorm, mt, sz, std::move(doc), wsS,
                                         /*fromDisk=*/false));
        CHECK_MSG(a.size() == 1, "upsert outside the root is dropped");
        CHECK_MSG(findFile(a, rogueNorm) == nullptr, "the outside path is not indexed");
        CHECK_MSG(a.byKey("rogue").empty(), "outside declarations are not projected");
        a.close();
    }

    // Include search: workspace child dirs (inc/include/src, etc.) and the
    // FreeBASIC installation's system header folder are searched in fbc's
    // relative order before a file is declared missing.
    {
        // Platform layout of the system header folder next to an fbc install.
#ifdef _WIN32
        CHECK(systemIncludeDirForExecutable("C:\\FreeBASIC") == fs::path("C:\\FreeBASIC\\inc"));
        CHECK(systemIncludeDirForExecutable("C:\\FreeBASIC\\bin") ==
              fs::path("C:\\FreeBASIC\\bin\\inc"));
#else
        CHECK(systemIncludeDirForExecutable("/usr/bin") == fs::path("/usr/include/freebasic"));
        CHECK(systemIncludeDirForExecutable("/opt/fb/bin") == fs::path("/opt/fb/include/freebasic"));
#endif

        // findFbcExecutableDir walks PATH and requires an executable `fbc`:
        // a non-executable decoy in an earlier entry is skipped.
        {
            fs::path const bin0 = sandbox / "fbcbin0";
            fs::path const bin1 = sandbox / "fbcbin1";
            fs::create_directories(bin0);
            fs::create_directories(bin1);
            std::string const exeName =
#ifdef _WIN32
                "fbc.exe";
#else
                "fbc";
#endif
            {
                std::ofstream exe(bin1 / exeName);
                exe << "#!/bin/sh\n";
            }
#ifndef _WIN32
            // The decoy has no execute bits; it must be skipped.
            std::ofstream decoy(bin0 / "fbc");
            decoy << "#!/bin/sh\n";
            std::filesystem::permissions(bin1 / "fbc",
                                         std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_write |
                                             std::filesystem::perms::owner_exec,
                                         std::filesystem::perm_options::replace);
#endif
            char const sep =
#ifdef _WIN32
                ';';
#else
                ':';
#endif
            std::string const path = bin0.string() + sep + bin1.string();
            std::optional<fs::path> const found = findFbcExecutableDir(path);
            CHECK_MSG(found.has_value(), "a PATH with an executable fbc must be found");
            CHECK_MSG(found && found->filename() == fs::path("fbcbin1"),
                      "the non-executable fbc decoy must be skipped");
            CHECK(!findFbcExecutableDir(bin0.string()).has_value());
            CHECK(!findFbcExecutableDir("").has_value());
        }

        // resolveIncludeTarget precedence with a sandboxed "system" header dir.
        {
            fs::path const incWs = sandbox / "incws";
            fs::create_directories(incWs);
            fs::create_directories(incWs / "inc" / "pkg");
            writeFile(incWs / "inc" / "pkg" / "api.bi", "dim apiVal as integer\n");
            writeFile(incWs / "ownpv.bi", "dim ownVal as integer\n");
            writeFile(incWs / "main.bas", "#include \"pkg/api.bi\"\n");

            fs::path const sysInc = sandbox / "sysinc";
            fs::create_directories(sysInc);
            writeFile(sysInc / "sysonly.bi", "dim sysVal as integer\n");

            std::string const mainNorm = normalizePath(incWs / "main.bas");
            std::string const ownAbs = normalizePath(incWs / "ownpv.bi");

            // A child dir of the workspace root answers `folder/file.bi`.
            std::optional<std::string> resolved =
                resolveIncludeTarget("pkg/api.bi", mainNorm, incWs, sysInc);
            CHECK_MSG(resolved && *resolved == normalizePath(incWs / "inc" / "pkg" / "api.bi"),
                      "a workspace child dir must satisfy a sub-folder include");

            // The including file's own directory wins over a child dir.
            writeFile(incWs / "inc" / "ownpv.bi", "dim childOwnVal as integer\n");
            resolved = resolveIncludeTarget("ownpv.bi", mainNorm, incWs, sysInc);
            CHECK_MSG(resolved && *resolved == ownAbs,
                      "the file's own directory must win over a child dir");

            // The workspace root itself wins over a child dir.
            fs::create_directories(incWs / "pkg");
            writeFile(incWs / "pkg" / "api.bi", "dim rootApiVal as integer\n");
            resolved = resolveIncludeTarget("pkg/api.bi", mainNorm, incWs, sysInc);
            CHECK_MSG(resolved && *resolved == normalizePath(incWs / "pkg" / "api.bi"),
                      "the workspace root must win over a child dir");

            // The system folder is searched last, only for a workspace miss.
            resolved = resolveIncludeTarget("sysonly.bi", mainNorm, incWs, sysInc);
            CHECK_MSG(resolved && *resolved == normalizePath(sysInc / "sysonly.bi"),
                      "the system header folder must satisfy a workspace miss");

            // A blank system dir disables the system search; a true miss stays
            // unresolved (the include-not-found diagnostic source).
            resolved = resolveIncludeTarget("sysonly.bi", mainNorm, incWs, fs::path{});
            CHECK_MSG(!resolved.has_value(), "skipping the system dir must leave it unresolved");
            resolved = resolveIncludeTarget("nope.bi", mainNorm, incWs, sysInc);
            CHECK_MSG(!resolved.has_value(), "a total miss must stay unresolved");
        }

        // Scan-level: a child-dir include resolves to the child's path on the
        // edge (no phantom include-not-found for a resolvable header).
        {
            fs::path const incWs2 = sandbox / "incws2";
            fs::create_directories(incWs2);
            fs::create_directories(incWs2 / "inc" / "pkg");
            writeFile(incWs2 / "main.bas", "#include \"pkg/api.bi\"\ndim mainVal as integer\n");
            writeFile(incWs2 / "inc" / "pkg" / "api.bi", "dim apiVal as integer\n");
            std::string const mainNorm2 = normalizePath(incWs2 / "main.bas");
            WorkspaceIndex scan(incWs2);
            scan.open();
            scan.scan(false);
            auto const f = findFile(scan, mainNorm2);
            CHECK(f != nullptr && f->includes.size() == 1);
            CHECK_MSG(f->includes[0].target == normalizePath(incWs2 / "inc" / "pkg" / "api.bi"),
                      "a scan must resolve the include through the workspace child dir");
            scan.close();
        }
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