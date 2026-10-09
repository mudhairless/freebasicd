/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Writes the generated fbc catalog include (`src/fbc_diagnostics.inc`) from the
// imported snapshot (`tools/fbc_catalog.tsv`). The emit logic lives in the
// shared `fbc_catalog` module so `fbc_diagnostics_checks` byte-diffs exactly
// what this tool writes.
//
//   gen_fbc_catalog [tsv] [out.inc]
//     defaults: tools/fbc_catalog.tsv  src/fbc_diagnostics.inc
#include "fbc_catalog.h"

#include <cstdio>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>

namespace {

int reportFatal(std::string const &message) {
  int const written =
      std::fprintf(stderr, "gen_fbc_catalog: %s\n", message.c_str());
  return written < 0 ? 2 : 1;
}

std::string readFile(std::string const &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

} // namespace

int main(int argc, char **argv) {
  std::string const tsvPath = argc > 1 ? argv[1] : "tools/fbc_catalog.tsv";
  std::string const outPath = argc > 2 ? argv[2] : "src/fbc_diagnostics.inc";

  std::string const tsv = readFile(tsvPath);
  if (tsv.empty()) {
    return reportFatal("cannot read " + tsvPath);
  }
  std::string const out =
      fbc_catalog::emitCatalogInclude(fbc_catalog::parseCatalog(tsv));

  std::ofstream file(outPath, std::ios::binary | std::ios::trunc);
  if (!file) {
    return reportFatal("cannot open " + outPath);
  }
  file << out;
  if (!file) {
    return reportFatal("cannot write " + outPath);
  }
  return 0;
}
