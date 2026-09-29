/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// M11 index lifetime over the wire.
//
// One index per workspace root, as a client sees it: a watched-files event
// converging an external edit, an open buffer outranking the disk copy of the
// same file, and two workspace folders staying isolated through add and remove.
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

using namespace fbtest;

namespace {
// An open buffer outranks disk, and a cross-file range is measured against the
// buffer -- so a rescan that re-parses the disk copy moves the answer.
//
// The Windows leg is what proved the pairing matters. Every fixture here writes
// through a text-mode ofstream, so MSVC put CRLF on disk under the LF text the
// didOpen carried, and `dim localOnly` came back at 1:5-1:14 instead of
// 1:4-1:13: the index held the disk parse's byte offsets while contentForPath
// still served the buffer, one column out for every line the two copies
// disagreed on. Nothing about the request was wrong and nothing about the index
// was stale -- they were two different files.
//
// So the rescan here is *proven* to have run, or the assertion below is
// vacuous: `converged` exists only in the new disk bytes of a file nothing
// opened, so only a scan can have read them.
void TestScanKeepsOpenBufferAheadOfDisk() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartIndexedSession(
      session, server, output, fix,
      {{fix.mainUri, kMainContent}, {fix.libUri, kLibContent}});

  auto definition = [&](std::string const &tag) {
    return PollRequest(input, output, tag, fix.libUri, [&](std::string const &id) {
      return R"({"jsonrpc":"2.0","id":")" + id +
             R"(","method":"textDocument/definition","params":{"textDocument":{"uri":")" +
             fix.mainUri + R"("},"position":{"line":2,"character":22}}})";
    });
  };
  // The header's own columns: `dim localOnly` puts the name at 4..13.
  auto inHeader = [](std::string const &reply) {
    return reply.find("\"start\":{\"line\":1,\"character\":4}") !=
               std::string::npos &&
           reply.find("\"end\":{\"line\":1,\"character\":13}") !=
               std::string::npos;
  };

  std::string const before = definition("sob");
  Expect(inHeader(before),
         ("the header's own columns must be reported before any rescan; "
          "reply was " +
          before.substr(0, 200))
             .c_str());

  // Binary, so these are the exact bytes on every platform: the disk copy
  // diverges from the buffer by CRLF (one column per line after the first),
  // and a closed file gains a symbol.
  {
    std::ofstream out(fix.wsDir / "lib.bi", std::ios::binary);
    for (char c : std::string(kLibContent)) {
      if (c == '\n') {
        out.put('\r');
      }
      out.put(c);
    }
  }
  {
    std::ofstream out(fix.wsDir / "prog.bas", std::ios::binary);
    out << kProgContent << "sub converged()\nend sub\n";
  }
  std::string const watchedFrame =
      R"({"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[)"
      R"({"uri":")" +
      fix.libUri + R"(","type":2},{"uri":")" + fix.progUri +
      R"(","type":2}]}})";
  input->append(MakeLspFrame(watchedFrame.c_str()));

  bool rescanned = false;
  for (int n = 0; n < 100 && !rescanned; ++n) {
    std::string const id = "\"id\":\"sob" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"sob)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":"converged"}})";
    input->append(MakeLspFrame(request.c_str()));
    rescanned = WaitForOutputContaining(output, id, 50)
                    .find("\"name\":\"converged\"") != std::string::npos;
  }
  Expect(rescanned,
         "the rescan must read a closed file's new disk bytes, or the "
         "assertion below proves nothing");

  std::string const after = definition("soa");
  Expect(inHeader(after),
         ("a rescan must not move the header's columns off the open buffer; "
          "reply was " +
          after.substr(0, 200))
             .c_str());

  session.stop();
}

