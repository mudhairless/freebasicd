/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// M11 workspace roots and workspace/symbol.
//
// Root selection from a client root: the VCS and source/include-layout walks,
// the config-file marker, the home-folder guard, and the single-file mode —
// each asserted through a workspace/symbol query that only the right root can
// answer.
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

using namespace fbtest;

namespace {
// Files outside the workspace root must never be indexed: opening one and
// querying workspace/symbol must not surface its symbols.
void TestOutsideFileNotIndexed() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const wsDir = sandbox / "ws";
  std::filesystem::path const outsideDir = sandbox / "outside";
  std::filesystem::create_directories(wsDir);
  std::filesystem::create_directories(outsideDir);
  {
    std::ofstream out(wsDir / "main.bas");
    out << "dim mainVal as integer\n";
    std::ofstream out2(outsideDir / "dep.bi");
    out2 << "sub outsideFunc()\nend sub\n";
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const rootUri = FileUri(wsDir);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      rootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  // Open a header living outside the workspace root.
  std::string const outsideUri = FileUri(outsideDir / "dep.bi");
  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      outsideUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString("sub outsideFunc()\nend sub\n") + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"ext" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"ext)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  // Give open+scan time to settle; the outside file's symbol must never
  // appear in workspace/symbol.
  bool sawOutside = false;
  for (int n = 0; n < 40 && !sawOutside; ++n) {
    sawOutside =
        querySymbol(n, "outsideFunc").find("\"name\":\"outsideFunc\"") !=
        std::string::npos;
  }
  Expect(!sawOutside,
         "workspace/symbol must not return symbols from outside the root");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// The reported Kate regression: a document opened from a sibling project
// OUTSIDE the workspace root (the client root is this project, the file lives
// beside it, headers in the sibling's `inc` dir). Hovering a member access
// must resolve through the requesting file's on-demand #include closure — not
// fall back to the enclosing sub — while workspace/symbol stays strictly
// workspace-scoped (the out-of-root sub must not surface).
void TestHoverWorksForDocOutsideWorkspaceRoot() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const wsDir = sandbox / "ws";
  std::filesystem::create_directories(wsDir / ".git"); // project marker
  std::filesystem::path const projDir = sandbox / "proj";
  std::filesystem::create_directories(projDir / "src");
  std::filesystem::create_directories(projDir / "inc");
  {
    std::ofstream out(projDir / "inc" / "world.bi");
    out << "type Wall\n"
           "    as integer v1, v2\n"
           "end type\n"
           "type Map\n"
           "    as Wall walls(10)\n"
           "end type\n";
  }
  char const *kEngineText = "#include once \"world.bi\"\n"
                            "\n"
                            "sub runPhysics(map as Map, secIndex as integer)\n"
                            "    with map\n"
                            "        dim as Wall w = .walls(secIndex)\n"
                            "    end with\n"
                            "end sub\n";
  {
    std::ofstream out2(projDir / "src" / "engine.bas");
    out2 << kEngineText;
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const rootUri = FileUri(wsDir);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      rootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  // Open the out-of-root engine.bas and hover `.walls` (line 4, char 26).
  std::string const engineUri = FileUri(projDir / "src" / "engine.bas");
  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      engineUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString(kEngineText) + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));

  std::string const hoverFrame =
      R"({"jsonrpc":"2.0","id":"hxw","method":"textDocument/hover","params":)"
      R"({"textDocument":{"uri":")" +
      engineUri + R"("},"position":{"line":4,"character":26}}})";
  input->append(MakeLspFrame(hoverFrame.c_str()));
  std::string const hover = WaitForOutputContaining(output, "\"id\":\"hxw\"");
  Expect(hover.find("\"id\":\"hxw\"") != std::string::npos,
         "the out-of-root hover request must receive a response");
  Expect(hover.find("as Wall walls(10)") != std::string::npos,
         "an out-of-root member hover resolves the field declaration");
  Expect(hover.find("Member of `map` (`Map`).") != std::string::npos,
         "the with-implicit member names the variable and its type");
  Expect(hover.find("sub runPhysics(") == std::string::npos,
         "member hover must never fall back to the enclosing sub");

  // The out-of-root sub must stay out of workspace/symbol.
  bool sawOutside = false;
  auto querySymbol = [&](int n) {
    std::string const id = "\"id\":\"ext" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"ext)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":"runPhysics"}})";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };
  for (int n = 0; n < 40 && !sawOutside; ++n) {
    sawOutside =
        querySymbol(n).find("\"name\":\"runPhysics\"") != std::string::npos;
  }
  Expect(!sawOutside,
         "workspace/symbol must not surface symbols from outside the root");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// The source-layout counterpart of the VCS broad-root narrowing: the broad
