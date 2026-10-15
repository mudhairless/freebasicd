/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "document_link.h"

#include <utility>

namespace fblang {

std::vector<DocumentLinkItem> documentLinks(AnalyzedDoc const &doc,
                                            IncludeResolver const &resolve) {
  std::vector<DocumentLinkItem> out;
  if (!resolve) {
    return out;
  }
  for (IncludeDirective const &inc : doc.includes) {
    // A bare `#include` parses to an empty literal with a zero-width target
    // range: there is no filename to open, and a link there would be an
    // invisible click target at file offset 0.
    if (inc.literal.empty() || inc.target.beg == inc.target.end) {
      continue;
    }
    std::optional<std::string> target = resolve(inc.literal);
    if (!target) {
      continue; // unresolved: include-not-found is the report, not a dead link
    }
    out.push_back(DocumentLinkItem{inc.target, std::move(*target)});
  }
  return out;
}

IncludeDirective const *includeLiteralAt(AnalyzedDoc const &doc,
                                         std::uint32_t offset) {
  for (IncludeDirective const &inc : doc.includes) {
    if (inc.target.beg == inc.target.end) {
      continue; // a bare `#include` (or `#include ""`) names nothing to open
    }
    // Half-open, but the end is inclusive here so the closing quote is a hit:
    // a click just past the filename still names the file.
    if (offset >= inc.target.beg && offset <= inc.target.end) {
      return &inc;
    }
  }
  return nullptr;
}

} // namespace fblang
