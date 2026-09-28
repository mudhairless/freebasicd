/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "code_lens.h"

#include "i18n.h"
#include "symbols.h"

#include <algorithm>

namespace fblang {
namespace {

// The declarations that carry a lens: the procedure-like kinds and the type-ish
// roots. Deliberately the same node-kind set the call-hierarchy treats as a
// node (call_hierarchy.h), so a procedure a reader can navigate to is a
// procedure they can count references on.
bool carriesLens(SymbolKind kind) {
  switch (kind) {
  case SymbolKind::Sub:
  case SymbolKind::Function:
  case SymbolKind::Property:
  case SymbolKind::Constructor:
  case SymbolKind::Destructor:
  case SymbolKind::Operator:
  case SymbolKind::Type:
  case SymbolKind::Union:
  case SymbolKind::Enum:
  case SymbolKind::Namespace:
    return true;
  default:
    return false;
  }
}

// Pre-order, so a declaration is visited before the members nested inside it —
// which is also source order for a symbol tree. The sort in codeLenses does not
// rely on it.
void collectAnchors(std::vector<Symbol> const &roots,
                    std::vector<LensAnchor> &out) {
  for (Symbol const &sym : roots) {
    // A zero-width selection is a declaration with no name token: there is
    // nothing to anchor a lens to, and the click would have no position to
    // carry back.
    if (carriesLens(sym.kind) && sym.selection.beg != sym.selection.end) {
      out.push_back(LensAnchor{sym.selection, sym.name, sym.kind});
    }
    collectAnchors(sym.children, out);
  }
}

} // namespace

std::vector<CodeLens> codeLenses(AnalyzedDoc const &doc,
                                 ReferenceCounter const &count) {
  std::vector<LensAnchor> anchors;
  collectAnchors(doc.parse.roots, anchors);
  // Source order, stated rather than inherited from the walk above: a client is
  // free to render the array in any order, and a stable sort on the construct's
  // start puts a member procedure after the type that holds it.
  std::stable_sort(anchors.begin(), anchors.end(),
                   [](LensAnchor const &a, LensAnchor const &b) {
                     return a.selection.beg < b.selection.beg;
                   });

  std::vector<CodeLens> lenses;
  lenses.reserve(anchors.size());
  for (LensAnchor const &anchor : anchors) {
    std::size_t const references = count ? count(anchor) : 0;
    CodeLens lens;
    lens.anchor = anchor;
    lens.references = references;
    // TRANSLATORS: the count of places that reference this declaration, e.g.
    // "3 references". Keep the %s where the number goes; the plural form is
    // selected by the catalog's plural rule, so do not merge the two forms
    // into one.
    lens.title = trn("%s reference", "%s references",
                     static_cast<unsigned long>(references));
    lenses.push_back(std::move(lens));
  }
  return lenses;
}

} // namespace fblang
