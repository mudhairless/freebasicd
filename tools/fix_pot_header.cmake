# FreeBASIC Language Server
# Copyright (C) 2026 Ebben Feagan
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Post-process the pot extracted by the po-template target so the committed
# template header carries the project identity declared in LICENSE.md and the
# source-file headers (project name, copyright holder, license, maintainer
# contact). xgettext can only take --package-name / --copyright-holder /
# --msgid-bugs-address; the descriptive title, the "FIRST AUTHOR" line, the
# fuzzy header marker, the Last-Translator / Language-Team / charset
# placeholders, and the revision date are fixed here. Idempotent: every
# substitution below targets a placeholder that only raw xgettext output
# contains, so re-running po-template reproduces the committed file exactly.
set(_pot "${CMAKE_SOURCE_DIR}/po/freebasiclsp.pot")
file(READ "${_pot}" _content)

string(REPLACE "# SOME DESCRIPTIVE TITLE."
  "# FreeBASIC Language Server translations." _content "${_content}")
string(REPLACE "# Copyright (C) YEAR Ebben Feagan"
  "# Copyright (C) 2026 Ebben Feagan" _content "${_content}")
string(REPLACE
  "# This file is distributed under the same license as the freebasiclsp package."
  "# This file is distributed under the same license as the freebasiclsp package (GPL-3.0-or-later)."
  _content "${_content}")
string(REPLACE "# FIRST AUTHOR <EMAIL@ADDRESS>, YEAR."
  "# Ebben Feagan <ebben.feagan@gmail.com>, 2026." _content "${_content}")
string(REPLACE "#, fuzzy\n" "" _content "${_content}")

# Stamp PO-Revision-Date with the same extraction time xgettext recorded
# (xgettext derives POT-Creation-Date from the newest input's mtime, which
# keeps the committed pot reproducible between reruns). Reusing it keeps the
# header internally consistent and the whole target idempotent.
string(REGEX MATCH
  "[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9] [0-9][0-9]:[0-9][0-9][+-][0-9][0-9][0-9][0-9]"
  _when "${_content}")
string(REPLACE "\"PO-Revision-Date: YEAR-MO-DA HO:MI+ZONE\\n\""
  "\"PO-Revision-Date: ${_when}\\n\"" _content "${_content}")

string(REPLACE "\"Last-Translator: FULL NAME <EMAIL@ADDRESS>\\n\""
  "\"Last-Translator: Ebben Feagan <ebben.feagan@gmail.com>\\n\"" _content
  "${_content}")
string(REPLACE "\"Language-Team: LANGUAGE <LL@li.org>\\n\""
  "\"Language-Team: Ebben Feagan <ebben.feagan@gmail.com>\\n\"" _content
  "${_content}")
string(REPLACE "charset=CHARSET" "charset=UTF-8" _content "${_content}")

file(WRITE "${_pot}" "${_content}")