// client root carries no version-control marker anywhere, so the opened
// document's project is recognized by its `src` child instead of a .git
// directory. The index root becomes the project dir (`inner`, from the
// `/tmp/test/inner/src/file.bas` example), so a sibling dir of `src` inside
// the project is indexed too, while an unrelated sibling tree under the broad
// root never is.
void TestSourceLayoutRootNarrowsToOpenedProject() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const broad = sandbox / "broad";
  std::filesystem::path const proj = broad / "inner";
  std::filesystem::path const sibling = broad / "sibling";
  std::filesystem::create_directories(proj / "src");
  std::filesystem::create_directories(proj / "data");
  std::filesystem::create_directories(sibling);
  {
    std::ofstream out(proj / "src" / "app.bas");
    out << "sub wsOnly()\nend sub\n";
    std::ofstream out2(proj / "data" / "helper.bi");
    out2 << "sub helperOnly()\nend sub\n";
    std::ofstream out3(sibling / "other.bas");
    out3 << "sub siblingOnly()\nend sub\n";
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const appUri = FileUri(proj / "src" / "app.bas");
  std::string const broadRootUri = FileUri(broad);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      broadRootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      appUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString("sub wsOnly()\nend sub\n") + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"layout" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"layout)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  // The opened document's project is indexed...
  bool foundWs = false;
  for (int n = 0; n < 60 && !foundWs; ++n) {
    foundWs = querySymbol(n, "wsOnly").find("\"name\":\"wsOnly\"") !=
              std::string::npos;
  }
  Expect(foundWs, "the opened document's project must be indexed");

  // ...with the project dir (`inner`), not the `src` child, as root: a sibling
  // dir inside the project is indexed too.
  bool foundHelper = false;
  for (int n = 0; n < 40 && !foundHelper; ++n) {
    foundHelper =
        querySymbol(100 + n, "helperOnly").find("\"name\":\"helperOnly\"") !=
        std::string::npos;
  }
  Expect(foundHelper,
         "the layout root must be the project dir, not its src child");

  // ...and the sibling tree under the broad root must never be.
  bool sawSibling = false;
  for (int n = 0; n < 40 && !sawSibling; ++n) {
    sawSibling =
        querySymbol(200 + n, "siblingOnly").find("\"name\":\"siblingOnly\"") !=
        std::string::npos;
  }
  Expect(!sawSibling,
         "a sibling project under a broad root must not be indexed");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// The source-layout detection must recognize every catalogued directory name,
// source *and* include, full and abbreviated — not just `src`: each project
// below the broad root is laid out with a different translated name, opening a
// document in it must narrow the index scope to that project, and the marker-
// less sibling tree must stay out.
void TestSourceLayoutRootRecognizesCatalogNames() {
  struct LayoutCase {
    char const *dir;
    char const *symbol;
  };
  static constexpr LayoutCase const cases[] = {
      {"src", "fromSrc"},         {"source", "fromSource"},
      {"fuente", "fromFuente"},   {"zdr", "fromZdr"},
      {"ein", "fromEin"},         {"inc", "fromInc"},
      {"include", "fromInclude"}, {"incl", "fromIncl"},
      {"inkl", "fromInkl"},       {"sumber", "fromSumber"},
  };

  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const broad = sandbox / "broad";
  std::filesystem::path const sibling = broad / "sibling";
  std::filesystem::create_directories(sibling);
  {
    std::ofstream out2(sibling / "other.bas");
    out2 << "sub siblingOnly()\nend sub\n";
  }
  std::vector<std::string> appUris;
  for (std::size_t i = 0; i < std::size(cases); ++i) {
    std::filesystem::path const proj = broad / ("proj" + std::to_string(i));
    std::filesystem::create_directories(proj / cases[i].dir);
    std::ofstream out(proj / cases[i].dir / "app.bas");
    out << "sub " << cases[i].symbol << "()\nend sub\n";
    appUris.push_back(FileUri(proj / cases[i].dir / "app.bas"));
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const broadRootUri = FileUri(broad);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      broadRootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"cat" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"cat)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  int idx = 0;
  bool sawSibling = false;
  for (std::size_t i = 0; i < std::size(cases); ++i) {
    std::string const openFrame =
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
        R"({"uri":")" +
        appUris[i] + R"(","languageId":"basic","version":1,"text":")" +
        ToJsonString(std::string("sub ") + cases[i].symbol + "()\nend sub\n") +
        "\"}}}";
    input->append(MakeLspFrame(openFrame.c_str()));

    bool found = false;
    for (int n = 0; n < 60 && !found; ++n) {
      found = querySymbol(idx++, cases[i].symbol)
                  .find(std::string("\"name\":\"") + cases[i].symbol + "\"") !=
              std::string::npos;
    }
    Expect(found, (std::string("project laid out as `") + cases[i].dir +
                   "` must narrow the index scope")
                      .c_str());
    for (int n = 0; !sawSibling && n < 25; ++n) {
      sawSibling =
          querySymbol(idx++, "siblingOnly").find("\"name\":\"siblingOnly\"") !=
          std::string::npos;
    }
  }
  Expect(!sawSibling,
         "a sibling project under a broad root must not be indexed");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// Single-file mode (no client root): the source/include layout walk still
// names the project. Opening `/tmp/test/inner/src/file.bas` roots at
// `/tmp/test/inner`, so a sibling dir of `src` inside the project is indexed
// and a tree beside it is not. Without any layout marker the workspace stays
// the opened file's own directory (pre-existing behavior).
void TestSourceLayoutRootSingleFileMode() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const proj = sandbox / "lone" / "inner";
  std::filesystem::create_directories(proj / "src");
  std::filesystem::create_directories(proj / "data");
  std::filesystem::create_directories(sandbox / "lone" / "step");
  std::filesystem::create_directories(sandbox / "plain");
  std::filesystem::create_directories(sandbox / "park");
  {
    std::ofstream out(proj / "src" / "app.bas");
    out << "sub wsOnly()\nend sub\n";
    std::ofstream out2(proj / "data" / "mod.bas");
    out2 << "sub dataOnly()\nend sub\n";
    std::ofstream out3(sandbox / "lone" / "step" / "x.bas");
    out3 << "sub stepOnly()\nend sub\n";
    std::ofstream out4(sandbox / "plain" / "file.bas");
    out4 << "sub fileOnly()\nend sub\n";
    std::ofstream out5(sandbox / "plain" / "near.bas");
    out5 << "sub nearOnly()\nend sub\n";
    std::ofstream out6(sandbox / "park" / "y.bas");
    out6 << "sub parkOnly()\nend sub\n";
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  // No rootUri: single-file mode.
  input->append(MakeLspFrame(kInitializeFrame));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  std::string const appUri = FileUri(proj / "src" / "app.bas");
  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      appUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString("sub wsOnly()\nend sub\n") + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"single" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"single)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  bool foundWs = false;
  for (int n = 0; n < 60 && !foundWs; ++n) {
    foundWs = querySymbol(n, "wsOnly").find("\"name\":\"wsOnly\"") !=
              std::string::npos;
  }
  Expect(foundWs, "the opened document's project must be indexed");

  bool foundData = false;
  for (int n = 0; n < 40 && !foundData; ++n) {
    foundData =
        querySymbol(100 + n, "dataOnly").find("\"name\":\"dataOnly\"") !=
        std::string::npos;
  }
  Expect(foundData,
         "single-file mode must root at the source-layout project dir");

  bool sawStep = false;
  for (int n = 0; n < 40 && !sawStep; ++n) {
    sawStep = querySymbol(200 + n, "stepOnly").find("\"name\":\"stepOnly\"") !=
              std::string::npos;
  }
  Expect(!sawStep, "a tree beside the layout project must not be indexed");

  session.stop();

  // Without any layout marker the workspace is the file's own directory.
  lsp::NullLog log2;
  lsp::LanguageSession session2(log2);
  auto input2 = std::make_shared<FeedableIStream>();
  auto output2 = std::make_shared<StringOStream>();
  FreeBasicServer server2(session2);
  server2.registerHandlers();
  session2.start(input2, output2);

  input2->append(MakeLspFrame(kInitializeFrame));
  Expect(WaitForOutputContaining(output2, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  std::string const plainUri = FileUri(sandbox / "plain" / "file.bas");
  std::string const openPlain =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      plainUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString("sub fileOnly()\nend sub\n") + "\"}}}";
  input2->append(MakeLspFrame(openPlain.c_str()));

  auto queryPlain = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"plain" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"plain)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input2->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output2, id, 50);
  };

  bool foundNear = false;
  for (int n = 0; n < 60 && !foundNear; ++n) {
    foundNear = queryPlain(n, "nearOnly").find("\"name\":\"nearOnly\"") !=
                std::string::npos;
  }
  Expect(foundNear, "the file's own directory stays the single-file workspace");

  bool sawPark = false;
  for (int n = 0; n < 40 && !sawPark; ++n) {
    sawPark = queryPlain(100 + n, "parkOnly").find("\"name\":\"parkOnly\"") !=
              std::string::npos;
  }
  Expect(!sawPark,
         "the single-file workspace must not widen beyond the file's dir");

  session2.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// The home-folder guard that ends the two unbounded upward walks (the
// single-file marker walk and the source/include layout walk) has to decide "is
// this the home folder" by identity, not by comparing path objects: the two
// spellings come from different places and routinely disagree. Windows reports
// %USERPROFILE% in long form (C:\Users\runneradmin) while %TEMP% — and so every
// document URI built from it — names the same folder 8.3-short
// (C:\Users\RUNNER~1), and a home folder reached through a symlink keeps the
// link in the walk's spelling. Miss the guard and the walk climbs out of the
// home folder and roots the index at the profile or above: every stray .bas
// under it joins the workspace, so one project's symbols surface in another's
// workspace/symbol, and the project's own freebasicd.toml is never read, since
// the settings lookup only looks in the chosen root. That is how the Windows
// leg lost three tests, with %TEMP% handed out short.
//
// Each spelling below names the same directory the walk reaches but cannot be
// matched to it by comparing paths, so the guard has to ask the filesystem:
// `<home>/src/..` needs no privilege and runs everywhere, and the symlink is
// the case a *lexical* normalization cannot answer either — the portable
// stand-in for the 8.3 short name. Skipped with a printed note where the
// platform will not create a symlink, which is a privilege, not a behaviour.
void TestHomeFolderGuardAsksTheFilesystem() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const homeDir = sandbox / "home";
  // Laid out the way the layout walk looks for a project: a `src` child at the
  // top, so the walk finds a root the moment the guard lets it past home.
  std::filesystem::create_directories(homeDir / "src");
  std::filesystem::create_directories(homeDir / "lone");
  {
    std::ofstream out(homeDir / "src" / "stray.bas");
    out << "sub homeStray()\nend sub\n";
  }
  std::vector<std::filesystem::path> spellings{homeDir / "src" / ".."};
  {
    std::error_code ec;
    std::filesystem::create_directory_symlink(homeDir, sandbox / "homelink",
                                              ec);
    if (ec) {
      std::printf("[ NOTE    ] home-folder guard: no symlink spelling (%s)\n",
                  ec.message().c_str());
    } else {
      spellings.push_back(sandbox / "homelink");
    }
  }

  for (std::filesystem::path const &spelling : spellings) {
    ScopedEnv const home("HOME", spelling.string());

    lsp::NullLog log;
    lsp::LanguageSession session(log);
    auto input = std::make_shared<FeedableIStream>();
    auto output = std::make_shared<StringOStream>();
    FreeBasicServer server(session);
    server.registerHandlers();
    session.start(input, output);

    // No rootUri: single-file mode, so the layout walk names the workspace.
    input->append(MakeLspFrame(kInitializeFrame));
    Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                   .find("\"workspaceSymbolProvider\":") != std::string::npos,
           "initialize must advertise workspace/symbol");

    std::string const appUri = FileUri(homeDir / "lone" / "app.bas");
    input->append(MakeLspFrame(
        (R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
         R"({"uri":")" +
         appUri + R"(","languageId":"basic","version":1,"text":")" +
         ToJsonString("sub loneOnly()\nend sub\n") + "\"}}}")
            .c_str()));

    auto querySymbol = [&](char const *tag, int n, std::string const &name) {
      std::string const id =
          std::string("\"id\":\"") + tag + std::to_string(n) + "\"";
      input->append(MakeLspFrame(
          (std::string(R"({"jsonrpc":"2.0","id":")") + tag + std::to_string(n) +
           R"(","method":"workspace/symbol","params":{"query":")" + name +
           R"("}})")
              .c_str()));
      return WaitForOutputContaining(output, id, 50);
    };

    // The file's own directory is the workspace, so its declaration is there...
    bool foundLone = false;
    for (int n = 0; n < 60 && !foundLone; ++n) {
      foundLone =
          querySymbol("lone", n, "loneOnly").find("\"name\":\"loneOnly\"") !=
          std::string::npos;
    }
    Expect(foundLone, "the opened file's own directory stays the workspace");

    // ...and the layout project at the home folder is not, however the guard
    // was spelled.
    bool sawStray = false;
    for (int n = 0; n < 40 && !sawStray; ++n) {
      sawStray =
          querySymbol("stray", n, "homeStray").find("\"name\":\"homeStray\"") !=
          std::string::npos;
    }
    Expect(!sawStray,
           "the home folder's own layout project must stay out of the "
           "workspace of a file below it");

    session.stop();
  }
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// A client root that is itself a single project (has a .git marker) is used
// as-is; a *broad* root (e.g. an editor reporting the home directory, which
// hosts several sibling projects) is narrowed to the opened document's project
// on the first didOpen. Sibling trees under the broad root must never surface.
void TestBroadRootNarrowsToOpenedProject() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const broad = sandbox / "broad";
  std::filesystem::path const proj = broad / "proj";
  std::filesystem::path const sibling = broad / "sibling";
  std::filesystem::create_directories(proj / ".git");
  std::filesystem::create_directories(sibling);
  {
    std::ofstream out(proj / "app.bas");
    out << "sub wsOnly()\nend sub\n";
    std::ofstream out2(sibling / "other.bas");
    out2 << "sub siblingOnly()\nend sub\n";
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const appUri = FileUri(proj / "app.bas");
  std::string const broadRootUri = FileUri(broad);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      broadRootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      appUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString("sub wsOnly()\nend sub\n") + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"narrow" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"narrow)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  // The opened document's project is indexed...
  bool foundWs = false;
  for (int n = 0; n < 60 && !foundWs; ++n) {
    foundWs = querySymbol(n, "wsOnly").find("\"name\":\"wsOnly\"") !=
              std::string::npos;
  }
  Expect(foundWs, "the opened document's project must be indexed");

  // ...and the sibling tree under the broad root must never be.
  bool sawSibling = false;
  for (int n = 0; n < 40 && !sawSibling; ++n) {
    sawSibling =
        querySymbol(100 + n, "siblingOnly").find("\"name\":\"siblingOnly\"") !=
        std::string::npos;
  }
  Expect(!sawSibling,
         "a sibling project under a broad root must not be indexed");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// The broad-root narrowing must recognize every version-control marker the
// finder supports, not just `.git`: mercurial, svn, bazaar, fossil, etc. Each
// project sits under the same broad root; opening a document in it must narrow
// the index scope to that project and leave the sibling tree alone.
void TestBroadRootNarrowsToAnyVcsProject() {
  struct MarkerCase {
    char const *marker;
    char const *symbol;
  };
  static constexpr MarkerCase const cases[] = {
      {".git", "fromGit"},         {".hg", "fromHg"},
      {".svn", "fromSvn"},         {".bzr", "fromBzr"},
      {".fslckout", "fromFossil"}, {"_FOSSIL_", "fromFossilLegacy"},
      {".darcs", "fromDarcs"},     {".pijul", "fromPijul"},
      {"_MTN", "fromMonotone"},
  };

  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const broad = sandbox / "broad";
  std::filesystem::path const sibling = broad / "sibling";
  std::filesystem::create_directories(sibling);
  {
    std::ofstream out2(sibling / "other.bas");
    out2 << "sub siblingOnly()\nend sub\n";
  }
  std::vector<std::string> appUris;
  for (std::size_t i = 0; i < std::size(cases); ++i) {
    std::filesystem::path const proj = broad / ("proj" + std::to_string(i));
    std::filesystem::create_directories(proj / cases[i].marker);
    std::ofstream out(proj / "app.bas");
    out << "sub " << cases[i].symbol << "()\nend sub\n";
    appUris.push_back(FileUri(proj / "app.bas"));
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const broadRootUri = FileUri(broad);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      broadRootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"vcs" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"vcs)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  int idx = 0;
  bool sawSibling = false;
  for (std::size_t i = 0; i < std::size(cases); ++i) {
    std::string const openFrame =
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
        R"({"uri":")" +
        appUris[i] + R"(","languageId":"basic","version":1,"text":")" +
        ToJsonString(std::string("sub ") + cases[i].symbol + "()\nend sub\n") +
        "\"}}}";
    input->append(MakeLspFrame(openFrame.c_str()));

    bool found = false;
    for (int n = 0; n < 60 && !found; ++n) {
      found = querySymbol(idx++, cases[i].symbol)
                  .find(std::string("\"name\":\"") + cases[i].symbol + "\"") !=
              std::string::npos;
    }
    Expect(found, (std::string("project marked by ") + cases[i].marker +
                   " must narrow the index scope")
                      .c_str());
    for (int n = 0; !sawSibling && n < 25; ++n) {
      sawSibling =
          querySymbol(idx++, "siblingOnly").find("\"name\":\"siblingOnly\"") !=
          std::string::npos;
    }
  }
  Expect(!sawSibling,
         "a sibling project under a broad root must not be indexed");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// A freebasicd.toml marks its directory as a workspace root (priority-3
// detection). Under a *broad* client root the nearest config file between the
// opened document and the client root names the project; in single-file mode
// the nearest config file above the document does. Sibling trees stay outside
// the index either way.
void TestConfigFileRootDetection() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  // Phase 1: a broad client root; the opened document's project carries the
  // config file.
  std::filesystem::path const broad = sandbox / "broad";
  std::filesystem::path const proj = broad / "proj";
  std::filesystem::path const sibling = broad / "sibling";
  std::filesystem::create_directories(proj / "src");
  std::filesystem::create_directories(proj / "data");
  std::filesystem::create_directories(sibling);
  {
    std::ofstream out(proj / "freebasicd.toml");
    out << "[server]\n";
    std::ofstream out2(proj / "src" / "app.bas");
    out2 << "sub cfgProjOnly()\nend sub\n";
    std::ofstream out3(proj / "data" / "mod.bi");
    out3 << "sub cfgDataOnly()\nend sub\n";
    std::ofstream out4(sibling / "other.bas");
    out4 << "sub cfgSiblingOnly()\nend sub\n";
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const appUri = FileUri(proj / "src" / "app.bas");
  std::string const broadRootUri = FileUri(broad);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      broadRootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      appUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString("sub cfgProjOnly()\nend sub\n") + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"cfg" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"cfg)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  bool foundProj = false;
  for (int n = 0; n < 60 && !foundProj; ++n) {
    foundProj =
        querySymbol(n, "cfgProjOnly").find("\"name\":\"cfgProjOnly\"") !=
        std::string::npos;
  }
  Expect(foundProj, "the config-carrying project must be indexed");

  bool foundData = false;
  for (int n = 0; n < 40 && !foundData; ++n) {
    foundData =
        querySymbol(100 + n, "cfgDataOnly").find("\"name\":\"cfgDataOnly\"") !=
        std::string::npos;
  }
  Expect(foundData,
         "the config root must be the project dir, so a sibling dir of `src` "
         "inside it is indexed too");

  bool sawSibling = false;
  for (int n = 0; n < 40 && !sawSibling; ++n) {
    sawSibling = querySymbol(200 + n, "cfgSiblingOnly")
                     .find("\"name\":\"cfgSiblingOnly\"") != std::string::npos;
  }
  Expect(!sawSibling,
         "a sibling tree under the broad root must not be indexed");

  session.stop();

  // Phase 2: single-file mode (no client root): the nearest config file above
  // the document names the project.
  std::filesystem::path const lone = sandbox / "lone";
  std::filesystem::path const lp = lone / "proj";
  std::filesystem::create_directories(lp / "src");
  std::filesystem::create_directories(lp / "data");
  std::filesystem::create_directories(lone / "step");
  {
    std::ofstream out(lp / "freebasicd.toml");
    out << "[server]\n";
    std::ofstream out2(lp / "src" / "app.bas");
    out2 << "sub cfgSfProjOnly()\nend sub\n";
    std::ofstream out3(lp / "data" / "mod.bas");
    out3 << "sub cfgSfDataOnly()\nend sub\n";
    std::ofstream out4(lone / "step" / "x.bas");
    out4 << "sub cfgSfStepOnly()\nend sub\n";
  }

  lsp::NullLog log2;
  lsp::LanguageSession session2(log2);
  auto input2 = std::make_shared<FeedableIStream>();
  auto output2 = std::make_shared<StringOStream>();
  FreeBasicServer server2(session2);
  server2.registerHandlers();
  session2.start(input2, output2);

  input2->append(MakeLspFrame(kInitializeFrame));
  Expect(WaitForOutputContaining(output2, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  std::string const sfUri = FileUri(lp / "src" / "app.bas");
  std::string const openSf =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      sfUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString("sub cfgSfProjOnly()\nend sub\n") + "\"}}}";
  input2->append(MakeLspFrame(openSf.c_str()));

  auto querySf = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"cfs" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"cfs)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input2->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output2, id, 50);
  };

  bool foundSfProj = false;
  for (int n = 0; n < 60 && !foundSfProj; ++n) {
    foundSfProj =
        querySf(n, "cfgSfProjOnly").find("\"name\":\"cfgSfProjOnly\"") !=
        std::string::npos;
  }
  Expect(foundSfProj,
         "single-file mode must root at the config-carrying project");

  bool foundSfData = false;
  for (int n = 0; n < 40 && !foundSfData; ++n) {
    foundSfData =
        querySf(100 + n, "cfgSfDataOnly").find("\"name\":\"cfgSfDataOnly\"") !=
        std::string::npos;
  }
  Expect(foundSfData, "single-file mode must index the config project dir");

  bool sawStep = false;
  for (int n = 0; n < 40 && !sawStep; ++n) {
    sawStep =
        querySf(200 + n, "cfgSfStepOnly").find("\"name\":\"cfgSfStepOnly\"") !=
        std::string::npos;
  }
  Expect(!sawStep, "a tree beside the config project must not be indexed");

  session2.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// A client root that bears a freebasicd.toml (rootUri or a registered
// workspace folder) is *itself* a workspace root: it is used as-is and indexed
// eagerly at initialize, so workspace/symbol serves its symbols before any
// document is opened. A config-carrying registered folder joins the priority-0
// registered-root set the same way.
void TestConfigMarkerRootUsedAsIsEagerIndex() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  // Phase 1: rootUri points at a config-carrying directory.
  std::filesystem::path const proj = sandbox / "proj";
  std::filesystem::create_directories(proj / "sub");
  std::filesystem::create_directories(sandbox / "elsewhere");
  {
    std::ofstream out(proj / "freebasicd.toml");
    out << "[server]\n";
    std::ofstream out2(proj / "sub" / "app.bas");
    out2 << "sub eagerOnly()\nend sub\n";
    std::ofstream out3(sandbox / "elsewhere" / "x.bas");
    out3 << "sub elsewhereOnly()\nend sub\n";
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const rootUri = FileUri(proj);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      rootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"cmk" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"cmk)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  // Eager: no didOpen has been sent; the initialize-period scan must have
  // indexed the config-carrying root already.
  bool foundEager = false;
  for (int n = 0; n < 60 && !foundEager; ++n) {
    foundEager = querySymbol(n, "eagerOnly").find("\"name\":\"eagerOnly\"") !=
                 std::string::npos;
  }
  Expect(foundEager,
         "a config-carrying client root must be indexed at initialize");

  bool sawElsewhere = false;
  for (int n = 0; n < 40 && !sawElsewhere; ++n) {
    sawElsewhere = querySymbol(100 + n, "elsewhereOnly")
                       .find("\"name\":\"elsewhereOnly\"") != std::string::npos;
  }
  Expect(!sawElsewhere,
         "the as-is root must never reach beyond the config directory");

  session.stop();

  // Phase 2: a registered workspace folder with a config file is indexed
  // eagerly too, from the initialize reply's folder list alone.
  std::filesystem::path const fa = sandbox / "fa";
  std::filesystem::create_directories(fa);
  {
    std::ofstream out(fa / "freebasicd.toml");
    out << "[server]\n";
    std::ofstream out2(fa / "a.bas");
    out2 << "sub folderCfgOnly()\nend sub\n";
  }

  lsp::NullLog log2;
  lsp::LanguageSession session2(log2);
  auto input2 = std::make_shared<FeedableIStream>();
  auto output2 = std::make_shared<StringOStream>();
  FreeBasicServer server2(session2);
  server2.registerHandlers();
  session2.start(input2, output2);

  std::string const faUri = FileUri(fa);
  std::string const init2 =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"workspaceFolders":[)"
      R"({"uri":")" +
      faUri + R"(","name":"fa"}]}})";
  input2->append(MakeLspFrame(init2.c_str()));
  Expect(WaitForOutputContaining(output2, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  auto queryFolder = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"cmf" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"cmf)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input2->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output2, id, 50);
  };

  bool foundFolder = false;
  for (int n = 0; n < 60 && !foundFolder; ++n) {
    foundFolder =
        queryFolder(n, "folderCfgOnly").find("\"name\":\"folderCfgOnly\"") !=
        std::string::npos;
  }
  Expect(foundFolder,
         "a config-carrying workspace folder must be indexed at initialize");

  session2.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

void TestWorkspaceSymbolIndexesWorkspace() {
  char const *kLibContent =
      "function clamp(v as integer, lo as integer) as integer\n"
      "    if v < lo then return lo\n"
      "    return v\n"
      "end function\n";

  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const wsDir = sandbox / "ws";
  std::filesystem::create_directories(wsDir);
  {
    std::ofstream out(wsDir / "lib.bi");
    out << kLibContent;
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const fileUri = FileUri(wsDir / "lib.bi");
  std::string const rootUri = FileUri(wsDir);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      rootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  std::string const init = WaitForOutputContaining(output, "\"id\":\"init\"");
  Expect(init.find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      fileUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString(kLibContent) + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));

  bool found = false;
  for (int n = 0; n < 60 && !found; ++n) {
    std::string const id = "\"id\":\"ws" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"ws)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":"clamp"}})";
    input->append(MakeLspFrame(request.c_str()));
    std::string const snapshot = WaitForOutputContaining(output, id, 50);
    found = snapshot.find("\"name\":\"clamp\"") != std::string::npos;
  }
  Expect(found, "workspace/symbol must return the clamp function");
  std::string const last =
      WaitForOutputContaining(output, "\"name\":\"clamp\"", 5);
  Expect(last.find(fileUri) != std::string::npos,
         "workspace/symbol location must point into the workspace file");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// A member implementation (`sub T.proc()`) sits at file level in fbc's own
