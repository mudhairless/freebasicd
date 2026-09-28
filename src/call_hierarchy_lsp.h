/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "LibLsp/JsonRpc/RequestInMessage.h"
#include "LibLsp/lsp/lsRange.h"
#include "LibLsp/lsp/textDocument/callHierarchy.h"

// LspCpp's `td_outgoingCalls` is registered under the wire name
// "callHierarchy/CallHierarchyOutgoingCall" — the response *struct's* name
// where the protocol says "callHierarchy/outgoingCalls" — and the vendored copy
// has been wrong since 2019, untouched upstream. Its params and result types
// are right, so only the method name is redefined here (the
// `semantic_tokens_lsp.h` precedent, and the same reason the fork carries no
// commit for it): a client asking for the method the protocol names gets an
// answer.
//
// `td_prepareCallHierarchy` and `td_incomingCalls` are registered correctly and
// are used as shipped.
DEFINE_REQUEST_RESPONSE_TYPE(td_callHierarchyOutgoingCalls,
                             CallHierarchyOutgoingCallsParams,
                             optional<std::vector<CallHierarchyOutgoingCall>>,
                             "callHierarchy/outgoingCalls")
