/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "index.h"
#include "resolve.h"
#include "symbols.h"

namespace fblang {

// Quick fixes (M12): LSP `textDocument/codeAction`, LSP-agnostic and in byte
// offsets like the rest of the language layer. The session converts the edits
// to UTF-16 positions and wraps them in the single-file WorkspaceEdit.

// One edit; an empty range is a pure insertion at `range.beg`.
struct TextEditBytes {
  SourceRange range;
  std::string newText;
};

// A fix for one diagnostic: the title the client shows, the diagnostic it
// answers, and the edits that resolve it. `code` + `diagRange` echo the
// diagnostic back so the client can bind the fix to the squiggle it came from.
struct QuickFix {
  std::string title;
  std::string code;
  SourceRange diagRange;
  std::vector<TextEditBytes> edits;
};

// Everything a provider may consult besides the request document. Workspace
// and filesystem knowledge enters only through these members, so a provider
// stays a pure function of (diagnostic, context) and is unit-testable without
// an index, a session, or a workspace on disk.
struct QuickFixContext {
  std::string_view content;         // the request document's bytes
  AnalyzedDoc const *doc = nullptr; // its analysis (tokens, blocks, includes)
  std::filesystem::path const *documentPath = nullptr; // normalized abs path

  // The workspace's indexed files, for a fix that hunts for a candidate
  // target. Null when the request document is served without an index.
  std::vector<std::shared_ptr<IndexedFile const>> const *workspaceFiles =
      nullptr;

  // Resolve `literal` exactly as this document's own include edges were
  // resolved — the seam `include-not-found` is derived from. A fix that only
  // offers a literal this callback accepts is correct by construction: the
  // next publish resolves the edge it edits and the diagnostic is gone.
  std::function<std::optional<std::string>(std::string const &)> resolveInclude;
};

// Answers one diagnostic; returns the fixes it offers, best first.
using QuickFixProvider = std::function<std::vector<QuickFix>(
    Diagnostic const &, QuickFixContext const &)>;

struct QuickFixRegistration {
  std::string code; // diagnostic code this provider answers
  QuickFixProvider provider;
};

// Every fix this build knows, in offer order. Adding one is a row here plus
// its function: the session, the capability, and the protocol layer never
// change, and a diagnostic code with no row offers nothing.
std::vector<QuickFixRegistration> const &quickFixProviders();

// The provider registered for `code`, or nullptr when nothing answers it.
QuickFixProvider const *quickFixProviderFor(std::string const &code);

// The `include-not-found` diagnostics an index entry's edges produce (M6
// publishes exactly these, one Error per unresolved edge at the literal's
// range). Shared with the publish path so the two can never disagree on which
// range carries the code a fix keys on. Edges with no filename literal, and
// literals reaching past a `contentSize`-byte buffer, are skipped.
std::vector<Diagnostic>
unresolvedIncludeDiagnostics(std::vector<IncludeEdge> const &edges,
                             std::size_t contentSize);

} // namespace fblang
