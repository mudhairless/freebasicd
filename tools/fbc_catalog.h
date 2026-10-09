/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

// The imported fbc catalog, shared by the generator and the freshness test.
//
// `tools/fbc_catalog.tsv` is the durable snapshot extracted from `error.bas` at
// the pinned tag (see tools/extract_fbc_catalog.py). `emitCatalogInclude`
// turns it into the exact bytes of `src/fbc_diagnostics.inc`; `gen_fbc_catalog`
// writes them and `fbc_diagnostics_checks` byte-diffs them, so the writer and
// the freshness gate cannot disagree about the format.
namespace fbc_catalog {

// One catalog row. `level` is a warning's `-w` gate level and always 0 for an
// error (the level model does not apply to errors).
struct Entry {
  bool warning = false;
  int number = 0;
  std::string name;
  std::string text;
  int level = 0;
};

// Parse the five-column TSV (`kind number name text level`). Malformed lines
// are skipped; the caller decides whether the result is complete.
std::vector<Entry> parseCatalog(std::string_view tsv);

// The bytes of `src/fbc_diagnostics.inc`: two arrays (errors then warnings,
// each in number order) plus their counts, deterministic and LF-terminated.
std::string emitCatalogInclude(std::vector<Entry> const &entries);

} // namespace fbc_catalog
