#pragma once

#include "LibLsp/JsonRpc/RequestInMessage.h"
#include "LibLsp/lsp/lsRange.h"
#include "LibLsp/lsp/lsTextDocumentIdentifier.h"
#include "LibLsp/lsp/textDocument/SemanticTokens.h"

// `textDocument/semanticTokens/range` is a standard LSP 3.17 request that the
// vendored LspCpp tree ships no generated request type for. Define it here —
// the documented custom-protocol pattern: `LanguageSession::on` installs both
// the handler and the JSON parser for unregistered method names.
struct SemanticTokensRangeParams {
  lsTextDocumentIdentifier textDocument;
  lsRange range;
  MAKE_SWAP_METHOD(SemanticTokensRangeParams, textDocument, range)
};
MAKE_REFLECT_STRUCT(SemanticTokensRangeParams, textDocument, range)

DEFINE_REQUEST_RESPONSE_TYPE(td_semanticTokens_range, SemanticTokensRangeParams,
                             optional<SemanticTokens>,
                             "textDocument/semanticTokens/range")
