/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <string>
#include <vector>

namespace fbgrammar {

// One generated editor artifact: a path relative to `docs/grammar/` plus its
// exact bytes. Shared by the `gen_grammar` tool (which writes them) and the
// `grammar_checks` test (which byte-diffs them against the committed copies),
// so the writer and the freshness gate cannot disagree about the format.
struct GeneratedFile {
  std::string relativePath;
  std::string content;
};

// Deterministic emitter: stable key order, LF endings, fixed JSON field order,
// so a freshness diff is byte-stable. The TextMate output is parsed back with
// RapidJSON before it is returned; the micro YAML has no vendored parser to
// round-trip through, so its structure is by construction and its correctness
// is checked structurally in `grammar_checks`. All three grammars are derived
// from the `src/language.cpp` catalog: the lexer and the grammars cannot drift.
std::vector<GeneratedFile> generate();

} // namespace fbgrammar
