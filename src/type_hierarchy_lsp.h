/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "LibLsp/JsonRpc/RequestInMessage.h"
#include "LibLsp/lsp/lsRange.h"
#include "LibLsp/lsp/textDocument/typeHierarchy.h"

// The type-hierarchy request types (M15), defined locally for two reasons, both
// the `call_hierarchy_lsp.h` / `semantic_tokens_lsp.h` precedent: a method name
// the vendored tree registers wrongly, and a request type it does not ship at
// all. `LanguageSession::on` keys registration on the method *string* and
// installs the JSON parser with it, so the vendored `td_typeHierarchy` is left
// unregistered and nothing collides.
//
// 1. The vendored `td_typeHierarchy` answers a bare `TypeHierarchyItem` where
//    the protocol says `TypeHierarchyItem[] | null`. A server that answered one
//    item could never show a hierarchy, and the two are not compatible: the
//    response *struct* is serialized by the type's own `Reflect`, not by
//    whatever the request type's template parameter happens to be.
//
// 2. `typeHierarchy/supertypes` and `typeHierarchy/subtypes` have no request
//    type in the vendored tree at all. Their params are not a document and a
//    position — they embed the client's own `TypeHierarchyItem`, so the server
//    learns which type the client means from the item it was handed, by `uri` +
//    `selectionRange`.
//
// A client that supports `typeHierarchy/resolve` must be told not to send it:
// the capability is advertised through `typeHierarchyProvider`'s bool arm,
// which carries no `resolveProvider`, so the field stays absent rather than
// false.
//
// Parameters of `typeHierarchy/supertypes` and `typeHierarchy/subtypes`:
// `TypeHierarchyItem` and `optional<TypeHierarchyItem>`, per the protocol.
// Identical for the two methods, so one struct serves both; the method string
// is what distinguishes them.
struct TypeHierarchyItemParams {
  TypeHierarchyItem item;
  MAKE_SWAP_METHOD(TypeHierarchyItemParams, item)
};
MAKE_REFLECT_STRUCT(TypeHierarchyItemParams, item)

// Replaces the vendored `td_typeHierarchy` under its own name, so a reader
// sees the corrected signature next to the one it shadows.
DEFINE_REQUEST_RESPONSE_TYPE(td_typeHierarchyPrepare, TypeHierarchyParams,
                             optional<std::vector<TypeHierarchyItem>>,
                             "textDocument/typeHierarchy")

DEFINE_REQUEST_RESPONSE_TYPE(td_typeHierarchySupertypes,
                             TypeHierarchyItemParams,
                             optional<std::vector<TypeHierarchyItem>>,
                             "typeHierarchy/supertypes")

DEFINE_REQUEST_RESPONSE_TYPE(td_typeHierarchySubtypes, TypeHierarchyItemParams,
                             optional<std::vector<TypeHierarchyItem>>,
                             "typeHierarchy/subtypes")