// One index per registered workspace folder. workspace/symbol aggregates every
// live index, but a document opened in one folder is served strictly by its
// own folder's index: sibling-folder module roots must never leak into its
// completion closure.
void TestMultiWorkspaceFoldersStayIsolated() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const fa = sandbox / "fa";
  std::filesystem::path const fb = sandbox / "fb";
  std::filesystem::create_directories(fa);
  std::filesystem::create_directories(fb);
  {
    std::ofstream out(fa / "freebasicd.toml");
    out << "[server]\n";
    std::ofstream out2(fa / "a.bas");
    out2 << "sub alphaOnly()\nend sub\n";
    std::ofstream out3(fb / "freebasicd.toml");
    out3 << "[server]\n";
    std::ofstream out4(fb / "b.bas");
    out4 << "sub betaOnly()\nend sub\n";
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const faUri = FileUri(fa);
  std::string const fbUri = FileUri(fb);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"workspaceFolders":[)"
      R"({"uri":")" +
      faUri + R"(","name":"fa"},{"uri":")" + fbUri + R"(","name":"fb"}]}})";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"mul" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"mul)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  // Aggregation: both folder roots are eager (config markers) and both must
  // contribute to workspace/symbol with no didOpen at all.
  bool foundAlpha = false;
  for (int n = 0; n < 60 && !foundAlpha; ++n) {
    foundAlpha = querySymbol(n, "alphaOnly").find("\"name\":\"alphaOnly\"") !=
                 std::string::npos;
  }
  Expect(foundAlpha, "workspace/symbol must aggregate folder A's symbols");
  bool foundBeta = false;
  for (int n = 0; n < 60 && !foundBeta; ++n) {
    foundBeta =
        querySymbol(100 + n, "betaOnly").find("\"name\":\"betaOnly\"") !=
        std::string::npos;
  }
  Expect(foundBeta, "workspace/symbol must aggregate folder B's symbols");

  // Isolation: opening B's document and completing at module level must offer
  // B's own module root but never A's (B is served by B's index alone).
  std::string const bUri = FileUri(fb / "b.bas");
  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      bUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString("sub betaOnly()\nend sub\n") + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));

  std::string completion;
  for (int n = 0; n < 40; ++n) {
    std::string const id = "\"id\":\"mulc" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"mulc)" + std::to_string(n) +
        R"(","method":"textDocument/completion","params":{"textDocument":{"uri":")" +
        bUri + R"("},"position":{"line":2,"character":0}}})";
    input->append(MakeLspFrame(request.c_str()));
    std::string const snapshot = WaitForOutputContaining(output, id, 50);
    if (snapshot.find("\"label\":\"betaOnly\"") != std::string::npos) {
      // Clip to this reply: `alphaOnly` must be judged against B's own
      // completion only.
      completion = snapshot.substr(snapshot.rfind(id));
      break;
    }
    completion = snapshot;
  }
  Expect(completion.find("\"label\":\"betaOnly\"") != std::string::npos,
         "completion in folder B must offer B's own module root");
  Expect(completion.find("\"label\":\"alphaOnly\"") == std::string::npos,
         "completion in folder B must never offer folder A's module root");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// workspace/didChangeWorkspaceFolders adds and removes indexes live: an added
// config-carrying folder is indexed immediately; removing it closes its index
// and its symbols leave workspace/symbol — unless the session root or another
// registered folder still needs it, in which case it survives. The initialize
// reply advertises folder support and change notifications.
void TestWorkspaceFoldersChangedAddRemove() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const fa = sandbox / "fa";
  std::filesystem::path const fb = sandbox / "fb";
  std::filesystem::create_directories(fa);
  std::filesystem::create_directories(fb);
  {
    std::ofstream out(fa / "freebasicd.toml");
    out << "[server]\n";
    std::ofstream out2(fa / "a.bas");
    out2 << "sub addOnly()\nend sub\n";
    std::ofstream out3(fb / "freebasicd.toml");
    out3 << "[server]\n";
    std::ofstream out4(fb / "b.bas");
    out4 << "sub addedOnly()\nend sub\n";
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const faUri = FileUri(fa);
  std::string const fbUri = FileUri(fb);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"workspaceFolders":[)"
      R"({"uri":")" +
      faUri + R"(","name":"fa"}]}})";
  input->append(MakeLspFrame(initFrame.c_str()));
  std::string const init = WaitForOutputContaining(output, "\"id\":\"init\"");
  Expect(init.find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");
  Expect(init.find("\"workspaceFolders\":{\"supported\":true,"
                   "\"changeNotifications\":true}") != std::string::npos,
         "initialize must advertise folder support and change notifications");

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"chg" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"chg)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    const std::string snapshot = WaitForOutputContaining(output, id, 50);
    // Tail from this reply's id, so a stale mention in an earlier reply (this
    // session queries the same names again) never trips an assertion.
    return snapshot.substr(snapshot.rfind(id));
  };

  bool foundAdd = false;
  for (int n = 0; n < 60 && !foundAdd; ++n) {
    foundAdd = querySymbol(n, "addOnly").find("\"name\":\"addOnly\"") !=
               std::string::npos;
  }
  Expect(foundAdd, "the registered folder must be indexed at initialize");

  // Add fb at runtime: its symbols must appear without any didOpen.
  std::string const addFrame =
      R"({"jsonrpc":"2.0","method":"workspace/didChangeWorkspaceFolders","params":{"event":{"added":[)"
      R"({"uri":")" +
      fbUri + R"(","name":"fb"}],"removed":[]}}})";
  input->append(MakeLspFrame(addFrame.c_str()));

  bool foundAdded = false;
  for (int n = 0; n < 60 && !foundAdded; ++n) {
    foundAdded =
        querySymbol(100 + n, "addedOnly").find("\"name\":\"addedOnly\"") !=
        std::string::npos;
  }
  Expect(foundAdded, "an added config-carrying folder must be indexed at once");

  // Remove fb: its index closes and its symbols leave workspace/symbol, while
  // fa keeps serving.
  std::string const removeFrame =
      R"({"jsonrpc":"2.0","method":"workspace/didChangeWorkspaceFolders","params":{"event":{"added":[],)"
      R"("removed":[{"uri":")" +
      fbUri + R"(","name":"fb"}]}}})";
  input->append(MakeLspFrame(removeFrame.c_str()));
  // The removal runs on the notification FIFO thread; let it land before the
  // negative poll so a straggling pre-removal reply cannot trip the assertion.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  bool sawGone = false;
  for (int n = 0; n < 40 && !sawGone; ++n) {
    sawGone =
        querySymbol(200 + n, "addedOnly").find("\"name\":\"addedOnly\"") !=
        std::string::npos;
  }
  Expect(!sawGone, "removing a folder must drop its symbols from "
                   "workspace/symbol");

  bool stillAdd = false;
  for (int n = 0; n < 20 && !stillAdd; ++n) {
    stillAdd = querySymbol(300 + n, "addOnly").find("\"name\":\"addOnly\"") !=
               std::string::npos;
  }
  Expect(stillAdd, "an unaffected registered folder must keep serving");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

