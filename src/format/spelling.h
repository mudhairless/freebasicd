/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <string>
#include <string_view>

namespace fbformat {

// The documented spelling of the catalog word `wordLower` (lowercase, no type
// suffix) when it differs from plain TitleCase — "elseif" -> "ElseIf", "cint"
// -> "CInt", "lbound" -> "LBound" — or "" when TitleCase is the answer.
//
// The table is formatter data today (CaseStyle::Canonical is its only
// consumer); promote it to language.{h,cpp} if another consumer appears.
// Provenance and the regeneration procedure are documented in spelling.cpp.
std::string_view canonicalSpelling(std::string_view wordLower);

// First letter uppercase, the rest lowercase ("ELSEIF" -> "Elseif").
std::string titleCase(std::string_view word);

} // namespace fbformat
