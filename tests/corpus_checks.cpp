/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// fbc-agreement corpus driver.
//
// For every tests/corpus/*.bas snippet this driver:
//   1. parses it with our parser and checks the emitted diagnostic codes match
//      the golden <name>.diag file (absent golden = no diagnostics expected);
//   2. when the system fbc compiler is available, compiles the snippet with
//      `fbc -c` and asserts agreement with each file's `'@fbc:pass` /
//      `'@fbc:fail` marker (default: pass);
//   3. for pass files, additionally asserts our parser emits no Error-severity
//      diagnostics (Information/Warning, e.g. lang-mode, meta-directive, are
//      tolerated).
//
// Exit status: 0 = all agree, 1 = disagreement, 77 = fbc unavailable (skipped).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "parser.h"

namespace fs = std::filesystem;
using namespace fblang;

#ifndef CORPUS_DIR
#define CORPUS_DIR "tests/corpus"
#endif

static int failures = 0;

#ifdef _WIN32
static int exitCodeOf(int raw) { return raw; }
static const char *kNullDev = ">nul 2>&1";
#else
#include <sys/wait.h>
static int exitCodeOf(int raw) {
  return WIFEXITED(raw) ? WEXITSTATUS(raw) : raw;
}
static const char *kNullDev = ">/dev/null 2>&1";
#endif

static std::string readFile(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static std::vector<std::string> readDiagGolden(const fs::path &dir,
                                               const std::string &name) {
  fs::path gp = dir / (name + ".diag");
  if (!fs::exists(gp)) {
    return {};
  }
  std::ifstream in(gp);
  std::vector<std::string> out;
  std::string line;
  while (std::getline(in, line)) {
    std::string t = line;
    while (!t.empty() &&
           (t.back() == '\r' || t.back() == ' ' || t.back() == '\t')) {
      t.pop_back();
    }
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) {
      t.erase(t.begin());
    }
    if (t.empty() || t[0] == '#') {
      continue;
    }
    out.push_back(t);
  }
  std::sort(out.begin(), out.end());
  return out;
}

static std::vector<std::string> actualCodes(const ParseResult &r) {
  std::vector<std::string> out;
  out.reserve(r.diagnostics.size());
  for (const auto &d : r.diagnostics) {
    out.push_back(d.code);
  }
  std::sort(out.begin(), out.end());
  return out;
}

static bool anyErrorSeverity(const ParseResult &r) {
  for (const auto &d : r.diagnostics) {
    if (d.severity == Severity::Error) {
      return true;
    }
  }
  return false;
}

static void report(const std::string &name, const std::string &reason) {
  std::printf("[FAIL] %-24s %s\n", name.c_str(), reason.c_str());
  ++failures;
}

int main(int argc, char **argv) {
  fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::path(CORPUS_DIR);

  bool fbcAvailable = (std::system("fbc -version >/dev/null 2>&1") == 0);
  if (fbcAvailable) {
    std::printf("fbc found; compiling agreement checks enabled\n");
  } else {
    std::printf("fbc NOT found; compiling agreement checks will be skipped\n");
  }

  // Scratch dir for the fbc-compile half of the harness. Clear a stale copy
  // from an interrupted earlier run first so the fixture never leaks into this
  // one; it is removed again on every exit path below.
  fs::path work = fs::path("corpus_work");
  fs::remove_all(work);
  fs::create_directories(work);

  std::vector<fs::path> files;
  if (fs::exists(dir)) {
    for (const auto &e : fs::directory_iterator(dir)) {
      if (e.path().extension() == ".bas") {
        files.push_back(e.path());
      }
    }
  }
  std::sort(files.begin(), files.end());

  size_t passed = 0;
  for (const auto &f : files) {
    std::string name = f.stem().string();
    std::string content = readFile(f);

    // Parser side.
    ParseResult r = parseDocument(content);
    std::vector<std::string> actual = actualCodes(r);
    std::vector<std::string> expected = readDiagGolden(dir, name);

    bool failMarker = content.find("@fbc:fail") != std::string::npos;

    if (actual != expected) {
      std::string reason = "diagnostics mismatch";
      reason += ", got:";
      for (const auto &c : actual) {
        reason += " " + c;
      }
      reason += ", want:";
      for (const auto &c : expected) {
        reason += " " + c;
      }
      report(name, reason);
      continue;
    }

    bool fbcOk = true;
    if (fbcAvailable) {
      fs::path wf = work / (name + ".bas");
      std::ofstream out(wf, std::ios::binary);
      out << content;
      out.close();
      std::string cmd = "fbc -c \"" + wf.string() + "\" " + kNullDev;
      int exitCode = exitCodeOf(std::system(cmd.c_str()));
      fbcOk = (exitCode == 0);
    }

    if (failMarker) {
      if (!actual.empty() && fbcOk == false) {
        ++passed;
        std::printf("[ok]   %-24s fbc rejected as expected\n", name.c_str());
      } else if (!fbcAvailable) {
        if (!actual.empty()) {
          ++passed;
          std::printf("[ok]   %-24s (no fbc) diagnostics as expected\n",
                      name.c_str());
        }
      } else {
        report(name, fbcOk ? "fbc compiled but @fbc:fail expected"
                           : "fbc rejected but no diagnostic emitted");
      }
    } else {
      bool clean = !anyErrorSeverity(r);
      if (!clean) {
        report(name, "parser reports Error-severity diagnostics");
        continue;
      }
      if (!fbcOk) {
        report(name, "fbc rejected a @fbc:pass file");
        continue;
      }
      ++passed;
      std::printf("[ok]   %-24s %s\n", name.c_str(),
                  fbcAvailable ? "fbc + parser agree"
                               : "parser clean (no fbc)");
    }
  }

  std::printf("%zu files checked, %zu failures\n", files.size(), failures);
  std::error_code ec;
  fs::remove_all(work, ec);
  if (failures > 0) {
    return 1;
  }
  if (!fbcAvailable) {
    return 77; // ctest: skipped (fbc agreement half not run)
  }
  return 0;
}