void TestWatchedFilesRescanConverges() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const wsDir = sandbox / "ws";
  std::filesystem::create_directories(wsDir);
  std::string const lib = "sub greet()\nend sub\n";
  {
    std::ofstream out(wsDir / "lib.bi");
    out << lib;
  }
  // The header stays *closed*, because that is the case where disk is the
  // truth: the rescan converges it. An open buffer is the opposite case -- the
  // client owns those bytes until didClose, and a rescan that re-parsed the
  // disk copy would leave the index's offsets describing a different file than
  // the one a reply is measured against (see
  // TestScanKeepsOpenBufferAheadOfDisk). Opening main.bas is also what a real
  // editor does, and what picks the index root for this non-project client.
  std::string const main = "#include \"lib.bi\"\nsub mainProc()\nend sub\n";
  {
    std::ofstream out(wsDir / "main.bas");
    out << main;
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const fileUri = FileUri(wsDir / "lib.bi");
  std::string const mainUri = FileUri(wsDir / "main.bas");
  std::string const rootUri = FileUri(wsDir);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      rootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  // A non-project client root defers index creation to the first opened
  // document, exactly like a real editor always opens one.
  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      mainUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString(main) + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"wat" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"wat)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  // The initial background scan must index the closed header before the edit --
  // and since nothing opens it, only the scan can have read it.
  bool primed = false;
  for (int n = 0; n < 60 && !primed; ++n) {
    primed =
        querySymbol(n, "greet").find("\"name\":\"greet\"") != std::string::npos;
  }
  Expect(primed,
         "workspace/symbol must find the header symbol from the initial scan");

  // A disk edit converges through the watched-files notification: no reopen,
  // no didChange, no restart.
  {
    std::ofstream out(wsDir / "lib.bi");
    out << lib << "sub farewell()\nend sub\n";
  }
  std::string const watchedFrame =
      R"({"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[)"
      R"({"uri":")" +
      fileUri + R"(","type":2}]}})";
  input->append(MakeLspFrame(watchedFrame.c_str()));

  bool converged = false;
  for (int n = 0; n < 100 && !converged; ++n) {
    converged =
        querySymbol(100 + n, "farewell").find("\"name\":\"farewell\"") !=
        std::string::npos;
  }
  Expect(converged, "a watched-files event must converge an external header "
                    "edit into workspace/symbol");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}
} // namespace

namespace fbtest {

void RunIndexesTests() {
  RUN_TEST(TestWatchedFilesRescanConverges);
  RUN_TEST(TestScanKeepsOpenBufferAheadOfDisk);
  RUN_TEST(TestMultiWorkspaceFoldersStayIsolated);
  RUN_TEST(TestWorkspaceFoldersChangedAddRemove);
}

} // namespace fbtest
