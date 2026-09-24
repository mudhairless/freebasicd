#!/usr/bin/env bash
# FreeBASIC Language Server
# Copyright (C) 2026 Ebben Feagan
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Apply the GPL-3.0-or-later header to every first-party source file.
#
#   tools/license_headers.sh [--check] [root]
#
# Idempotent: a file already carrying the SPDX marker below is left untouched,
# so this can be re-run after a new source file is added. Corpus data
# (tests/corpus/*.bas, *.diag) and vendored code (third_party/) are out of
# scope by design. --check exits non-zero when any file lacks the header.
#
# The single source of truth for the header text is LICENSE.md; keep them in
# sync when the copyright line changes.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CHECK=0
if [[ ${1:-} == --check ]]; then
  CHECK=1
  shift
fi
if [[ -n ${1:-} ]]; then
  ROOT="$(cd "$1" && pwd)"
fi

C_HDR='/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */'

CMAKE_HDR='# FreeBASIC Language Server
# Copyright (C) 2026 Ebben Feagan
# SPDX-License-Identifier: GPL-3.0-or-later'

missing=0

apply() {
  local f="$1" hdr="$2"
  if grep -q 'SPDX-License-Identifier: GPL-3.0-or-later' "$f"; then
    return
  fi
  if ((CHECK)); then
    echo "missing header: $f" >&2
    missing=1
    return
  fi
  local tmp
  tmp="$(mktemp)"
  {
    printf '%s\n\n' "$hdr"
    cat "$f"
  } >"$tmp"
  # Write through the original inode so file mode/permissions are kept.
  cat "$tmp" >"$f"
  rm -f "$tmp"
  echo "header added: $f"
}

# C-family sources: src/, tests/, tools/.
while IFS= read -r -d '' f; do
  apply "$f" "$C_HDR"
done < <(
  find "$ROOT/src" "$ROOT/tests" "$ROOT/tools" \
    -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) -print0
)

# Root build file.
apply "$ROOT/CMakeLists.txt" "$CMAKE_HDR"

exit "$missing"