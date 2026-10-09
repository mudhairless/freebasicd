/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fbc_diagnostics.h"

#include <string>
#include <string_view>

namespace fblang {
namespace {

// The generated arrays and their counts. Only this translation unit includes
// the data; every reader goes through the functions below.
#include "fbc_diagnostics.inc"

FbcCatalogEntry const *tableFor(FbcMessageKind kind) {
  return kind == FbcMessageKind::Error ? kFbcErrors : kFbcWarnings;
}

int countFor(FbcMessageKind kind) {
  return kind == FbcMessageKind::Error ? kFbcErrorCount : kFbcWarningCount;
}

} // namespace

FbcCatalogEntry const *fbcMessage(FbcMessageKind kind, int number) {
  int const count = countFor(kind);
  if (number < 1 || number > count) {
    return nullptr;
  }
  return &tableFor(kind)[number - 1];
}

int fbcMessageCount(FbcMessageKind kind) { return countFor(kind); }

std::string fbcCode(FbcMessageKind kind, int number) {
  if (fbcMessage(kind, number) == nullptr) {
    return {};
  }
  return std::string("fbc ") +
         (kind == FbcMessageKind::Error ? "error: " : "warning: ") +
         std::to_string(number);
}

std::string_view fbcMessageDocsUrl() {
  return "https://www.freebasic.net/wiki/CompilerErrMsg";
}

} // namespace fblang
