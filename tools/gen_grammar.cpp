/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Writes the generated editor grammars (`docs/grammar/*`) from the catalog.
// The emit logic lives in the shared `grammar_emitter` module so
// `grammar_checks` byte-diffs exactly what this tool would write.
//
//   gen_grammar [output-dir]     # default: docs/grammar
#include "grammar_emitter.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>

namespace {

// Print a fatal tool error and return the process exit code: 1 for the
// failure itself, or 2 when even the diagnostic cannot be written back to
// stderr (fprintf returns negative), so a wrapper can tell a silently
// failing run apart.
int reportFatal(std::string const &message) {
  int const written =
      std::fprintf(stderr, "gen_grammar: %s\n", message.c_str());
  return written < 0 ? 2 : 1;
}

} // namespace

int main(int argc, char **argv) {
  std::filesystem::path const outDir = argc > 1 ? argv[1] : "docs/grammar";

  std::error_code ec;
  std::filesystem::create_directories(outDir, ec);
  if (ec) {
    return reportFatal("cannot create " + outDir.string() + ": " +
                       ec.message());
  }

  for (fbgrammar::GeneratedFile const &file : fbgrammar::generate()) {
    std::filesystem::path const path = outDir / file.relativePath;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return reportFatal("cannot open " + path.string());
    }
    out << file.content;
    if (!out) {
      return reportFatal("cannot write " + path.string());
    }
  }
  return 0;
}