// model, so the workspace walk reaches it with no container around it: the
// qualifier comes from `Symbol::ownerName` and lands in `containerName`,
// which is the field a client renders as `T::proc` (Kate builds the label
// from containerName + name).
void TestWorkspaceSymbolQualifiesMemberImplementations() {
  char const *kModContent = "type T\n"
                            "    dim x as long\n"
                            "end type\n"
                            "\n"
                            "sub T.proc()\n"
                            "end sub\n";

  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const wsDir = sandbox / "ws";
  std::filesystem::create_directories(wsDir);
  {
    std::ofstream out(wsDir / "mod.bas");
    out << kModContent;
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const fileUri = FileUri(wsDir / "mod.bas");
  std::string const rootUri = FileUri(wsDir);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      rootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  WaitForOutputContaining(output, "\"id\":\"init\"");

  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      fileUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString(kModContent) + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));

  std::string last;
  bool found = false;
  for (int n = 0; n < 60 && !found; ++n) {
    std::string const id = "\"id\":\"wsq" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"wsq)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":"proc"}})";
    input->append(MakeLspFrame(request.c_str()));
    last = WaitForOutputContaining(output, id, 50);
    found = last.find("\"containerName\":\"T\"") != std::string::npos;
  }
  Expect(found, "workspace/symbol must return the member implementation, with "
                "its qualifier as containerName");
  Expect(last.find("\"name\":\"proc\"") != std::string::npos &&
             last.find("\"containerName\":\"T\"") != std::string::npos,
         "the qualifier must ride as containerName (a client renders the pair "
         "as T::proc) — without it the hit is an unqualified `proc` with no "
         "owner, unlike the outline's `T.proc`");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}
} // namespace

namespace fbtest {

void RunWorkspaceTests() {
  RUN_TEST(TestWorkspaceSymbolIndexesWorkspace);
  RUN_TEST(TestWorkspaceSymbolQualifiesMemberImplementations);
  RUN_TEST(TestOutsideFileNotIndexed);
  RUN_TEST(TestHoverWorksForDocOutsideWorkspaceRoot);
  RUN_TEST(TestSourceLayoutRootNarrowsToOpenedProject);
  RUN_TEST(TestSourceLayoutRootRecognizesCatalogNames);
  RUN_TEST(TestSourceLayoutRootSingleFileMode);
  RUN_TEST(TestHomeFolderGuardAsksTheFilesystem);
  RUN_TEST(TestBroadRootNarrowsToOpenedProject);
  RUN_TEST(TestBroadRootNarrowsToAnyVcsProject);
  RUN_TEST(TestConfigFileRootDetection);
  RUN_TEST(TestConfigMarkerRootUsedAsIsEagerIndex);
}

} // namespace fbtest
